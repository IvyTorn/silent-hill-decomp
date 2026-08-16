/*
 * gpu_rdp.c - the GpuNv2a_* interface on the N64's RDP.
 *
 * "Nv2a" is a misnomer everywhere but the Xbox: the name is the shared console
 * GPU interface that gpu_xbox.c / psx_libgpu_xbox.c drive, and the PSP port
 * implements the same set on the GE. This is the RDP's turn.
 *
 * MILESTONE 2b SCOPE. Frame begin/end own the display and clear, and that is
 * all that draws. EmitTris counts and discards. That is deliberate and stated
 * rather than hidden: the geometry arriving here is screen-space PSX primitives
 * with 4bpp/8bpp CLUT textures, and turning those into RDP tiles is milestone
 * 3's whole job -- TMEM is 4 KB against the PSX's 1 MB VRAM, so it needs a real
 * texture cache and an OT sort by texture, not a quick rdpq_triangle call here.
 * Until then the counters below are the instrument that says how much work a
 * frame actually carries.
 *
 * One thing to know before writing any of that: rdpq calls are QUEUED, and the
 * graphics_* helpers are immediate CPU writes into the surface. Mixing them
 * without draining first means the queue lands on top of the CPU pixels
 * whenever it happens to run. Use rdpq_detach_wait() before any CPU drawing,
 * not rdpq_detach_show().
 */
#include <libdragon.h>

#include "gpu_nv2a.h"
#include "sh_log.h"
#include "sh_log_n64.h"

/* Matches display_init below. The README's memory budget assumes this: at
 * 16bpp double-buffered plus a Z buffer it is ~450 KB of RDRAM, and raising it
 * is not free on a machine with 8 MB. */
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
static surface_t  s_zbuf;
static int        s_inited;
static int        s_trisThisFrame;
static unsigned long long s_frameStart;

void GpuNv2a_Init(void)
{
    if (s_inited)
        return;

    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    s_zbuf = surface_alloc(FMT_RGBA16, SCR_W, SCR_H);
    rdpq_init();
    s_inited = 1;

    SH_DBG("[GPU] rdp up: %dx%d 16bpp x2 + z", SCR_W, SCR_H);
}

void GpuNv2a_FrameBegin(void)
{
    unsigned int clear;

    if (!s_inited)
        GpuNv2a_Init();

    s_frameStart = get_ticks();
    s_trisThisFrame = 0;

    s_fb = display_get();
    rdpq_attach_clear(s_fb, &s_zbuf);

    /* The PSX draw-env isbg background -- the fog colour in-game. Taking it
     * from gpu_xbox.c rather than picking one here is what keeps a map's fog
     * and the frame clear the same colour; the PC port has a whole class of
     * one-frame white flashes from getting this wrong. */
    clear = GpuXbox_GetClearColor();
    rdpq_set_mode_fill(RGBA32((clear >> 16) & 0xFF, (clear >> 8) & 0xFF, clear & 0xFF, 0xFF));
    rdpq_fill_rectangle(0, 0, SCR_W, SCR_H);
}

void GpuNv2a_FrameEnd(void)
{
    if (!s_inited || s_fb == NULL)
        return;

    if (ShLogN64_ScreenEnabled())
    {
        /* detach_WAIT, not detach_show: graphics_draw_text is an immediate CPU
         * write into the surface while the frame's fills and triangles are
         * QUEUED RDP commands. Painting before the queue drains puts the text
         * underneath whatever runs next. */
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
        SH_DBG("[GPU] frame %d: %d tris offered, %d us",
               g_Nv2aFrameCount, s_trisThisFrame,
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

/* Screen freeze: the pause/save/inventory backgrounds hold the last gameplay
 * frame while a menu draws over it. Not available yet -- it is the readback
 * path under another name, and that waits on milestone 3 for the same reason
 * (the framebuffer here is 16-bit and the callers expect A8R8G8B8). */
void GpuNv2a_FreezeCapture(void) { }
void GpuNv2a_FreezeBlit(void) { }
void GpuNv2a_FreezeRelease(void) { }

/* --------------------------------------------------------- not yet drawing */

void GpuNv2a_EmitTris(const ShVertex* verts, int count)
{
    (void)verts;
    s_trisThisFrame += count / 3;
}

/* ShVertex is 64 bytes, so this array is its count x 64. 4096 would be 256 KB
 * of a machine that has 8 MB -- the Xbox's batch sizing does not transfer.
 * 1024 is 64 KB and still more primitives than a 320x240 frame can rasterise
 * in a 30 Hz budget. */
static ShVertex s_batch[1024];

ShVertex* GpuNv2a_BatchAlloc(int count)
{
    if (count <= 0 || count > (int)(sizeof(s_batch) / sizeof(s_batch[0])))
        return NULL;
    return s_batch;
}

void  GpuNv2a_BindTexture(const void* addr, int w, int h) { (void)addr; (void)w; (void)h; }
void  GpuNv2a_BindPaletted(const void* page, const void* pal) { (void)page; (void)pal; }
void  GpuNv2a_BindWhite(void) { }
void  GpuNv2a_SetPaletteDmaVariant(int variant) { (void)variant; }
void  GpuNv2a_SetBlendMode(int mode) { (void)mode; }
void  GpuNv2a_SetDepthTest(int enable) { (void)enable; }
void  GpuNv2a_SetDepthWrite(int enable) { (void)enable; }
void  GpuNv2a_SetScissor(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }

/* Texture memory. Returns NULL rather than a heap block: a caller that gets a
 * pointer here will fill it and expect the texture to appear, and nothing yet
 * uploads to TMEM. Failing honestly keeps that a visible NULL check instead of
 * an invisible wrong picture. */
void* GpuNv2a_AllocTexMem(int bytes) { (void)bytes; return NULL; }

/* Framebuffer readback for the screen-grab effects (pause/save backgrounds,
 * air-screamer distortion, StoreImage). The N64 framebuffer IS in RDRAM and
 * directly addressable, so this becomes real cheaply -- but it must report the
 * true 16-bit format, and the callers expect A8R8G8B8. Converting a whole frame
 * per grab is milestone 3's problem, so this says "no surface" for now. */
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
