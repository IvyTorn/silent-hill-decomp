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

#include <math.h>
#include <stdlib.h>

#include "gpu_nv2a.h"
#include "sh_log.h"
#include "sh_log_n64.h"

/* 0 while the port has nothing worth looking at. The on-screen log is the only
 * diagnostic channel a TV or a headless emulator run has, and geometry starts
 * appearing long before the game reaches anything worth seeing. */
#define SH_N64_LOG_HIDE_ON_FIRST_TRI 0

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

/* --------------------------------------------------------------- texture */

/* psx_vram.c hands out exactly what the RDP's TLUT hardware wants: a 256x256
 * page of 8-bit indices (CI8) and a 256-entry A8R8G8B8 palette. The only
 * conversion needed is the palette to RGBA16, once per bind.
 *
 * TMEM IS 4 KB, and a 256-entry TLUT occupies half of it, so a CI8 tile gets
 * 2048 bytes. That is the whole difficulty of this port's renderer: the PSX
 * sampled a 1 MB VRAM freely, and here every primitive's texture has to be
 * DMA'd in as a sub-rectangle small enough to fit. 64x32 is the largest tile
 * that fits, so a triangle whose UV span exceeds it falls back to flat shade
 * rather than drawing with wrong texels -- an honest miss is easier to see and
 * to count than a subtly wrong picture. */
#define TEX_PAGE_DIM  256
#define TEX_TILE_W    64
#define TEX_TILE_H    32

static const uint8_t*  s_texPage;
static const uint32_t* s_texPal;
static uint16_t        s_tlut[256];
static int             s_tlutDirty;
static surface_t       s_pageSurf;

/* Census, so a frame that draws nothing can say why. */
static int s_cnTris;
static int s_cnDropped;
static int s_cnTexTris;
static int s_cnTexTooBig;

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
    if (s_texEnabled && s_texPage != NULL)
    {
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
        rdpq_mode_tlut(TLUT_RGBA16);
    }
    else
    {
        rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
        rdpq_mode_tlut(TLUT_NONE);
    }

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

static void BuildTlut(void)
{
    int i;

    for (i = 0; i < 256; i++)
    {
        uint32_t c = s_texPal[i];             /* A8R8G8B8 */
        s_tlut[i] = (uint16_t)((((c >> 16) & 0xF8) << 8) |
                               (((c >> 8)  & 0xF8) << 3) |
                               (((c)       & 0xF8) >> 2) |
                               ((c >> 31) & 1));
    }
}

/* rdpq wants S, T and W contiguous, and ShVertex keeps W up in pos[3], so a
 * textured vertex cannot be passed through the way an untextured one is. Ten
 * floats: X Y R G B A S T W. */
static void StageTexVert(float* out, const ShVertex* v)
{
    out[0] = v->pos[0];
    out[1] = v->pos[1];
    out[2] = v->col[0];
    out[3] = v->col[1];
    out[4] = v->col[2];
    out[5] = v->col[3];
    /* UVs arrive normalised: gpu_xbox.c pre-multiplies by PAL_UV_SCALE (1/256)
     * for the NV2A, which wants 0..1. The RDP wants texels. */
    out[6] = v->tex[0] * (float)TEX_PAGE_DIM;
    out[7] = v->tex[1] * (float)TEX_PAGE_DIM;
    out[8] = v->pos[3];
}

static const rdpq_trifmt_t TRIFMT_SH_TEX = {
    .pos_offset   = 0,
    .shade_offset = 2,
    .tex_offset   = 6,
    .tex_tile     = TILE0,
    .z_offset     = -1,
};

static void DrawTexturedTri(const ShVertex* a, const ShVertex* b, const ShVertex* c)
{
    float va[9], vb[9], vc[9];
    int   s0, t0, s1, t1;

    StageTexVert(va, a);
    StageTexVert(vb, b);
    StageTexVert(vc, c);

    /* The sub-rectangle this triangle actually samples. Floor/ceil rather than
     * round: a bilinear tap reaches one texel past the corner, and a tile that
     * is one short shows as a bright seam along the edge. */
    s0 = (int)floorf(fminf(va[6], fminf(vb[6], vc[6])));
    t0 = (int)floorf(fminf(va[7], fminf(vb[7], vc[7])));
    s1 = (int)ceilf (fmaxf(va[6], fmaxf(vb[6], vc[6]))) + 1;
    t1 = (int)ceilf (fmaxf(va[7], fmaxf(vb[7], vc[7]))) + 1;

    if (s0 < 0) s0 = 0;
    if (t0 < 0) t0 = 0;
    if (s1 > TEX_PAGE_DIM) s1 = TEX_PAGE_DIM;
    if (t1 > TEX_PAGE_DIM) t1 = TEX_PAGE_DIM;

    if (s1 <= s0 || t1 <= t0)
        return;

    if ((s1 - s0) > TEX_TILE_W || (t1 - t0) > TEX_TILE_H)
    {
        /* Does not fit TMEM. Draw it flat rather than with wrong texels: a
         * missing texture is visible and countable, a wrong one is neither.
         *
         * The mode is restored IMMEDIATELY, not deferred through s_modeDirty:
         * ApplyMode only runs at the top of a flush, so a deferred restore
         * would leave every following triangle in the same run drawing
         * untextured too. */
        s_cnTexTooBig++;
        rdpq_mode_tlut(TLUT_NONE);
        rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
        rdpq_triangle(&TRIFMT_SH_SHADE, (const float*)a, (const float*)b, (const float*)c);
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
        rdpq_mode_tlut(TLUT_RGBA16);
        s_cnTris++;
        return;
    }

    rdpq_tex_upload_sub(TILE0, &s_pageSurf, NULL, s0, t0, s1, t1);
    rdpq_triangle(&TRIFMT_SH_TEX, va, vb, vc);
    s_cnTris++;
    s_cnTexTris++;
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

    if (s_texEnabled && s_texPage != NULL)
    {
        if (s_tlutDirty)
        {
            BuildTlut();
            rdpq_tex_upload_tlut(s_tlut, 0, 256);
            s_tlutDirty = 0;
        }

        for (i = s_runStart; i + 2 < s_batchUsed; i += 3)
            DrawTexturedTri(&s_batch[i], &s_batch[i + 1], &s_batch[i + 2]);
    }
    else
    {
        for (i = s_runStart; i + 2 < s_batchUsed; i += 3)
        {
            rdpq_triangle(&TRIFMT_SH_SHADE,
                          (const float*)&s_batch[i],
                          (const float*)&s_batch[i + 1],
                          (const float*)&s_batch[i + 2]);
            s_cnTris++;
        }
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
    s_cnTexTris  = 0;
    s_cnTexTooBig = 0;
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
    /* The on-screen log used to stand down as soon as the first triangle was
     * drawn, on the reasoning that it exists for when nothing is drawing. That
     * was wrong for this port's actual state: geometry appears long before the
     * game reaches anything worth looking at, and hiding the log took away the
     * only diagnostic channel a headless emulator run or a TV has.
     *
     * It stays up until something explicitly turns it off. Flip this when there
     * is a picture worth seeing. */
    if (SH_N64_LOG_HIDE_ON_FIRST_TRI && s_cnTris > 0 && ShLogN64_ScreenEnabled())
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
        SH_DBG("[GPU] f%d tris=%d tex=%d big=%d drop=%d %dus",
               g_Nv2aFrameCount, s_cnTris, s_cnTexTris, s_cnTexTooBig, s_cnDropped,
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
    s_texPage    = NULL;
    s_texPal     = NULL;
    s_modeDirty  = 1;
}

/* The paletted path, which is where almost everything the game draws arrives:
 * psx_vram.c decodes a PSX tpage into a 256x256 CI8 index page plus a 256-entry
 * A8R8G8B8 palette, and those map onto the RDP's own CI8 + TLUT with no
 * repacking. */
void GpuNv2a_BindPaletted(const void* page, const void* pal)
{
    if (page == s_texPage && pal == s_texPal && s_texEnabled)
        return;

    GpuNv2a_FlushBatch();

    if (page == NULL || pal == NULL)
    {
        s_texEnabled = 0;
        s_texPage    = NULL;
        s_texPal     = NULL;
    }
    else
    {
        if (pal != s_texPal)
            s_tlutDirty = 1;
        s_texPage    = (const uint8_t*)page;
        s_texPal     = (const uint32_t*)pal;
        s_pageSurf   = surface_make_linear((void*)page, FMT_CI8, TEX_PAGE_DIM, TEX_PAGE_DIM);
        s_texEnabled = 1;
    }
    s_modeDirty = 1;
}

/* The NON-paletted path: gpu_xbox.c's pre-decoded RGBA images (the minimap and
 * the hi-res chunk pool). Not wired -- those are 32-bit and would need a
 * different TMEM budget again, and nothing on the critical path uses them.
 * Reverting to untextured is honest; drawing them with the previous page's
 * texels would not be. */
void GpuNv2a_BindTexture(const void* addr, int w, int h)
{
    (void)addr; (void)w; (void)h;
    GpuNv2a_BindPaletted(NULL, NULL);
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

/* Decoded-page and palette storage for psx_vram.c's cache. 8-byte aligned: the
 * RDP's texture DMA wants it, and an unaligned page would make every
 * rdpq_tex_upload_sub of it a slow path. Sized by PAGE_N/PAL_N over there,
 * which this port cuts to 4 pages and 16 palettes -- the Xbox's 64 pages is
 * 4 MB, which is half this machine. */
void* GpuNv2a_AllocTexMem(int bytes)
{
    if (bytes <= 0)
        return NULL;
    return memalign(8, (size_t)bytes);
}

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
