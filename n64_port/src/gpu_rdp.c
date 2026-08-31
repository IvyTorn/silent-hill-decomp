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
/* The texel budget is TMEM's lower half (the TLUT owns the upper): 2048 bytes,
 * one byte per CI8 texel, with each ROW padded to 8 bytes by the RDP's DMA.
 * The old check demanded a fixed 64x32 box, which rejected every tall-thin
 * sprite the title screen draws -- a 17x49 glyph strip is 1176 padded bytes
 * and fits with room to spare. Shape does not matter; padded area does. */
#define TEX_TMEM_BYTES 2048

static int TexBoxFits(int s0, int t0, int s1, int t1)
{
    int w    = s1 - s0;
    int h    = t1 - t0;
    int padW = (w + 7) & ~7;
    return w > 0 && h > 0 && padW * h <= TEX_TMEM_BYTES;
}

static const uint8_t*  s_texPage;
static const uint32_t* s_texPal;
/* The TLUT is DMA'd by the RDP when the queued load EXECUTES, not when it is
 * enqueued. One static table, rebuilt for the next palette while the previous
 * load was still queued, handed the earlier triangles the LATER palette --
 * Harry's parts flickering into each other's colours every frame. A ring gives
 * each upload its own memory; wrapping drains the queue first. */
#define TLUT_RING_N 32
static uint16_t        s_tlutRing[TLUT_RING_N][256];
static int             s_tlutIdx;
static uint16_t*       s_tlut = s_tlutRing[0];
static int             s_tlutDirty;
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

static void DrawTexturedTri(const ShVertex* a, const ShVertex* b, const ShVertex* c)
{
    float va[9], vb[9], vc[9];

    StageTexVert(va, a);
    StageTexVert(vb, b);
    StageTexVert(vc, c);
    DrawStagedTri(va, vb, vc, 7);
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
        s_cnTexTooBig++;
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
    {
        unsigned long long _t0 = get_ticks();
        int t;
        for (t = t0; t < t1; t++)
            data_cache_hit_writeback((void*)(s_texPage + t * TEX_PAGE_DIM + s0),
                                     (unsigned)(s1 - s0));
        rdpq_tex_upload_sub(TILE0, &s_pageSurf, NULL, s0, t0, s1, t1);
        rdpq_triangle(&TRIFMT_SH_TEX, va, vb, vc);
        s_cnSubmitTicks += get_ticks() - _t0;
        s_cnUploads++;
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
        {
            BuildTlut();
            /* Same cache rule as the page: the TLUT was just built by CPU
             * stores and the RDP will DMA it. */
            data_cache_hit_writeback(s_tlut, 256 * sizeof(uint16_t));
            rdpq_tex_upload_tlut(s_tlut, 0, 256);
            s_tlutDirty = 0;
        }

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
            rdpq_triangle(&TRIFMT_SH_SHADE,
                          (const float*)&s_batch[i],
                          (const float*)&s_batch[i + 1],
                          (const float*)&s_batch[i + 2]);
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
    {
        BuildTlut();
        data_cache_hit_writeback(s_tlut, 256 * sizeof(uint16_t));
        rdpq_tex_upload_tlut(s_tlut, 0, 256);
        s_tlutDirty = 0;
    }

    for (t = t0; t < t1; t++)
        data_cache_hit_writeback((void*)(s_texPage + t * TEX_PAGE_DIM + s0),
                                 (unsigned)(s1 - s0));

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
    s_modeDirty = 1;   /* combiner was changed; next flush re-applies */
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
    s_inited = 1;

    SH_DBG("[GPU] rdp up: %dx%d 16bpp x2, no z (painter's order from the OT)", SCR_W, SCR_H);
}

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
    s_cnTlutWrap = 0;
    s_cnBinds    = 0;
    g_PsxVramDecodes = g_PsxVramDrains = g_PsxVramPalBuilds = 0;
    g_PsxVramDecodeTicks = g_PsxVramDrainTicks = 0;
    s_modeDirty  = 1;

    {
        unsigned long long _t0 = get_ticks();
        s_fb = display_get();
        s_cnWaitFbTicks = get_ticks() - _t0;
    }
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
            SH_DBG("[PROF] frame=%uus waitFb=%uus submit=%uus uploads=%d binds=%d tlutWrap=%d | dec=%d/%uus drain=%d/%uus pal=%d | audio=%dms",
                   (unsigned)TICKS_TO_US((unsigned)g_Nv2aDrawCycles),
                   (unsigned)TICKS_TO_US((unsigned)s_cnWaitFbTicks),
                   (unsigned)TICKS_TO_US((unsigned)s_cnSubmitTicks),
                   s_cnUploads, s_cnBinds, s_cnTlutWrap,
                   g_PsxVramDecodes, (unsigned)TICKS_TO_US((unsigned)g_PsxVramDecodeTicks),
                   g_PsxVramDrains, (unsigned)TICKS_TO_US((unsigned)g_PsxVramDrainTicks),
                   g_PsxVramPalBuilds, g_ProfAudioMs);
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
        if (page != s_texPage)
            s_cnBinds++;
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
