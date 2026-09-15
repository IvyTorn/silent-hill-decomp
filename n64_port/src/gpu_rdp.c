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
#include "pc_config.h"    /* g_PcConfig.n64ClipScreen */
#include "sh_log.h"
#include "sh_log_n64.h"
#include "sh_t3d.h"

/* 0 while the port has nothing worth looking at. The on-screen log is the only
 * diagnostic channel a TV or a headless emulator run has, and geometry starts
 * appearing long before the game reaches anything worth seeing. */
#define SH_N64_LOG_HIDE_ON_FIRST_TRI 1

/* Matches display_init below. The README's memory budget assumes this. */
#define SCR_W 320
#define SCR_H 240

int g_Nv2aFbW      = SCR_W;
int g_Nv2aFbH      = SCR_H;
int g_Nv2aContentW = SCR_W;
int g_Nv2aContentH = SCR_H;
int g_Nv2aContentX = 0;
int g_Nv2aFrameCount = 0;
/* unsigned long long, matching gpu_xbox.c's extern and gpu_nv2a.c's definition.
 * As an int it was 4 bytes, gpu_xbox.c's per-frame `g_Nv2aDrawCycles = 0`
 * stored 8, and the next 4 were g_Nv2aFrameCount -- zeroed every frame. That
 * was the "frame counter stuck at 0" bug, and psx_vram.c keys its page LRU on
 * that counter: with every page's lastUse equal to a frozen frame number no
 * page was ever evictable, so every miss past the eighth page drained the
 * whole GPU queue and redecoded into the same slot. */
unsigned long long g_Nv2aDrawCycles = 0;
extern int g_ProfAudioMs;                 /* psx_libgpu_xbox.c: last audio pump */
extern int g_PsxVramDecodes, g_PsxVramDrains, g_PsxVramPalBuilds;
extern unsigned long long g_PsxVramDecodeTicks, g_PsxVramDrainTicks;
extern unsigned g_PsxVramPalGen;
/* world_draw.c: game-side draw phases, in SH_CYCLES ticks (== get_ticks here). */
extern unsigned long long g_XbChunkDrawCycles, g_XbCharaCycles;
extern int PsxVram_PageIs4bpp(const void* page);   /* psx_vram.c */
extern int VSync(int mode);                        /* psx_libgpu_xbox.c: -1 = read the counter */
extern int g_VBlanks;                              /* the game's own vblank delta */
/* The REAL timestep: game_main.c feeds MIN(GsGetVcount(), H_BLANKS_PER_FRAME_MIN)
 * into g_DeltaTime, so this -- not g_VBlanks -- is what decides how much game
 * time a frame advances, and the MIN is why a slow frame plays in slow motion. */
extern int GsGetVcount(void);
extern unsigned           g_XbCharaCount;
/* sh_log_n64.c: what logging cost this frame. */
extern unsigned long long g_ProfLogStderrTicks, g_ProfLogSdTicks;
extern int                g_ProfLogLines;

static surface_t* s_fb;
static int        s_inited;
static unsigned long long s_frameStart;

/* Z-buffer: 320x240x16 = 150KB, surface_alloc'd ONLY when zbuffer=1. It did
 * not fit while the heap ran 1152/1208KB with the world resident (a static
 * .bss buffer only shrinks the heap region -- same RAM); pc_chara_pool.c's
 * s_poolBoneCoords (205KB, dead on N64: globalCharaPool=0) was reclaimed to
 * make room, ~260KB free now. With Z live, t3d_world.c draws the opaque world
 * TILE-SORTED across blocks (each unique tile uploads once instead of ~2.6x)
 * and semitransparent geometry after it Z-tested-not-written, keeping PSX
 * blend order. zbuffer=0 (default) keeps the painter's path untouched. */
static surface_t s_zbuf;
static int       s_zbufTried;
static int ZBufOn(void) { return g_PcConfig.n64ZBuffer && s_zbuf.buffer != NULL; }
/* Public: t3d_world.c enables Z for the world only when the buffer is live. */
int GpuNv2a_ZBufActive(void) { return ZBufOn(); }
/* 0 = no Z; 1 = world writes/tests depth (tile-sorted path) and so does the
 * character; 2 = the character alone: his parts occlude each other by pixel
 * (the PSX sorted his polygons into the OT one by one, which per-PART
 * painter's order cannot reproduce -- the holstered gun showed through the
 * trousers), the world stays painter's and never touches the Z image. */
int GpuNv2a_ZBufMode(void) { return ZBufOn() ? g_PcConfig.n64ZBuffer : 0; }
/* Public: t3d_world.c reads the native-character switch through here so it
 * needs no config include of its own. */
int GpuNv2a_NativeCharaEnabled(void) { return g_PcConfig.n64NativeChara; }
int GpuNv2a_CharaCull(void) { return g_PcConfig.n64CharaCull; }
int GpuNv2a_WorldCull(void) { return g_PcConfig.n64WorldCull; }
int GpuNv2a_WorldAa(void) { return g_PcConfig.n64WorldAa; }
int GpuNv2a_CharaDebug(void) { return g_PcConfig.n64CharaDebug; }
int GpuNv2a_CharaXpose(void) { return g_PcConfig.n64CharaXpose; }
int GpuNv2a_MemWatch(void) { return g_PcConfig.n64MemWatch; }

/* --------------------------------------------------------------- batch */

/* 1024 vertices is 64 KB at ShVertex's 64-byte stride. It does NOT have to hold
 * a whole frame -- BatchAlloc flushes when it fills -- so this is a staging
 * buffer, not a frame buffer. The Xbox's 4096 would be 256 KB for no benefit. */
/* 1024 -> 512 with the native world renderer: the batch now carries only
 * characters, effects and 2D (the world bypasses it), and 32KB of .bss goes
 * back to the heap. A fuller batch just flushes more often. */
#define MAX_BATCH_VERTS 512

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
/* The texel budget is TMEM's lower half (the TLUT owns the upper): 2048 bytes,
 * one byte per CI8 texel, with each ROW padded to 8 bytes by the RDP's DMA.
 * The old check demanded a fixed 64x32 box, which rejected every tall-thin
 * sprite the title screen draws -- a 17x49 glyph strip is 1176 padded bytes
 * and fits with room to spare. Shape does not matter; padded area does. */
#define TEX_TMEM_BYTES 2048

/* Set at bind time from the page's stored format. A CI4 page packs two texels
 * per byte, so TMEM's 2 KB holds 4096 of its texels instead of 2048 and the
 * RDP's 8-byte row granularity lands every 16 texels instead of every 8. */
static int s_pageIs4;

static int TexBoxFits(int s0, int t0, int s1, int t1)
{
    int w    = s1 - s0;
    int h    = t1 - t0;
    int padW = s_pageIs4 ? ((w + 15) & ~15) : ((w + 7) & ~7);
    int cost = s_pageIs4 ? (padW * h) / 2 : (padW * h);
    return w > 0 && h > 0 && cost <= TEX_TMEM_BYTES;
}

static const uint8_t*  s_texPage;
static const uint32_t* s_texPal;
/* The TLUT is DMA'd by the RDP when the queued load EXECUTES, not when it is
 * enqueued. One static table, rebuilt for the next palette while the previous
 * load was still queued, handed the earlier triangles the LATER palette --
 * Harry's parts flickering into each other's colours every frame. A ring gives
 * each upload its own memory; wrapping drains the queue first. */
#define TLUT_RING_N 64
static uint16_t        s_tlutRing[TLUT_RING_N][256];
static const void*     s_tlutRingPal[TLUT_RING_N];   /* palette each slot was built from */
static unsigned        s_tlutRingGen[TLUT_RING_N];   /* g_PsxVramPalGen at build time */
static int             s_tlutIdx;
static uint16_t*       s_tlut = s_tlutRing[0];
static int             s_tlutDirty;
static int             s_cnTlutReuse;

/* Texture tile window. TMEM holds 2 KB of CI8 texels; instead of loading each
 * triangle's own UV box, load the largest window around it that fits and let
 * every following triangle whose box lies inside skip the load entirely --
 * the two halves of a quad always do, neighbouring wall quads usually do. The
 * loader is kept across triangles of one page so its per-call setup is not
 * redone. Both reset on every page bind and every frame. */
static tex_loader_t    s_texLoader;
static int             s_texLoaderValid;
static int             s_winValid, s_winS0, s_winT0, s_winS1, s_winT1;
static int             s_cnWinHits;
static surface_t       s_pageSurf;

/* Census, so a frame that draws nothing can say why. */
static int s_cnTris;
static int s_cnDropped;
static int s_cnTexTris;
static int s_cnTexTooBig;
static int s_cnTexSplit;
/* Per-frame profile: where the CPU's frame goes. waitFb = display_get() (the
 * RDP still owns every buffer = RDP-bound); submit = tile upload + triangle
 * issue; binds = texture page changes (each is a flush); tlutWrap = ring
 * wraps (each is an rspq_wait). */
static unsigned long long s_cnWaitFbTicks;
static unsigned long long s_cnSubmitTicks;
static int s_cnUploads;
static int s_cnTlutWrap;
static int s_cnBinds;
static int s_cnBinds4;   /* of those, pages bound as CI4 */
/* Is the render phase CPU work or time BLOCKED behind the RDP? [OTT] measured
 * ~240 us per primitive, constant across a 38-prim menu and a 507-prim room,
 * which the per-primitive CPU path (a few float conversions) cannot explain.
 * Two instruments settle it: the RDP's own busy counters (cleared at
 * FrameBegin; 62.5 MHz), which say how long the pipeline and the TMEM loader
 * actually ran during the CPU's frame, and the SPREAD of individual submit
 * calls -- CPU cost is uniform, queue stalls are bursty. */
static unsigned long long s_cnTriMax, s_cnUpMax;
static int s_cnTriCalls, s_cnTriSlow, s_cnUpSlow;
#define PROF_SLOW_TICKS (TICKS_FROM_MS(1) / 10)   /* 100 us */
static unsigned RdpBusyUs(uint32_t counter24) { return (unsigned)(((unsigned long long)(counter24 & 0xFFFFFF) * 2u) / 125u); }

static const rdpq_trifmt_t TRIFMT_SH_SHADE = {
    .pos_offset   = 0,
    .shade_offset = 4,
    .tex_offset   = -1,
    .z_offset     = -1,
};

/* The render mode the RDP is actually in, so a bind that changes nothing the
 * RDP cares about does not re-issue it. `s_modeDirty` is set by every page
 * bind, but the mode depends only on WHETHER we are texturing and on the PSX
 * blend -- not on which page. That made ~100 full mode resets per frame, and a
 * mode reset is not a cheap register write: rdpq_set_mode_standard rewrites
 * the whole SOM/combiner state, which the RDP can only adopt by draining its
 * pipeline. With the RDP's pipe-busy counter sitting at 205 ms of a 233 ms
 * frame for only ~940 triangles, per-command drains are the cost, and these
 * are the ones we can simply not issue. Reset in FrameBegin, because
 * rdpq_attach re-establishes the mode itself. */
static int s_appliedTex   = -1;
static int s_appliedBlend = -2;
static int s_cnModeSets;

/* Force the next ApplyMode to re-issue the PSX render mode. Called after the
 * native world path (ShT3d_WorldFlush) reprograms rdpq for 3D, so the PSX OT
 * walk that follows does not inherit t3d's combiner/blender/scissor. */
void GpuNv2a_PsxModeInvalidate(void)
{
    s_modeDirty    = 1;
    s_appliedTex   = -1;
    s_appliedBlend = -2;
    /* The native passes LOAD_TLUT their own palettes into the same TMEM
     * bank this path memoises as "my palette is resident" (s_tlutDirty=0).
     * Since the double-draw fix the PSX path draws only a few prims a frame
     * (the global-PLM instances: wall posters, the map board), so nothing
     * else re-dirtied it -- they drew with the world's last palette. */
    s_tlutDirty    = 1;
    /* rdpq scissor was narrowed to the t3d viewport; restore full screen. */
    rdpq_set_scissor(0, 0, SCR_W, SCR_H);
}

static void ApplyMode(void)
{
    int texOn;

    if (!s_modeDirty)
        return;
    s_modeDirty = 0;

    texOn = (s_texEnabled && s_texPage != NULL) ? 1 : 0;
    if (texOn == s_appliedTex && s_curBlend == s_appliedBlend)
        return;                     /* same mode the RDP already holds */
    s_appliedTex   = texOn;
    s_appliedBlend = s_curBlend;
    s_cnModeSets++;

    rdpq_set_mode_standard();
    /* Stage 1: the PSX path (characters, items, 2D) has no real per-vertex
     * depth yet (pos[2]=0), so it must NOT touch the Z-buffer -- otherwise it
     * would write Z=0 (nearest) and wrongly occlude the foreground world pass.
     * It keeps compositing by painter's order exactly as before. Characters
     * begin Z-testing in a later stage once they carry real depth. */
    if (ZBufOn())
        rdpq_mode_zbuf(false, false);
    if (texOn)
    {
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
        rdpq_mode_tlut(TLUT_RGBA16);
        /* PSX texel 0x0000 is ALWAYS transparent; the TLUT encodes it as alpha
         * 0, but without an alpha test the RDP happily draws alpha-0 texels as
         * opaque black. That was every glyph and sprite carrying its empty
         * texels as a solid black box -- the title menu's "START" sat in one. */
        rdpq_mode_alphacompare(1);
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

    if (++s_tlutIdx >= TLUT_RING_N)
    {
        s_tlutIdx = 0;
        rspq_wait();
        s_cnTlutWrap++;
    }
    s_tlut = s_tlutRing[s_tlutIdx];

    for (i = 0; i < 256; i++)
    {
        uint32_t c = s_texPal[i];             /* A8R8G8B8 */
        s_tlut[i] = (uint16_t)((((c >> 16) & 0xF8) << 8) |
                               (((c >> 8)  & 0xF8) << 3) |
                               (((c)       & 0xF8) >> 2) |
                               ((c >> 31) & 1));
#ifdef SH_N64_PORT
        /* [TLUT]: how many palette entries are TRANSPARENT. Menu glyph cells
         * are 12 wide but the font advances only 9-13, so neighbouring
         * letters overlap by design and the cell's spare columns MUST be
         * transparent or every letter paints over the next -- which is what
         * the garbled menu looks like. opaque==256 means nothing is
         * transparent and the alpha test can never discard. */
        if (i == 255) {
            int _t = 0, _k;
            for (_k = 0; _k < 256; _k++) if ((s_tlut[_k] & 1) == 0) _t++;
            {
                static int _tl;
                if ((_tl++ & 31) == 0)
                    SH_DBG("[TLUT] transparent=%d opaque=%d entry0=%04x", _t, 256 - _t, s_tlut[0]);
            }
        }
#endif
    }
}

/* Bind the current palette's TLUT. A ring slot already built from this palette
 * (same pointer, same generation) is reloaded by the RDP from the same RAM
 * with no CPU work; otherwise the next slot is built. The dedup matters: 60-200
 * page binds a frame cycle through ~20 palettes, and each ring wrap is a full
 * GPU drain. */
static void UploadTlut(void)
{
    int k;

    for (k = 0; k < TLUT_RING_N; k++)
        if (s_tlutRingPal[k] == (const void*)s_texPal && s_tlutRingGen[k] == g_PsxVramPalGen)
            break;
    if (k == TLUT_RING_N)
    {
        BuildTlut();                      /* advances s_tlutIdx; drains on wrap */
        k = s_tlutIdx;
        s_tlutRingPal[k] = (const void*)s_texPal;
        s_tlutRingGen[k] = g_PsxVramPalGen;
        /* Same cache rule as the page: built by CPU stores, DMA'd by the RDP. */
        data_cache_hit_writeback(s_tlutRing[k], 256 * sizeof(uint16_t));
    }
    else
        s_cnTlutReuse++;
    /* CI4 indexes a 16-entry bank (palette 0), and a 4-bit PSX page only ever
     * had 16 real colours anyway -- the other 240 entries PaletteBuild wrote
     * are whatever VRAM sat past the CLUT row. Uploading them was a 480-byte
     * DMA per bind for texels that cannot address them. */
    rdpq_tex_upload_tlut(s_tlutRing[k], 0, s_pageIs4 ? 16 : 256);
    s_tlutDirty = 0;
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

/* Draw one staged triangle, splitting it until each piece's UV footprint fits
 * the TMEM tile. Bisection at the midpoint of the longest UV edge; position,
 * colour, UV and W all interpolate linearly, WHICH IS FAITHFUL: the PSX
 * texture-mapped affinely, so a linearly split pair rasterises to exactly the
 * texels the unsplit triangle would have -- this is not an approximation the
 * way it would be under perspective-correct sampling.
 *
 * Depth 5 caps a triangle at 32 pieces. A triangle still too big after that
 * spans >2048 texels of a 256-texel page and is degenerate input; it draws
 * flat rather than wrong. */
static void DrawStagedTri(const float* va, const float* vb, const float* vc, int depth);

static void MidVert(float* out, const float* p, const float* q)
{
    int i;
    for (i = 0; i < 9; i++)
        out[i] = (p[i] + q[i]) * 0.5f;
}

/* --- screen clipping -------------------------------------------------------
 *
 * The RDP walks a triangle from its top scanline to its bottom one whether or
 * not any of those scanlines cross the scissor rectangle. The PSX GPU did not:
 * it rasterised only inside the drawing area, so PSX games hand over
 * primitives that reach far off-screen and never paid for it. [OTS] shows the
 * in-game ordering table spanning x -864..1120 and y -977..858 on a 320x240
 * screen, and the RDP's own counters put its pipeline busy for ~150 ms of a
 * 164 ms frame with only ~900 triangles -- ~10,000 RDP cycles each, which
 * visible pixels cannot account for and off-screen scanlines can. Every N64
 * microcode clips on the RSP for exactly this reason; here it is done on the
 * CPU, in screen space, before rdpq_triangle ever sees the triangle.
 *
 * Clip rect = the live scissor plus a guard band, so the edges the clip
 * creates always lie off-screen: no T-junction seams against unclipped
 * neighbours, and a triangle that overhangs by a few pixels is not split at
 * all. Attributes are interpolated linearly in screen space, which is exactly
 * what the RDP does for shade, and exact for S,T here because W is 1.0 on
 * this port (no PGXP). clip_screen=0 in silenthill.cfg turns it off. */
static float s_clipX0, s_clipY0, s_clipX1 = (float)SCR_W, s_clipY1 = (float)SCR_H;
static int   s_cnClipped, s_cnRejected;

/* RDP probe (rdp_probe=1 in silenthill.cfg). The RDP's pipe-busy counter reads
 * ~200 us per triangle whether the triangles are large or clipped small, and
 * texture loading is under 5% of it. To split that time into fill, texture
 * and raw per-command cost without three test sessions, the probe cycles the
 * render every 64 frames -- the [PROF] period -- through: 0 normal; 1 NOFILL
 * (every triangle is issued at one pixel: same commands, loads and syncs,
 * no rasterisation); 2 NOTEX (flat shade, no loads: same triangle count, no
 * texture path). Each census line names its mode. The picture flickers; it
 * is a measurement build setting, not a way to play.
 *
 * The area census runs always: the summed screen-space area of every
 * triangle after clipping is the overdraw the RDP actually rasterised. */
static int s_probeMode;
static unsigned long long s_cnAreaPx;
static int s_cnBigTris;      /* > 4096 px after clipping */
static int s_cnCulled;       /* backfacing triangles rejected */
static void CountArea(const float* a, const float* b, const float* c)
{
    float ar = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
    if (ar < 0.0f) ar = -ar;
    ar *= 0.5f;
    s_cnAreaPx += (unsigned long long)ar;
    if (ar > 4096.0f) s_cnBigTris++;
}
#define CLIP_GUARD   8.0f
#define CLIP_MAX_V   9      /* 3 vertices + one per plane, rounded up */

/* One Sutherland-Hodgman pass against x (axis 0) or y (axis 1) = bound. */
static int ClipEdge(const float* in, int inN, float* out, int n, int axis, float bound, int keepGreater)
{
    int i, outN = 0;

    for (i = 0; i < inN; i++)
    {
        const float* a  = in + i * n;
        const float* b  = in + ((i + 1) % inN) * n;
        float        da = keepGreater ? (a[axis] - bound) : (bound - a[axis]);
        float        db = keepGreater ? (b[axis] - bound) : (bound - b[axis]);
        int          ina = (da >= 0.0f), inb = (db >= 0.0f);

        if (ina)
        {
            memcpy(out + outN * n, a, (size_t)n * sizeof(float));
            outN++;
        }
        if (ina != inb)
        {
            float  t = da / (da - db);
            float* o = out + outN * n;
            int    k;
            for (k = 0; k < n; k++)
                o[k] = a[k] + (b[k] - a[k]) * t;
            o[axis] = bound;   /* land exactly on the plane; no drift across passes */
            outN++;
        }
    }
    return outN;
}

/* Clip one triangle of n floats per vertex (X at 0, Y at 1). Writes the
 * resulting convex polygon to `out` (CLIP_MAX_V * n floats) and returns its
 * vertex count: 3 untouched, 0 nothing visible, else fan it. */
static int ClipTri(const float* va, const float* vb, const float* vc, int n, float* out)
{
    const float x0 = s_clipX0 - CLIP_GUARD, y0 = s_clipY0 - CLIP_GUARD;
    const float x1 = s_clipX1 + CLIP_GUARD, y1 = s_clipY1 + CLIP_GUARD;
    float bufA[CLIP_MAX_V * 9], bufB[CLIP_MAX_V * 9];
    int   cnt;

    if (!g_PcConfig.n64ClipScreen)
        goto passthrough;

    if ((va[0] < x0 && vb[0] < x0 && vc[0] < x0) || (va[0] > x1 && vb[0] > x1 && vc[0] > x1) ||
        (va[1] < y0 && vb[1] < y0 && vc[1] < y0) || (va[1] > y1 && vb[1] > y1 && vc[1] > y1))
    {
        s_cnRejected++;
        return 0;
    }
    if (va[0] >= x0 && va[0] <= x1 && va[1] >= y0 && va[1] <= y1 &&
        vb[0] >= x0 && vb[0] <= x1 && vb[1] >= y0 && vb[1] <= y1 &&
        vc[0] >= x0 && vc[0] <= x1 && vc[1] >= y0 && vc[1] <= y1)
        goto passthrough;

    memcpy(bufA,         va, (size_t)n * sizeof(float));
    memcpy(bufA + n,     vb, (size_t)n * sizeof(float));
    memcpy(bufA + 2 * n, vc, (size_t)n * sizeof(float));
    cnt = ClipEdge(bufA, 3, bufB, n, 0, x0, 1);
    if (cnt) cnt = ClipEdge(bufB, cnt, bufA, n, 0, x1, 0);
    if (cnt) cnt = ClipEdge(bufA, cnt, bufB, n, 1, y0, 1);
    if (cnt) cnt = ClipEdge(bufB, cnt, out,  n, 1, y1, 0);
    if (cnt < 3)
    {
        s_cnRejected++;
        return 0;
    }
    s_cnClipped++;
    return cnt;

passthrough:
    memcpy(out,         va, (size_t)n * sizeof(float));
    memcpy(out + n,     vb, (size_t)n * sizeof(float));
    memcpy(out + 2 * n, vc, (size_t)n * sizeof(float));
    return 3;
}

static void DrawTexturedTri(const ShVertex* a, const ShVertex* b, const ShVertex* c)
{
    float va[9], vb[9], vc[9], poly[CLIP_MAX_V * 9];
    int   n, i;

    StageTexVert(va, a);
    StageTexVert(vb, b);
    StageTexVert(vc, c);
    /* Clipping BEFORE the tile-window logic also shrinks the UV box it loads:
     * only the visible part of a wall quad's texture reaches TMEM. */
    n = ClipTri(va, vb, vc, 9, poly);
    for (i = 1; i + 1 < n; i++)
        DrawStagedTri(poly, poly + i * 9, poly + (i + 1) * 9, 7);
}

static void DrawStagedTri(const float* va, const float* vb, const float* vc, int depth);

/* DrawStagedTri with the probe hooks bypassed (the NOFILL mode re-enters
 * through here so its one-pixel triangle is not shrunk again). */
static void DrawStagedTriNoProbe(const float* va, const float* vb, const float* vc, int depth)
{
    int saved = s_probeMode;
    s_probeMode = 0;
    DrawStagedTri(va, vb, vc, depth);
    s_probeMode = saved;
}

static void DrawStagedTri(const float* va, const float* vb, const float* vc, int depth)
{
    int s0, t0, s1, t1;

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

    /* Backface rejection. The PSX GPU drew both faces because it had no
     * culling at all, so the game hands over every polygon of a closed room
     * including the ~half pointing away from the camera; each costs a full
     * transform, a TMEM window and an RDP command to end up hidden behind the
     * ones facing us. The screen-space signed area is the test, and it is two
     * subtractions and a cross product on coordinates already in hand.
     *
     * OFF by default: nothing guarantees SH's world geometry is consistently
     * wound, and a wrong guess turns walls invisible rather than merely ugly.
     * cull_backfaces=1 to try it, -1 to cull the other winding. */
    if (g_PcConfig.n64CullBackfaces)
    {
        float ar = (vb[0] - va[0]) * (vc[1] - va[1]) - (vc[0] - va[0]) * (vb[1] - va[1]);
        if (ar * (float)g_PcConfig.n64CullBackfaces > 0.0f)
        {
            s_cnCulled++;
            return;
        }
    }
    CountArea(va, vb, vc);
    if (s_probeMode == 1)
    {
        /* NOFILL: one-pixel triangle, texture path intact. */
        float pb[9], pc[9];
        memcpy(pb, va, sizeof pb); memcpy(pc, va, sizeof pc);
        pb[0] += 1.0f; pc[1] += 1.0f;
        if (!(s_winValid && s0 >= s_winS0 && t0 >= s_winT0 && s1 <= s_winS1 && t1 <= s_winT1))
        {
            /* keep the load pattern honest: same window logic as below */
        }
        DrawStagedTriNoProbe(va, pb, pc, depth);
        return;
    }
    if (s_probeMode == 2)
    {
        /* NOTEX: the flat fallback below, with no texture load. */
        s_cnTexTooBig--;    /* not a real miss; the branch below counts it */
        goto flat_fallback;
    }

    if (!TexBoxFits(s0, t0, s1, t1))
    {
        if (depth > 0)
        {
            /* Split the longest UV edge and recurse. Two pieces per level, so
             * the worst case at depth 5 is 32 uploads for one triangle --
             * which is still drawing, where the old path drew grey. */
            float mid[9];
            float dab, dbc, dca;
            /* Split along the axis that is actually too large, or the split
             * can shave the harmless axis forever while the offending one
             * never shrinks -- which is how the first version of this spent
             * five levels and still handed 233 leaves to the flat path. */
            int   ax = ((s1 - s0) >= (t1 - t0)) ? 6 : 7;

            dab = fabsf(va[ax] - vb[ax]);
            dbc = fabsf(vb[ax] - vc[ax]);
            dca = fabsf(vc[ax] - va[ax]);

            s_cnTexSplit++;
            if (dab >= dbc && dab >= dca)
            {
                MidVert(mid, va, vb);
                DrawStagedTri(va, mid, vc, depth - 1);
                DrawStagedTri(mid, vb, vc, depth - 1);
            }
            else if (dbc >= dca)
            {
                MidVert(mid, vb, vc);
                DrawStagedTri(va, vb, mid, depth - 1);
                DrawStagedTri(va, mid, vc, depth - 1);
            }
            else
            {
                MidVert(mid, vc, va);
                DrawStagedTri(mid, vb, vc, depth - 1);
                DrawStagedTri(va, vb, mid, depth - 1);
            }
            return;
        }

        /* Out of depth: degenerate UV span. Draw flat rather than wrong; the
         * mode is restored IMMEDIATELY because ApplyMode only runs at the top
         * of a flush. */
flat_fallback:
        s_cnTexTooBig++;
        /* Changes the combiner directly and restores it below, so the memo
         * still describes the RDP correctly on the way out. */
        rdpq_mode_tlut(TLUT_NONE);
        rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
        {
            /* TRIFMT_SH_SHADE reads pos at 0 and shade at 4 from ShVertex, but
             * these are STAGED 9-float verts: pos 0, shade 2. A dedicated
             * format keeps the fallback honest. */
            static const rdpq_trifmt_t TRIFMT_STAGED_SHADE = {
                .pos_offset   = 0,
                .shade_offset = 2,
                .tex_offset   = -1,
                .z_offset     = -1,
            };
            rdpq_triangle(&TRIFMT_STAGED_SHADE, va, vb, vc);
        }
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
        rdpq_mode_tlut(TLUT_RGBA16);
        s_cnTris++;
        return;
    }

    /* The RDP's texture DMA reads RAM, not the CPU's data cache, and the page
     * was decoded by CPU stores that may still be sitting there -- the symptom
     * is every texel sampling as index 0 and the whole screen drawing one flat
     * colour. Write back exactly the rows this tile will read; the page can be
     * REDECODED into the same buffer at any time, so this cannot be hoisted to
     * the bind. */
    /* The page itself was written back to RAM when it was decoded
     * (psx_vram.c, SH_DMA_WRITEBACK), so no per-tile cache work here. */
    if (s_winValid && s0 >= s_winS0 && t0 >= s_winT0 && s1 <= s_winS1 && t1 <= s_winT1)
    {
        s_cnWinHits++;
    }
    else
    {
        unsigned long long _t0 = get_ticks();
        int w, h, ws0, wt0;
        /* Window arithmetic is in TEXELS; a CI4 page gets twice the budget and
         * twice the row granularity, which is the whole point of storing it. */
        const int align  = s_pageIs4 ? 16 : 8;
        const int budget = s_pageIs4 ? (TEX_TMEM_BYTES * 2) : TEX_TMEM_BYTES;

        /* Grow the box to the largest window that still fits TMEM, wide
         * first: the triangles that follow a wall quad's first half share
         * its rows far more often than its columns. */
        w = (s1 - s0 + align - 1) & ~(align - 1);
        if (w < align) w = align;
        h = t1 - t0;
        while (w < 128 && (w * 2) * h <= budget)
            w *= 2;
        if (w > TEX_PAGE_DIM) w = TEX_PAGE_DIM;
        h = budget / w;
        if (h > TEX_PAGE_DIM) h = TEX_PAGE_DIM;
        if (h < t1 - t0)
        {
            w = (s1 - s0 + align - 1) & ~(align - 1);   /* the bare box; TexBoxFits proved it fits */
            h = t1 - t0;
        }
        ws0 = s0 - (w - (s1 - s0)) / 2;
        if (ws0 < 0) ws0 = 0;
        if (ws0 + w > TEX_PAGE_DIM) ws0 = TEX_PAGE_DIM - w;
        /* A CI4 load snaps s0 DOWN to the byte and s1 UP (rdpq_tex.c), which
         * would widen the rect past the budget just computed. w is a multiple
         * of 16, so an even ws0 keeps both edges on byte boundaries. */
        if (s_pageIs4) ws0 &= ~1;
        wt0 = t0 - (h - (t1 - t0)) / 2;
        if (wt0 < 0) wt0 = 0;
        if (wt0 + h > TEX_PAGE_DIM) wt0 = TEX_PAGE_DIM - h;

        if (!s_texLoaderValid)
        {
            s_texLoader = tex_loader_init(TILE0, &s_pageSurf);
            tex_loader_set_tmem_addr(&s_texLoader, 0);
            s_texLoaderValid = 1;
        }
        tex_loader_load(&s_texLoader, ws0, wt0, ws0 + w, wt0 + h);
        s_winS0 = ws0; s_winT0 = wt0; s_winS1 = ws0 + w; s_winT1 = wt0 + h;
        s_winValid = 1;
        s_cnUploads++;
        {
            unsigned long long d = get_ticks() - _t0;
            s_cnSubmitTicks += d;
            if (d > s_cnUpMax) s_cnUpMax = d;
            if (d > PROF_SLOW_TICKS) s_cnUpSlow++;
        }
    }
    {
        unsigned long long _t0 = get_ticks(), d;
        rdpq_triangle(&TRIFMT_SH_TEX, va, vb, vc);
        d = get_ticks() - _t0;
        s_cnSubmitTicks += d;
        s_cnTriCalls++;
        if (d > s_cnTriMax) s_cnTriMax = d;
        if (d > PROF_SLOW_TICKS) s_cnTriSlow++;
    }
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
            UploadTlut();

        for (i = s_runStart; i + 2 < s_batchUsed; i += 3)
            DrawTexturedTri(&s_batch[i], &s_batch[i + 1], &s_batch[i + 2]);
    }
    else
    {
        /* PSX abr 2 is SUBTRACTIVE: out = B - F. The RDP blender has no
         * negative coefficient, so it cannot do that -- and routing it through
         * MULTIPLY at the vertices' alpha of 1.0 REPLACES the screen with the
         * fade colour instead: the boot fade is a full-screen abr-2 quad at
         * 0.88 grey, and that painted the entire frame flat light-grey over
         * everything, every frame, for as long as this port has drawn.
         *
         * The mapping that keeps the fade a fade: draw BLACK with alpha equal
         * to the subtrahend's brightness. MEM*(1-f) is multiplicative
         * darkening -- not bit-exact against B-F's clamp, but monotone in f,
         * black at f=1, identity at f=0, which is the whole visible behaviour
         * of a fade. */
        if (s_curBlend == 3)
        {
            for (i = s_runStart; i < s_batchUsed; i++)
            {
                float f = s_batch[i].col[0];
                if (s_batch[i].col[1] > f) f = s_batch[i].col[1];
                if (s_batch[i].col[2] > f) f = s_batch[i].col[2];
                s_batch[i].col[0] = 0.0f;
                s_batch[i].col[1] = 0.0f;
                s_batch[i].col[2] = 0.0f;
                s_batch[i].col[3] = f;
            }
        }

        for (i = s_runStart; i + 2 < s_batchUsed; i += 3)
        {
            float poly[CLIP_MAX_V * 8];
            int   n, k;
            n = ClipTri((const float*)&s_batch[i], (const float*)&s_batch[i + 1],
                        (const float*)&s_batch[i + 2], 8, poly);
            for (k = 1; k + 1 < n; k++)
            {
                CountArea(poly, poly + k * 8, poly + (k + 1) * 8);
                if (s_probeMode == 1)
                {
                    float pb[8], pc[8];
                    memcpy(pb, poly, sizeof pb); memcpy(pc, poly, sizeof pc);
                    pb[0] += 1.0f; pc[1] += 1.0f;
                    rdpq_triangle(&TRIFMT_SH_SHADE, poly, pb, pc);
                }
                else
                    rdpq_triangle(&TRIFMT_SH_SHADE, poly, poly + k * 8, poly + (k + 1) * 8);
            }
            s_cnTris++;
        }
    }

    s_runStart = s_batchUsed;
}

/* Axis-aligned textured quad -> one rdpq_tex_blit instead of two recursive
 * split-triangles. The menu/HUD art is 128-256px tiles whose UV boxes never
 * fit the 2048-byte TMEM window, so the tri path re-uploads dozens of chunks
 * per sprite per frame - the measured "menus run like shit". The blitter
 * chunks TMEM internally and draws texture_rectangles, which is the N64-native
 * way to move sprites. Flat-colour quads only (2D art is flat-lit); the
 * batch is flushed first so paint order inside a run is preserved, and the
 * combiner is restored via s_modeDirty for whatever draws next. */
int GpuNv2a_TryBlitQuad(const ShVertex* v0, const ShVertex* v1,
                        const ShVertex* v2, const ShVertex* v3)
{
    float x0, y0, x1, y1, s0f, t0f, s1f, t1f;
    int   s0, t0, s1, t1, t;

    if (!s_texEnabled || s_texPage == NULL)
        return 0;

    /* Screen-space axis-aligned rect in the FT4 vertex order TL,TR,BL,BR. */
    if (v0->pos[1] != v1->pos[1] || v2->pos[1] != v3->pos[1] ||
        v0->pos[0] != v2->pos[0] || v1->pos[0] != v3->pos[0])
        return 0;
    x0 = v0->pos[0]; y0 = v0->pos[1];
    x1 = v3->pos[0]; y1 = v3->pos[1];
    if (x1 <= x0 || y1 <= y0)
        return 0;

    /* UV rect must be axis-aligned the same way (no flips - the game's 2D
     * art never mirrors through this path). */
    if (v0->tex[1] != v1->tex[1] || v2->tex[1] != v3->tex[1] ||
        v0->tex[0] != v2->tex[0] || v1->tex[0] != v3->tex[0])
        return 0;
    s0f = v0->tex[0]; t0f = v0->tex[1];
    s1f = v3->tex[0]; t1f = v3->tex[1];
    if (s1f <= s0f || t1f <= t0f)
        return 0;

    /* Flat colour only. */
    if (v0->col[0] != v1->col[0] || v0->col[0] != v2->col[0] || v0->col[0] != v3->col[0] ||
        v0->col[1] != v1->col[1] || v0->col[1] != v2->col[1] || v0->col[1] != v3->col[1] ||
        v0->col[2] != v1->col[2] || v0->col[2] != v2->col[2] || v0->col[2] != v3->col[2])
        return 0;

    /* ShVertex UVs are RAW TEXELS (PutVertUV stores u/v verbatim), NOT 0..1 --
     * scaling them by the page dimension pushed every span past the clamp and
     * made this function bail on every quad, which is why arming it changed
     * nothing on screen. */
    s0 = (int)s0f;
    t0 = (int)t0f;
    s1 = (int)(s1f + 0.5f);
    t1 = (int)(t1f + 0.5f);
    /* DECLINE anything that reaches outside the 256x256 page -- do NOT clamp.
     * PSX VRAM is 512 rows and sprites routinely span past a page's bottom
     * (KONAMI.TIM is 192 rows from v=240), so clamping kept a 16-row slice and
     * then stretched it over the sprite's full height: a dark vertical bar
     * where the logo belongs, which is exactly how this froze the console on
     * its first live boot. The triangle path already handles these correctly,
     * so handing them back is both safe and right. */
    if (s0 < 0 || t0 < 0 || s1 > TEX_PAGE_DIM || t1 > TEX_PAGE_DIM)
        return 0;
    if (s1 <= s0 || t1 <= t0)
        return 0;

    /* Earlier prims of this run first, so layering survives. */
    GpuNv2a_FlushBatch();
    /* FORCE the full mode apply: ApplyMode early-outs when !s_modeDirty, which
     * would leave whatever the previous prim set -- including TLUT_NONE, under
     * which this CI8 page samples its palette INDICES as colour. Dirty it first
     * so tlut, alpha-compare and the PSX blend mode are all established, then
     * swap only the combiner (prim colour drives modulation here, not shade). */
    s_modeDirty = 1;
    ApplyMode();
    if (s_tlutDirty)
        UploadTlut();

    /* No cache work here: psx_vram.c writes the page back when it decodes it
     * (SH_DMA_WRITEBACK), exactly as on the triangle path. This loop also
     * assumed a CI8 row pitch, which a CI4 page does not have. */

    /* PutVertUV ALREADY applied the PSX 0x80=1.0 modulation (r <<= 1, clamped),
     * so col is the final factor -- doubling it again blew every blit to white. */
    rdpq_set_prim_color(RGBA32((int)(v0->col[0] * 255.0f),
                               (int)(v0->col[1] * 255.0f),
                               (int)(v0->col[2] * 255.0f), 255));
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);

    rdpq_tex_blit(&s_pageSurf, x0, y0, &(rdpq_blitparms_t){
        .s0      = s0,
        .t0      = t0,
        .width   = s1 - s0,
        .height  = t1 - t0,
        .scale_x = (x1 - x0) / (float)(s1 - s0),
        .scale_y = (y1 - y0) / (float)(t1 - t0),
    });

    s_cnTris += 2;
    s_cnTexTris += 2;
    s_modeDirty    = 1;   /* combiner was changed; next flush re-applies */
    s_appliedBlend = -2;  /* ...and it really must re-apply: invalidate the memo */
    return 1;
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
    /* Tiny3D init is LAZY (first ShT3d_SpikeDraw) -- calling it here, before
     * the [BOOT] psx-ram/arena setup, wedged boot at varying steps. */
    s_inited = 1;

    SH_DBG("[GPU] rdp up: %dx%d 16bpp x2, no z (painter's order from the OT)", SCR_W, SCR_H);
}

/* RSP-park detector (evidence for the zero-command-header crash, see
 * ShN64_RspCrashDetail). FrameEnd marks the end of each frame's command
 * stream with a syncpoint; FrameBegin checks whether the RSP ever reached the
 * previous one while it waits for a framebuffer. */
static rspq_syncpoint_t   s_frameSp;
static int                s_frameSpValid;

void GpuNv2a_FrameBegin(void)
{
    unsigned int clear;

    if (!s_inited)
        GpuNv2a_Init();

    /* Incremented HERE, not in FrameEnd: gpu_xbox.c documents the contract as
     * "incremented each GpuNv2a_FrameBegin" and psx_vram.c keys its whole
     * page/palette LRU on it (lastUse == thisFrame decides what may be
     * evicted). A counter that moves at the wrong end of the frame makes the
     * cache think every page was touched in the frame it is about to draw. */
    g_Nv2aFrameCount++;

    s_frameStart = get_ticks();
    /* The RDP's 24-bit busy counters, zeroed here so [PROF] reads one frame's
     * worth. Wraps at 268 ms; a frame longer than that under-reports. */
    *DP_STATUS = DP_WSTATUS_RESET_TMEM_COUNTER | DP_WSTATUS_RESET_PIPE_COUNTER |
                 DP_WSTATUS_RESET_CMD_COUNTER  | DP_WSTATUS_RESET_CLOCK_COUNTER;
    s_cnTriMax = s_cnUpMax = 0;
    s_cnTriCalls = s_cnTriSlow = s_cnUpSlow = 0;
    s_cnClipped = s_cnRejected = 0;
    s_cnModeSets = 0;
    /* rdpq_attach re-establishes the render mode for the new frame. */
    s_appliedTex = -1; s_appliedBlend = -2;
    s_cnAreaPx = 0;
    s_cnBigTris = 0;
    s_cnCulled = 0;
    s_probeMode = g_PcConfig.n64RdpProbe ? (int)((g_Nv2aFrameCount / 64) % 3) : 0;
    s_clipX0 = 0.0f;           s_clipY0 = 0.0f;
    s_clipX1 = (float)SCR_W;   s_clipY1 = (float)SCR_H;
    s_batchUsed  = 0;
    s_runStart   = 0;
    s_cnTris     = 0;
    s_cnDropped  = 0;
    s_cnTexTris  = 0;
    s_cnTexTooBig = 0;
    s_cnTexSplit = 0;
    s_cnWaitFbTicks = 0;
    s_cnSubmitTicks = 0;
    s_cnUploads  = 0;
    s_cnBinds4   = 0;
    s_cnTlutWrap = 0;
    s_cnBinds    = 0;
    s_cnWinHits  = 0;
    s_cnTlutReuse = 0;
    s_winValid   = 0;
    s_texLoaderValid = 0;
    g_PsxVramDecodes = g_PsxVramDrains = g_PsxVramPalBuilds = 0;
    g_PsxVramDecodeTicks = g_PsxVramDrainTicks = 0;
    g_XbChunkDrawCycles = 0;
    g_XbCharaCycles = 0;
    g_XbCharaCount = 0;
    g_ProfLogStderrTicks = g_ProfLogSdTicks = 0;
    g_ProfLogLines = 0;
    s_modeDirty  = 1;

    {
        unsigned long long _t0 = get_ticks();
        /* RSP-park detector. The recurring "RSP crash" is the rsp_queue ucode
         * parked on a ZERO command header -- a stray 32-bit zero written into
         * the rspq buffer by a writer not yet found -- and display_get() below
         * then blocks on a framebuffer the RDP never releases until the 5 s
         * watchdog fires; by then the game state around the write is gone.
         * So poll for the framebuffer instead, and if LAST frame's end-of-
         * stream syncpoint is still unreached after 1 s (a slow frame clears it
         * well inside that; a parked RSP never does) take a game-state
         * snapshot NOW, then fall through to the blocking display_get() so its
         * watchdog still produces the [CRASH] hole dump. A wait that resolves
         * after the snapshot is logged as such, so a merely slow frame is
         * never mistaken for a park. */
        extern void ShN64_ParkSnapshot(unsigned frame, unsigned ms) __attribute__((weak));
        int parkLogged = 0;
        /* 32-bit COUNT register, NOT get_ticks(): the 64-bit get_ticks() is a
         * software extension that is not interrupt-safe -- a timer interrupt
         * between its read and its `last` update makes the caller see a false
         * wrap and PERMANENTLY adds 2^32 to the shared tick base. Polled
         * thousands of times per frame from this loop it did exactly that
         * (every [RSPQ-PARK] read "+91.6 s" = 2^32/46875), which both faked
         * the parks and skewed every 64-bit tick consumer for the rest of the
         * run. TICKS_SINCE is the documented idiom for short intervals:
         * register-only, wrap-safe below ~45 s. */
        uint32_t t0_32 = TICKS_READ();
        s_fb = display_try_get();
        while (s_fb == NULL)
        {
            unsigned ms = (unsigned)(TICKS_SINCE(t0_32) / (TICKS_PER_SECOND / 1000));
            if (!parkLogged && s_frameSpValid && ms >= 1000 && !rspq_syncpoint_check(s_frameSp))
            {
                parkLogged = 1;
                SH_DBG("[RSPQ-PARK] RSP made no progress for %u ms (frame %u): last frame's syncpoint unreached",
                       ms, (unsigned)g_Nv2aFrameCount);
                if (ShN64_ParkSnapshot)
                    ShN64_ParkSnapshot((unsigned)g_Nv2aFrameCount, ms);
            }
            if (ms >= 1500)
                break;      /* hand the wait, and its watchdog, back to display_get */
            s_fb = display_try_get();
        }
        if (s_fb == NULL)
            s_fb = display_get();
        else if (parkLogged)
            SH_DBG("[RSPQ-PARK] resolved after the snapshot: a slow frame, not a park");
        s_cnWaitFbTicks = get_ticks() - _t0;
    }

    /* NOTE: a per-frame rspq_wait() drain used to sit here (to bound each
     * frame's command stream). It was a band-aid on a wrong theory and it
     * reached into libdragon's buffer machinery every frame; the RDP-overrun
     * crashes only began AFTER it landed. Removed -- libdragon's own
     * backpressure (the RSP blocks when the RDP dynamic buffer is full) keeps
     * the two in step, and the enlarged rspq command buffer is the real fix
     * for the original RSP run-off. display_get() above already paced us to a
     * free framebuffer. */

    /* Lazy one-time Z-buffer alloc, gated by config (zbuffer=1: world +
     * character, zbuffer=2: character only). Done here rather than Init so
     * a failed 150KB alloc on a 4MB machine just falls back to no-Z instead
     * of wedging boot. */
    if (!s_zbufTried && g_PcConfig.n64ZBuffer)
    {
        s_zbufTried = 1;
        s_zbuf = surface_alloc(FMT_RGBA16, SCR_W, SCR_H);
        SH_DBG("[GPU] z-buffer %s (%dKB) mode=%d (1 = world tile-sorted + character, 2 = character only)",
               s_zbuf.buffer ? "ON" : "alloc FAILED (no heap) -> painter's path",
               (SCR_W * SCR_H * 2) / 1024, g_PcConfig.n64ZBuffer);
    }

    rdpq_attach(s_fb, ZBufOn() ? &s_zbuf : NULL);

    /* The PSX draw-env isbg background -- the fog colour in-game. Taking it
     * from gpu_xbox.c rather than picking one here is what keeps a map's fog
     * and the frame clear the same colour. */
    clear = GpuXbox_GetClearColor();
    rdpq_set_mode_fill(RGBA32((clear >> 16) & 0xFF, (clear >> 8) & 0xFF, clear & 0xFF, 0xFF));
    rdpq_fill_rectangle(0, 0, SCR_W, SCR_H);
    if (ZBufOn())
        rdpq_clear_z(ZBUF_MAX);   /* far; the world writes nearer as it draws */

    ShT3d_NotifyFrameBegin();
}

void GpuNv2a_FrameEnd(void)
{
    if (!s_inited || s_fb == NULL)
        return;

    /* Native world is drawn in game_main BEFORE GsDrawOt (world under the PSX
     * characters/items -- correct fixed-camera order). This point stays as a
     * safety flush of anything still queued. */
    GpuNv2a_FlushBatch();

    /* Stage-0 spike (disabled): see t3d_n64.c for the state-discipline note. */
    ShT3d_SpikeDraw();
    ShT3d_NotifyFrameEnd();

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
    if (SH_N64_LOG_HIDE_ON_FIRST_TRI && s_cnTexTris > 0 && ShLogN64_ScreenEnabled())
    {
        ShLogN64_ScreenEnable(0);
        SH_DBG("[GPU] first TEXTURED geometry drawn; on-screen log off");
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
        /* Park detector: the last thing queued this frame. FrameBegin can then
         * tell "RSP still working through a long frame" from "RSP parked on a
         * zero header" by whether this point is ever reached. */
        s_frameSp      = rspq_syncpoint_new();
        s_frameSpValid = 1;
        rdpq_detach_show();
    }
    s_fb = NULL;

    g_Nv2aDrawCycles = get_ticks() - s_frameStart;


    {
        /* Paced by a LOCAL counter: g_Nv2aFrameCount is stuck at 0 (open bug,
         * see README), and pacing on it makes this line flood every frame. */
        static int s_censusTick = 0;
        if ((s_censusTick++ & 63) == 0)
        {
            /* TEMP diagnostic: the camera matrix, from the frame loop so no
             * game-side sampling phase can hide it. MATRIX is {s16 m[3][3];
             * s16 pad; s32 t[3]} = 32 bytes; HAL cannot include game.h, so
             * read it as raw halves/words. */
            extern short VbWvsMatrix[];
            extern int   g_N64CamProbe[4]; /* vb,vw,mv,ic call counts */
            extern int   g_N64VbSnap[6];   /* hier00,hier22,work00,id200,wvs00,super */
            {
                /* [PIPE]: the whole vertex pipeline in one line per window.
                 * zeroBone/zeroModel = matrices the animation or the chunk
                 * loader never wrote (a nonzero count means geometry is
                 * unpositioned, NOT mis-projected). instX/instZ = the spread
                 * of model instance positions this window: a real room spans
                 * thousands of units, a coagulated pile spans ~0. */
                extern int g_N64ZeroBoneCount, g_N64BoneCount;
                extern int g_N64ZeroModelMat, g_N64ModelMat;
                extern int g_N64InstSpread[4];
                extern int g_N64AnimLoop, g_N64AnimGated, g_N64AnimUpd;
                SH_DBG("[PIPE] bones=%d zero=%d models=%d zero=%d instX=%d..%d instZ=%d..%d",
                       g_N64BoneCount, g_N64ZeroBoneCount,
                       g_N64ModelMat, g_N64ZeroModelMat,
                       g_N64InstSpread[0], g_N64InstSpread[1],
                       g_N64InstSpread[2], g_N64InstSpread[3]);
                SH_DBG("[PIPE2] animLoop=%d gated=%d boneUpd=%d",
                       g_N64AnimLoop, g_N64AnimGated, g_N64AnimUpd);
                g_N64AnimLoop = g_N64AnimGated = g_N64AnimUpd = 0;
                g_N64BoneCount = g_N64ZeroBoneCount = 0;
                g_N64ModelMat = g_N64ZeroModelMat = 0;
                g_N64InstSpread[0] = g_N64InstSpread[2] = 0x7FFFFFF;
                g_N64InstSpread[1] = g_N64InstSpread[3] = -0x7FFFFFF;
            }
            SH_DBG("[WVS] m00=%d t2=%d cam=%d,%d,%d,%d vb=%d,%d,%d,%d,%d,%d",
                   (int)VbWvsMatrix[0],
                   (int)((int*)((char*)VbWvsMatrix + 20))[2],
                   g_N64CamProbe[0], g_N64CamProbe[1], g_N64CamProbe[2], g_N64CamProbe[3],
                   g_N64VbSnap[0], g_N64VbSnap[1], g_N64VbSnap[2],
                   g_N64VbSnap[3], g_N64VbSnap[4], g_N64VbSnap[5]);
        }
        if (((s_censusTick - 1) & 63) == 0)
        {
            SH_DBG("[GPU] f%d tris=%d tex=%d split=%d big=%d drop=%d %dus",
                   g_Nv2aFrameCount, s_cnTris, s_cnTexTris, s_cnTexSplit, s_cnTexTooBig, s_cnDropped,
                   (int)TICKS_TO_US((unsigned)g_Nv2aDrawCycles));
            /* ONE frame's CPU budget, in microseconds. frame = FrameBegin to
             * here; waitFb = blocked on a free framebuffer (RDP-bound if big);
             * submit = tile uploads + triangle issue; vram dec/drain = page
             * decodes and full GPU drains the texture cache had to do. What is
             * left of `frame` after these is the game update + OT walk ([OTT]). */
            SH_DBG("[PROF] frame=%uus waitFb=%uus submit=%uus uploads=%d win=%d binds=%d/ci4=%d tlutWrap=%d tlutReuse=%d | dec=%d/%uus drain=%d/%uus pal=%d | chunk=%uus chara=%uus/%u | log=%d %uus+%uus | audio=%dms | rdp clk=%uus pipe=%uus tmem=%uus cmd=%uus | tri=%d max=%uus slow=%d | up max=%uus slow=%d | clip=%d rej=%d cull=%d modes=%d | px=%uK big=%d probe=%d | vbl=%d dt=%d vc=%d",
                   (unsigned)TICKS_TO_US((unsigned)g_Nv2aDrawCycles),
                   (unsigned)TICKS_TO_US((unsigned)s_cnWaitFbTicks),
                   (unsigned)TICKS_TO_US((unsigned)s_cnSubmitTicks),
                   s_cnUploads, s_cnWinHits, s_cnBinds, s_cnBinds4, s_cnTlutWrap, s_cnTlutReuse,
                   g_PsxVramDecodes, (unsigned)TICKS_TO_US((unsigned)g_PsxVramDecodeTicks),
                   g_PsxVramDrains, (unsigned)TICKS_TO_US((unsigned)g_PsxVramDrainTicks),
                   g_PsxVramPalBuilds,
                   (unsigned)TICKS_TO_US((unsigned)g_XbChunkDrawCycles),
                   (unsigned)TICKS_TO_US((unsigned)g_XbCharaCycles), g_XbCharaCount,
                   g_ProfLogLines,
                   (unsigned)TICKS_TO_US((unsigned)g_ProfLogStderrTicks),
                   (unsigned)TICKS_TO_US((unsigned)g_ProfLogSdTicks),
                   g_ProfAudioMs,
                   RdpBusyUs(*DP_CLOCK), RdpBusyUs(*DP_PIPE_BUSY), RdpBusyUs(*DP_TMEM_BUSY), RdpBusyUs(*DP_BUSY),
                   s_cnTriCalls, (unsigned)TICKS_TO_US((unsigned)s_cnTriMax), s_cnTriSlow,
                   (unsigned)TICKS_TO_US((unsigned)s_cnUpMax), s_cnUpSlow,
                   s_cnClipped, s_cnRejected, s_cnCulled, s_cnModeSets,
                   (unsigned)(s_cnAreaPx / 1000ULL), s_cnBigTris, s_probeMode,
                   VSync(-1), g_VBlanks, GsGetVcount());
        }
    }
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
    s_winValid       = 0;    /* TMEM window belongs to the previous page */
    s_texLoaderValid = 0;

    if (page == NULL || pal == NULL)
    {
        s_texEnabled = 0;
        s_texPage    = NULL;
        s_texPal     = NULL;
        s_pageIs4    = 0;
    }
    else
    {
        if (pal != s_texPal)
            s_tlutDirty = 1;
        if (page != s_texPage)
            s_cnBinds++;
        s_texPage    = (const uint8_t*)page;
        s_texPal     = (const uint32_t*)pal;
        {
            /* The TLUT upload LENGTH follows the page format (16 entries for
             * CI4, 256 for CI8), so a format change has to re-upload even when
             * the palette pointer is unchanged. Only a format change: dirtying
             * unconditionally would put a 1 KB palette DMA on every one of the
             * ~100 binds in a frame, which is what the ring dedup exists to
             * avoid. */
            int was4  = s_pageIs4;
            s_pageIs4 = PsxVram_PageIs4bpp(page);
            if (s_pageIs4 != was4)
                s_tlutDirty = 1;
            if (s_pageIs4)
                s_cnBinds4++;
        }
        s_pageSurf   = surface_make_linear((void*)page, s_pageIs4 ? FMT_CI4 : FMT_CI8,
                                           TEX_PAGE_DIM, TEX_PAGE_DIM);
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
    int x1 = x + w, y1 = y + h;

    /* The RDP has no render-target bounds: the scissor is the only thing
     * between a primitive and RDRAM, and rdpq accepts any rectangle up to
     * 1024x1024. A clip past the 320x240 surface therefore makes the RDP
     * rasterise straight into whatever the heap holds after the framebuffer
     * -- here the two 2KB rspq command queues rspq_init allocated right
     * after display_init's surfaces, where a black fill writes the zero
     * command words the RSP parks on (the watchdog "RSP crash" at rsp_queue
     * pc=018 = RSPQCmd_WaitNewInput). PutDrawEnv's sub-region clip is a PSX
     * VRAM rectangle transformed disp-relative, so it can extend past the
     * surface; clamp it. w/h <= 0 is gpu_xbox's "reset to the content rect",
     * which the Xbox GPU did implicitly and this backend used to ignore. */
    if (w <= 0 || h <= 0)
    {
        x = 0; y = 0; x1 = SCR_W; y1 = SCR_H;
    }
    if (x < 0)      x = 0;
    if (y < 0)      y = 0;
    if (x1 > SCR_W) x1 = SCR_W;
    if (y1 > SCR_H) y1 = SCR_H;
    if (x1 < x)     x1 = x;   /* entirely outside: empty scissor, nothing draws */
    if (y1 < y)     y1 = y;
    GpuNv2a_FlushBatch();
    rdpq_set_scissor(x, y, x1, y1);
    s_clipX0 = (float)x;
    s_clipY0 = (float)y;
    s_clipX1 = (float)x1;
    s_clipY1 = (float)y1;
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
