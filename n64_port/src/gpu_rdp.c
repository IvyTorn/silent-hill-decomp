/*
 * gpu_rdp.c - the GpuNv2a_* interface on the N64's RDP.
 *
 * "Nv2a" is a misnomer everywhere but the Xbox: the name is the shared console
 * GPU interface that gpu_xbox.c / psx_libgpu_xbox.c drive, and the PSP port
 * implements the same set on the GE. This is the RDP's turn.
 *
 * THE MODEL, which is gpu_xbox.c's and not ours to change: the caller asks for
 * n vertices with GpuNv2a_BatchAlloc(n) and writes into the returned pointer
 * directly. There is no matching "submit" call. A run accumulates until some
 * piece of state changes -- texture, blend, scissor -- and the state setter
 * flushes the run that was drawn under the OLD state before adopting the new
 * one. GpuNv2a_EmitTris exists but gpu_xbox.c barely uses it.
 *
 * NO Z-BUFFER, deliberately. The PSX had none: ordering is painter's, from the
 * OT walk, and the OT arrives here already sorted. Adding depth would not just
 * waste 150 KB of a machine with 8 MB, it would be WRONG -- the game leans on
 * submission order for semitransparent draws, and a depth test would start
 * rejecting fragments the PSX drew.
 *
 * ShVertex is laid out {float pos[4]; col[4]; tex[2]; spec[4]; pad[2]}, which is
 * exactly what rdpq_triangle wants: pos at offset 0, shade at 4. Untextured
 * triangles are passed straight through with no conversion at all. Textured
 * ones cannot be, because rdpq wants S,T,W contiguous and ShVertex has W up in
 * pos[3]; those stage through a small array.
 *
 * One thing to know before extending this: rdpq calls are QUEUED, and the
 * graphics_* helpers are immediate CPU writes into the surface. Mixing them
 * without draining first means the queue lands on top of the CPU pixels
 * whenever it happens to run. Use rdpq_detach_wait() before any CPU drawing,
 * not rdpq_detach_show().
 */
#include <libdragon.h>

#include "gpu_nv2a.h"
#include "sh_log.h"
#include "sh_log_n64.h"

/* Matches display_init below. The README's memory budget assumes this. */
#define SCR_W 320
#define SCR_H 240

int g_Nv2aFbW      = SCR_W;
int g_Nv2aFbH      = SCR_H;
int g_Nv2aContentW = SCR_W;
int g_Nv2aContentH = SCR_H;
int g_Nv2aContentX = 0;
int g_Nv2aFrameCount = 0;
int g_Nv2aDrawCycles = 0;

static surface_t* s_fb;
static int        s_inited;
static unsigned long long s_frameStart;

/* --------------------------------------------------------------- batch */

/* 1024 vertices is 64 KB at ShVertex's 64-byte stride. It does NOT have to hold
 * a whole frame -- BatchAlloc flushes when it fills -- so this is a staging
 * buffer, not a frame buffer. The Xbox's 4096 would be 256 KB for no benefit. */
#define MAX_BATCH_VERTS 1024

static ShVertex s_batch[MAX_BATCH_VERTS];
static int      s_batchUsed;
static int      s_runStart;

/* Current draw state. s_curBlend is the shared encoding: 0 opaque, else
 * 1 + PSX abr (0 average, 1 additive, 2 subtractive, 3 quarter-additive). */
static int s_curBlend = -1;
static int s_texEnabled;
static int s_modeDirty = 1;

/* Census, so a frame that draws nothing can say why. */
static int s_cnTris;
static int s_cnDropped;

static const rdpq_trifmt_t TRIFMT_SH_SHADE = {
    .pos_offset   = 0,
    .shade_offset = 4,
    .tex_offset   = -1,
    .z_offset     = -1,
};

static void ApplyMode(void)
{
    if (!s_modeDirty)
        return;
    s_modeDirty = 0;

    rdpq_set_mode_standard();
    rdpq_mode_combiner(s_texEnabled ? RDPQ_COMBINER_TEX_SHADE : RDPQ_COMBINER_SHADE);

    switch (s_curBlend)
    {
        case 0:  /* opaque */
            rdpq_mode_blender(0);
            break;
        case 2:  /* PSX abr 1: B + F */
        case 4:  /* PSX abr 3: B + F/4 -- additive is the closest the RDP has */
            rdpq_mode_blender(RDPQ_BLENDER_ADDITIVE);
            break;
        case 1:  /* PSX abr 0: (B + F)/2. gpu_xbox.c already sets col[3]=0.5 */
        case 3:  /* PSX abr 2: B - F. The RDP blender cannot subtract; an alpha
                  * blend darkens rather than inverts, which is wrong but far
                  * closer than drawing it opaque. */
        default:
            rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
            break;
    }
}

/* Draw everything accumulated since the last flush under the state that was
 * current while it was written. */
void GpuNv2a_FlushBatch(void)
{
    int i;

    if (s_batchUsed <= s_runStart)
    {
        s_runStart = s_batchUsed;
        return;
    }

    ApplyMode();

    /* Untextured only for now. Texture binding is recorded but nothing is
     * uploaded to TMEM yet, so a textured run draws with its shade colour --
     * flat-lit geometry rather than an invisible one. */
    for (i = s_runStart; i + 2 < s_batchUsed; i += 3)
    {
        rdpq_triangle(&TRIFMT_SH_SHADE,
                      (const float*)&s_batch[i],
                      (const float*)&s_batch[i + 1],
                      (const float*)&s_batch[i + 2]);
        s_cnTris++;
    }

    s_runStart = s_batchUsed;
}

ShVertex* GpuNv2a_BatchAlloc(int count)
{
    ShVertex* p;

    if (count <= 0 || count > MAX_BATCH_VERTS)
    {
        s_cnDropped++;
        return NULL;
    }

    if (s_batchUsed + count > MAX_BATCH_VERTS)
    {
        /* Flush the pending run before its source is overwritten. rdpq has
         * already consumed the vertices into the command queue by then, so
         * there is nothing to drain. */
        GpuNv2a_FlushBatch();
        s_batchUsed = 0;
        s_runStart  = 0;
    }

    p = s_batch + s_batchUsed;
    s_batchUsed += count;
    return p;
}

void GpuNv2a_EmitTris(const ShVertex* verts, int count)
{
    /* gpu_xbox.c writes through BatchAlloc and almost never calls this. When it
     * does, the vertices are already somewhere else, so they have to be copied
     * into the batch to join the current run. */
    ShVertex* d;
    int       i;

    if (verts == NULL || count <= 0)
        return;

    d = GpuNv2a_BatchAlloc(count);
    if (d == NULL)
        return;

    for (i = 0; i < count; i++)
        d[i] = verts[i];
}

/* --------------------------------------------------------------- frame */

void GpuNv2a_Init(void)
{
    if (s_inited)
        return;

    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    rdpq_init();
    s_inited = 1;

    SH_DBG("[GPU] rdp up: %dx%d 16bpp x2, no z (painter's order from the OT)", SCR_W, SCR_H);
}

void GpuNv2a_FrameBegin(void)
{
    unsigned int clear;

    if (!s_inited)
        GpuNv2a_Init();

    s_frameStart = get_ticks();
    s_batchUsed  = 0;
    s_runStart   = 0;
    s_cnTris     = 0;
    s_cnDropped  = 0;
    s_modeDirty  = 1;

    s_fb = display_get();
    rdpq_attach(s_fb, NULL);

    /* The PSX draw-env isbg background -- the fog colour in-game. Taking it
     * from gpu_xbox.c rather than picking one here is what keeps a map's fog
     * and the frame clear the same colour. */
    clear = GpuXbox_GetClearColor();
    rdpq_set_mode_fill(RGBA32((clear >> 16) & 0xFF, (clear >> 8) & 0xFF, clear & 0xFF, 0xFF));
    rdpq_fill_rectangle(0, 0, SCR_W, SCR_H);
}

void GpuNv2a_FrameEnd(void)
{
    if (!s_inited || s_fb == NULL)
        return;

    GpuNv2a_FlushBatch();

    /* The on-screen log exists for when nothing is drawing. The moment the
     * renderer produces a triangle it has done its job and is only in the way,
     * so it stands down permanently -- SH_DBG still reaches IS-Viewer and USB. */
    if (s_cnTris > 0 && ShLogN64_ScreenEnabled())
    {
        ShLogN64_ScreenEnable(0);
        SH_DBG("[GPU] first geometry drawn; on-screen log off");
    }

    if (ShLogN64_ScreenEnabled())
    {
        int rows = ShLogN64_Rows();
        int i;

        rdpq_detach_wait();
        graphics_set_color(0xFFFFFFFF, 0);
        for (i = 0; i < rows; i++)
            graphics_draw_text(s_fb, 4, 4 + (i * 9), ShLogN64_Row(i));
        display_show(s_fb);
    }
    else
    {
        rdpq_detach_show();
    }
    s_fb = NULL;

    g_Nv2aFrameCount++;
    g_Nv2aDrawCycles = (int)(get_ticks() - s_frameStart);

    if ((g_Nv2aFrameCount & 63) == 0)
        SH_DBG("[GPU] f%d tris=%d drop=%d %dus",
               g_Nv2aFrameCount, s_cnTris, s_cnDropped,
               (int)TICKS_TO_US((unsigned)g_Nv2aDrawCycles));
}

void GpuNv2a_WaitVbl(void)
{
    vi_wait_vblank();
}

void GpuNv2a_DrainGpu(void)
{
    rspq_wait();
}

/* --------------------------------------------------------------- state */

void GpuNv2a_SetBlendMode(int mode)
{
    if (mode == s_curBlend)
        return;
    GpuNv2a_FlushBatch();
    s_curBlend  = mode;
    s_modeDirty = 1;
}

/* Untextured: the combiner drops the texture stage and the vertex colour passes
 * through. No 1x1-white texture needed -- that was an NV2A workaround for its
 * combiner always sampling stage 0. */
void GpuNv2a_BindWhite(void)
{
    if (!s_texEnabled)
        return;
    GpuNv2a_FlushBatch();
    s_texEnabled = 0;
    s_modeDirty  = 1;
}

/* Texture binding is RECORDED but nothing reaches TMEM yet. TMEM is 4 KB
 * against the PSX's 1 MB VRAM, so every texture change is a DMA and this needs
 * a real cache plus an OT sort by texture -- the next piece of work, and too
 * big to fake here. Flushing on the change keeps the run boundaries correct so
 * that when uploads land, the batching around them is already right. */
void GpuNv2a_BindTexture(const void* addr, int w, int h)
{
    (void)addr; (void)w; (void)h;
    if (s_texEnabled)
        return;
    GpuNv2a_FlushBatch();
    s_texEnabled = 0;   /* stays off until TMEM upload exists */
    s_modeDirty  = 1;
}

void GpuNv2a_BindPaletted(const void* page, const void* pal)
{
    (void)page; (void)pal;
    GpuNv2a_BindTexture(NULL, 0, 0);
}

void GpuNv2a_SetPaletteDmaVariant(int variant) { (void)variant; }

/* The PSX has no depth buffer and neither does this port: ordering is the OT's.
 * Accepting and ignoring these is not a stub, it is the correct behaviour. */
void GpuNv2a_SetDepthTest(int enable)  { (void)enable; }
void GpuNv2a_SetDepthWrite(int enable) { (void)enable; }

void GpuNv2a_SetScissor(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    GpuNv2a_FlushBatch();
    rdpq_set_scissor(x, y, x + w, y + h);
}

/* Texture memory. Returns NULL rather than a heap block: a caller that gets a
 * pointer here will fill it and expect the texture to appear. */
void* GpuNv2a_AllocTexMem(int bytes) { (void)bytes; return NULL; }

/* Screen freeze and framebuffer readback (pause/save backgrounds, air-screamer
 * distortion, StoreImage). The N64 framebuffer IS in RDRAM and directly
 * addressable, so these become real cheaply -- but the callers expect
 * A8R8G8B8 and this surface is 16-bit, so it needs a conversion pass. */
void GpuNv2a_FreezeCapture(void) { }
void GpuNv2a_FreezeBlit(void) { }
void GpuNv2a_FreezeRelease(void) { }

const void* GpuNv2a_ReadbackSurface(int fromLastQueued, int* w, int* h, int* pitchBytes)
{
    (void)fromLastQueued;
    if (w) *w = 0;
    if (h) *h = 0;
    if (pitchBytes) *pitchBytes = 0;
    return NULL;
}

int GpuNv2a_Ms(void)
{
    return (int)(get_ticks_ms() & 0x7FFFFFFF);
}
