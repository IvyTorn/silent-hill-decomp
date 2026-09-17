/*
 * t3d_world.c - the native world renderer: SHW chunks + SHT tiles on Tiny3D.
 *
 * Offline, mkworld.py compiles each map cell's IPD into a draw stream (SHW)
 * and the area's textures into TMEM-sized tiles (SHT). Here, a loaded chunk's
 * stream is recorded ONCE into rspq blocks (per model buffer, opaque and
 * semitransparent passes); per frame Ipd_ChunkDraw keeps its subcell PVS and
 * runs blocks instead of the per-prim GTE path. The RSP transforms, the CPU
 * only replays command lists.
 *
 * Space conventions (must match mkworld.py):
 *   - Vertices are s16 Q8 model-local, verbatim from the LM pools.
 *   - t3d world units are SH Q8 / 8 (i.e. 1/32 world unit): the 1/8 scale
 *     lives in the instance matrices so the largest exterior translation
 *     still fits a s16.16 matrix. The view matrix converts the same way.
 *   - The projection matrix reproduces the PSX pipe exactly:
 *       px = ((ofx + h*xv/zv) + OFS_X)*SCL_X + CONTENT_X
 *     with (h, ofx, ofy) read from the game at draw time and
 *     (OFS_X.., SCL_X..) the live draw-env transform gpu_xbox.c applies to
 *     every PSX prim -- so native world and PSX-path characters agree.
 *   - UVs are 10.5 tile-local texels, T3D's own unit.
 *
 * NO Z-BUFFER yet (mode_zbuf off; T3D_FLAG_DEPTH unset): world draws under
 * the whole OT (it runs before the walk), which layers characters over walls
 * until the Z stage lands. Draw order inside a chunk is the stream order.
 */
#include <libdragon.h>

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <t3d/t3d.h>
#include <t3d/t3dmath.h>

#include "sh_log.h"
#include "sh_t3d.h"

/* ------------------------------------------------------------- formats */

#define SHW_MAGIC 0x53485731u /* 'SHW1' */
/* bufferCount bit 15: baked with the area's GLB.PLM (mkworld --glb);
 * bit 14: OP_VERTS indices are cell-global (shared vertex windows). */
#define SHW_FLAG_GLB     0x8000u
#define SHW_FLAG_SHAREDV 0x4000u
#define SHW_COUNT_MASK   0x3FFFu
#define SHT_MAGIC 0x53485431u /* 'SHT1' */

#define OP_TILE   0x1
#define OP_MATRIX 0x2
#define OP_VERTS  0x3
#define OP_TRIS   0x4
#define OP_END    0xF

/* Diagnostic: draw the whole world as flat shaded (vertex colour), no texture,
 * to isolate geometry-projects from texture problems. 0 = real textured path. */
#define SH_T3DW_FLAT 0

/* Exteriors keep FOUR cells active (MapFlag_FourActiveChunks); interiors 1-2.
 * A cell the pool cannot place stays on the PSX fallback. */
#define MAX_WCHUNKS   4

/* Chunk data (verts + matrices + tile refs) lives in a FIXED per-slot arena,
 * not the heap: at map time the game runs the heap down to double digits and
 * fragments the rest, which first starved these allocations and then crashed
 * libdragon mid-block-recording (rspq_next_buffer memsets an unchecked
 * malloc). The arena caps what a cell may need; bigger cells stay PSX. */
/* 84K (was 80K): with its baked pickup object (SHOTGUN_) the reception cell
 * ERFE00 needs 80,688 B of hard carve; at 80K only 1.2K remained and its 191
 * group AABBs (2.3K, soft) no longer fit -> "background-only", losing the
 * foreground pass that occludes Harry behind the counter. Funded by trimming
 * the over-provisioned character arena (56K -> 48K for a 42K need). */
#define WCHUNK_ARENA_BYTES (84 * 1024)  /* 60K held 45/58 DECIMATED ER cells
                                         * but only 28 of the ORIGINAL bake
                                         * (the reception itself is 67.7K +
                                         * group centroids); 80K holds 43,
                                         * incl. ER0001/ERFE00 next door. The
                                         * +60K is paid for by psx_vram.c's
                                         * retired 64K swizzle staging. */

/* Every SHT tile is at most 2048 bytes BY CONSTRUCTION (the TMEM budget:
 * 4096 CI4 texels or 2048 CI8 texels, both 2KB), so tile pixels live in a
 * static fixed-slot pool -- 83 small heap mallocs per chunk fragmented the
 * heap until the 48KB vertex block could not be placed at 136KB free. */
#define TILE_SLOT_BYTES 2048
/* 128 slots (256KB): the police-station second room's chunk carries 122 unique
 * tiles, so a 96-slot pool loaded only ~20 of them and re-uploaded the rest
 * every frame (187 tile uploads/frame in [PROF], plus missing textures). Funded
 * by the ~232KB of kanji tables reclaimed on the USA cart (pc_kanji.c). */
#define TILE_SLOTS      128

typedef struct
{
    int16_t  sthIdx;    /* -1 = free */
    int16_t  refs;      /* character pool only (eager); world pool is LRU */
    uint8_t  fmt;       /* 0 = CI4, 1 = CI8 */
    uint8_t  pad;
    uint16_t w, h;
    uint32_t pixLen;
    uint32_t lastUse;   /* world pool: frame this tile was last DRAWN (LRU) */
    void*    pix;       /* slot in s_tilePool, written back for RDP DMA */
} WTile;

/* World tiles are loaded LAZILY (on first draw) and evicted LRU, so the pool
 * holds the working set -- the tiles actually drawn this frame -- instead of
 * every resident chunk's full tile set. Two big adjacent rooms (ERFE00 122 +
 * ERFF00 89 = 211 tiles) overran the 128-slot pool under the old eager
 * per-chunk residency: the second room got only ~52 of its 122 tiles, so its
 * wall groups tile-missed and drew NOTHING (the "missing walls"). */
static uint32_t s_tileFrame;   /* ++ per world flush; the LRU clock */

typedef struct
{
    /* Byte offsets into the chunk's arena cmd region; 0 words = empty. */
    uint32_t opaOff, semiOff;
    /* mkworld-tagged decals (posters/maps/signs lying 0..64 Q8 on a larger
     * parallel surface of another instance). Opaque, but drawn AFTER the
     * whole opaque batch: the RDP opaque Z compare passes anything within its
     * per-pixel dz tolerance, so an oblique wall drawn after its poster
     * repaints it -- and the tile-sorted batch order made that arbitrary. */
    uint32_t decalOff;
    uint16_t opaWords, semiWords, decalWords;
    uint16_t vbase;      /* first vert (pair-aligned) of this buffer */
    /* First OP_VERTS-group index of each stream in the chunk's groupPos
     * array; RunPass counts groups upward from here as it replays. */
    uint16_t opaGroupBase, semiGroupBase;
} WBuf;

/* Un-instanced LM models baked as world OBJECTS (mkworld OBJ1 trailer): the
 * *_HID item pickups game logic places at runtime. Measured <= 2 per cell. */
#define WOBJ_PER_CHUNK 8

typedef struct
{
    int            cellX, cellZ;
    int            inUse;
    int            bufCount;
    int            instCount;
    /* World objects: the LAST objCount buffers/instances, found by name at
     * draw time (ShT3d_WorldObjectDraw). 0 when the SHW has no trailer. */
    int            objCount, objBufBase, objInstBase;
    char           objNames[WOBJ_PER_CHUNK][8];
    WBuf*          bufs;       /* -> own arena slot */
    T3DVertPacked* verts;      /* -> own arena slot, cache-written-back */
    T3DMat4FP*     mats;       /* -> arena: 2 x instCount, composed PER FRAME
                                * through the game's own view path
                                * (ShT3d_ComposeInstanceView); s_matPhase picks
                                * the half the RSP isn't still replaying */
    int16_t*       rawRot;     /* -> arena: 9 s16/instance, Q12 file order */
    int32_t*       rawTrans;   /* -> arena: 3 s32/instance, Q8 world */
    float*         viewRow;    /* -> arena: 4 floats/instance -- composed view
                                * row 2 (R2/4096, t2/8): the painter's-split
                                * depth axis in the game's own view space */
    int16_t*       groupPos;   /* per-OP_VERTS-group LOCAL AABB, 6 s16 each
                                * (center xyz + half-extent xyz in the /8
                                * instance-local vert space), after the cmd
                                * streams; NULL = none fitted (all background) */
    int            groupCount;
    uint16_t*      tileRefs;   /* -> own arena slot, after mats */
    uint8_t*       cmds;       /* -> own arena slot: all passes' streams */
    uint8_t*       arena;      /* this chunk's block (pool or fixed arena) */
    uint32_t       arenaBytes; /* block size; pool chunks shrink to what they used */
    int            tileRefCount;
    int            triCount;   /* all passes, all buffers: the chunk's fill upper bound */
    int            glbBaked;   /* SHW_FLAG_GLB: global-PLM instances are native too */
    int            sharedVerts;/* SHW_FLAG_SHAREDV: every buffer's vbase is 0 */
} WChunk;

static WChunk  s_chunks[MAX_WCHUNKS];

/* World chunk memory: a best-fit pool over two fixed regions -- the old
 * 3 x 84K .bss slots merged into one block, plus the tail of the dead PSX
 * BODYPROG window (dso_n64.c, ~259K). Fixed 84K slots held only 4/32 SPR
 * and 23/128 THR cells (median 123-147K); the pool places whole cells
 * wherever a gap fits. Blocks are derived from the live chunks, so there is
 * no free list to corrupt. */
#define WPOOL_BSS_BYTES (3 * WCHUNK_ARENA_BYTES)
static uint8_t s_wpoolBss[WPOOL_BSS_BYTES] __attribute__((aligned(16)));
typedef struct { uint8_t* base; uint32_t bytes; } WRegion;
static WRegion s_wreg[3];
static int     s_wregCount;

/* Cells the pool could not place, retried when a chunk is evicted. */
#define WPENDING_MAX 8
typedef struct { char base[16]; int cellX, cellZ; uint32_t need; } WPending;
static WPending s_wpending[WPENDING_MAX];
static int      s_wpendingCount;
static WTile   s_tiles[TILE_SLOTS];
static uint8_t s_tilePool[TILE_SLOTS][TILE_SLOT_BYTES] __attribute__((aligned(16)));
static int     s_tileRam;

/* ---- native character store (Phase C2): ONE resident character (Harry).
 * A character .ILM is 23 RIGID parts, one per bone (no vertex skinning), so
 * a part maps onto a world instance: geometry baked by mkchara.py, the
 * per-frame matrix is the game's own BONE view matrix, written by the bone
 * loop (ShT3d_CharaBone) instead of composed from the IPD. Its 21 tiles are
 * eager-resident here in their own pool so the area SHT's slot/palette
 * namespaces stay untouched across map changes; its parts live in a fixed
 * arena as a WChunk. Static .bss (~84KB) -- heap headroom is the constraint
 * on this machine, and these never move. */
/* HERO.SHW with the six baked weapons needs 34 tiles (the guns/blades pull in
 * HERO.TIM regions the body never touched); the loader DROPS tiles past the
 * slot count, so 22 would silently untexture them. Static .bss (+28KB). */
#define CTILE_SLOTS      36
/* HERO.SHW with the six baked weapons is 37.4KB; the loader's hard carve
 * (verts + 2 matrix phases + viewRow + bufs + cmds) measures 41,488 B, plus
 * ~1.5K of soft group AABBs -> ~42K. The old 40K (sized for the 25K body-only
 * bake) would overflow and drop Harry to the PSX path; 48K leaves 6K. Static
 * .bss, costs no heap -- the 8K trimmed from an earlier 56K funds the world
 * chunk arena's 84K. */
#define CHAR_ARENA_BYTES (48 * 1024)
static WTile     s_cTiles[CTILE_SLOTS];
static uint8_t   s_cPool[CTILE_SLOTS][TILE_SLOT_BYTES] __attribute__((aligned(16)));
static int       s_cTileCount;
static uint16_t* s_cPals;
static uint32_t* s_cPalOffsets;
static uint16_t* s_cPalWords;
static int       s_cPalCount;
static WChunk    s_charChunk;
static uint8_t   s_charArena[CHAR_ARENA_BYTES] __attribute__((aligned(16)));
static int       s_charLoaded, s_charLoadTried;
/* Per-part LOCAL geometric centroid (raw vert space, matches CharaBone's raw
 * translation). The painter's sort keys on the part's mass, not its bone
 * joint: a gun extends forward from a hand whose joint sits beside the torso,
 * so a joint-Z key sorts it with the torso and it clips through -- exactly the
 * PSX ordering-table's per-primitive depth is what this approximates. */
static int16_t   s_charPartCent[32][3];
static int       s_cnRunPassTris;   /* [T3DCB2] probe: tris t3d_tri_draw'd */

/* Tile-bind dedup: the command stream re-emits OP_TILE for a tile it already
 * used in an earlier BUFFER or in the other half of the fg/bg split, so the
 * same (tref,pal) is uploaded to TMEM several times per frame for nothing.
 * Track the last SUCCESSFUL bind and skip an identical one -- the tile is
 * still resident (RunPass draws never touch TMEM between binds). Reset per
 * flush pass in WorldFrameStart, because the PSX OT walk between passes
 * clobbers TMEM. */
static uint16_t  s_lastBoundTref, s_lastBoundPal;
static int       s_lastBoundOk;
static int       s_cnTileUp, s_cnTileDedup;   /* per-frame: tile uploads vs deduped */
static int       s_charActive;      /* inside a native character's bone loop */
static int       s_charDiagMode = -1;   /* auto-cycled: 0 textured, 1 flat solid */
static int       s_charPhase;       /* double-buffered part matrices */
static uint32_t  s_charMask;        /* parts the animation wrote this frame */
static T3DMat4FP s_charHidden;      /* collapses an unwritten part behind the eye */

/* Held weapons: mkchara --weapon bakes the six HERO-textured ITEM PLMs into
 * HERO.SHW as extra parts after the body (knife hammer axe handgun rifle
 * shotgun -- the slot order, see build_chara.sh), each its own buffer AND
 * instance. The equipped one is drawn by CharaFlush with the right-hand
 * bone's matrix, so it rides Harry's Z-buffer instead of the depth-less PSX
 * OT that used to paint the gun over his hip from any side. The count rides
 * in the SHW header's cellZ byte (-128 + N) so an older bake reads 0. */
#define CHAR_WEAPON_COUNT     6
#define CHAR_RIGHT_HAND_BONE 10     /* HarryBone_RightHand: every 10RHAND* variant's bone */
static int       s_charWeaponBase = -1;  /* first weapon part index, -1 = none baked */
static int       s_charRHandPart  = -1;  /* the 10RHAND* part the bone loop wrote THIS frame */
static int       s_charHeldWeapon = -1;  /* slot to draw natively this frame, -1 = none */

static FILE*   s_sht;             /* area tile store, kept open */
static char    s_shtPrefix[8];
static uint16_t s_shtTileCount;
static uint16_t s_shtPalCount;
static uint16_t* s_pals;          /* all palettes resident, uncached */
static uint32_t* s_palOffsets;    /* file offset + word count per palette */
static uint16_t* s_palWords;

static T3DViewport s_wvp;
/* Same camera with the depth row shifted nearer by s_decalBias tiny3d depth
 * steps, attached for the decal pass only. tiny3d hands the RDP a 16-bit
 * per-vertex depth, so a fixed separation is resolved only up to a distance
 * (~2 Q8 of poster-to-wall resolves to ~6 m at near=4, 4 Q8 to ~8 m); a
 * constant NDC offset is a polygon offset that holds at every distance and
 * lets a decal win only over surfaces within a few depth steps of it. */
static T3DViewport s_wvpDecal;
static int         s_wvpDecalInited;
static int         s_decalBias;
static int     s_wvpInited;
static int     s_frameActive;     /* between NotifyFrameBegin/End */
static int     s_worldStarted;    /* per-frame world state applied */

/* one-shot + census diagnostics */
static int     s_logOnce;
static int     s_cnBlocks, s_cnFallback;

/* Projection planes (see the nearP comment in WorldFrameStart).
 *
 * FAR IS NOT FREE HERE. tiny3d normalises clip W by k = 2/(far+near) and
 * hands the ucode fixed-point copies of it: W scale round(65535k)/65536,
 * screen scale round(size*64k), depth scale round(65535*8k). At far=50000
 * those came out 3/65536 (vs 2.62), 1 (vs 0.82) and 21 -- so the Z-buffer
 * used ~1/24 of its range (the poster z-fighting that survived vertex
 * precision, decal ordering and every near plane) and native geometry drew
 * ~6.7% larger on screen than the PSX path. far+near = 2048 makes k exactly
 * 1/1024: W 64, screen 20 (320) / 15 (240), depth 512 -- all exact. 2044
 * t3d units is 64 m; fogged areas cull at ~15 m. */
#define WORLD_PROJ_NEAR 4.0f
#define WORLD_PROJ_FAR  2044.0f

/* N64 hardware fog for the native world and Harry: tiny3d writes a per-vertex
 * fog factor into shade alpha on the RSP, the RDP fog blender mixes in the
 * area's fog colour. Fed every frame from g_WorldEnvWork (ShT3d_WorldFogSet).
 * The game's ramp (WorldEnv_FogDistanceSet, curve D_800AE1C0) is 0 until 1/16
 * of fog.nearDistance and ~94% by 13/16; a linear 6%..80% span tracks it. */
static int     s_fogOn;
static uint8_t s_fogRgb[3];
static float   s_fogFullZ;      /* view depth (t3d units) of 100% fog */
/* Groups whose nearest point is beyond this are skipped: fully fogged
 * geometry over a fog-coloured clear is invisible, and at ~14 m outdoor fog a
 * 40 m cell is mostly past it -- the RDP fill the frame was spending. 0 = off
 * (no fog, or a clear colour that is not the fog colour). */
static float   s_fogCullZ;
static int     s_cnFogCulled;
/* The game's fog.intensity (Q12) is added to every PSX-path fog factor on top
 * of the ramp; as a fraction of the ramp span it shifts the native range. */
static float   s_fogShift;
static int     s_fogSetThisFrame;
#define WORLD_FOG_CULL_MARGIN 16.0f   /* t3d units (~0.5 m) past the draw distance */

void ShT3d_WorldFogSet(int enabled, int r, int g, int b, int fullQ8, int drawQ8,
                       int intensityQ12)
{
    extern int PcConfig_N64WorldFog(void);
    extern unsigned int GpuXbox_GetClearColor(void);
    unsigned clear;
    int dr, dg, db;

    extern int g_N64FogBlend;   /* gpu_rdp.c: PSX-path opaque prims fog too */

    s_fogOn    = enabled && fullQ8 > 0 && PcConfig_N64WorldFog();
    s_fogCullZ = 0.0f;
    s_fogSetThisFrame = 1;
    g_N64FogBlend = 0;
    if (!s_fogOn)
        return;
    if (intensityQ12 < -4096) intensityQ12 = -4096;
    if (intensityQ12 >  4096) intensityQ12 =  4096;
    s_fogShift = (float)intensityQ12 / 4096.0f;
    s_fogRgb[0] = (uint8_t)r;
    s_fogRgb[1] = (uint8_t)g;
    s_fogRgb[2] = (uint8_t)b;
    s_fogFullZ  = (float)fullQ8 / 8.0f;
    clear = GpuXbox_GetClearColor();
    dr = (int)((clear >> 16) & 0xFF) - r;
    dg = (int)((clear >> 8) & 0xFF) - g;
    db = (int)(clear & 0xFF) - b;
    if (dr >= -12 && dr <= 12 && dg >= -12 && dg <= 12 && db >= -12 && db <= 12)
        s_fogCullZ = (float)(drawQ8 > fullQ8 ? drawQ8 : fullQ8) / 8.0f + WORLD_FOG_CULL_MARGIN;
    /* 2 = the clear IS the fog colour, so fully fogged PSX prims may be
     * dropped too; 1 = fog, but a dropped prim would leave a hole. */
    g_N64FogBlend = s_fogCullZ > 0.0f ? 2 : 1;
}

/* tiny3d's RSP fog is linear in CLIP-space z: 0 at 2*near, full at 2*far
 * (rsp_tiny3d.rspl: fog = (z_clip - 2n) * 16384/(f-n), saturated). This
 * projection gives z_clip = A*d + B for view distance d, so a span [ds, de]
 * in distance maps to near = (A*ds+B)/2, far = (A*de+B)/2. */
static void WorldFogApply(void)
{
    if (!s_fogOn)
    {
        t3d_fog_set_enabled(false);
        rdpq_mode_fog(0);
        return;
    }
    {
        float n = WORLD_PROJ_NEAR, f = WORLD_PROJ_FAR;
        float A = f / (f - n);
        float B = -2.0f * f * n / (f - n);
        /* 4%..75% stays within ~5 points of D_800AE1C0 over 30..70%, where
         * 6%..80% ran 9-13 points thin against the PSX-path prims. */
        float ds = s_fogFullZ * 0.04f, de = s_fogFullZ * 0.75f;
        float sh = (de - ds) * s_fogShift;
        ds -= sh;
        de -= sh;
        t3d_fog_set_range((A * ds + B) * 0.5f, (A * de + B) * 0.5f);
    }
    t3d_fog_set_enabled(true);
    /* rdpq adjusts the standard TEX_SHADE/SHADE combiners so shade alpha (now
     * fog) stops modulating the texture alpha test. */
    rdpq_mode_fog(RDPQ_FOG_STANDARD);
    rdpq_set_fog_color(RGBA32(s_fogRgb[0], s_fogRgb[1], s_fogRgb[2], 0xFF));
}

/* Chunk loads that fail RETRY every frame (the FixOffsets maintenance loop
 * calls the hook per loaded chunk per frame); log the first few and then a
 * heartbeat, or the ring drowns. */
static int WFailLog(void)
{
    static int n;
    n++;
    return n <= 12 || (n & 511) == 0;
}

int ShT3d_Ready(void);            /* from t3d_n64.c */

/* Camera, COPIED (not aliased) at Ipd_ChunkDraw time. GsWSMATRIX is mutated
 * by every later object draw (characters), so by the deferred FrameEnd flush
 * the pointer would read a character's matrix. s_wm = view ROTATION; the
 * camera POSITION comes separately from D_800C3868.t (GsWSMATRIX.t is zero on
 * this port -- deriving eye from it put the camera at the origin). Q8 world. */
static int16_t s_wm[9];
static int32_t s_camPos[3];
static int32_t s_playerPos[3];   /* Q8 world; the painter's-split reference */
static int s_haveView;
static int s_geomH, s_geomOfx, s_geomOfy;

/* The player's view-space depth (world/8), composed once per frame in
 * WorldViewSet through the game's own view path; RunPass classifies each
 * group as foreground (nearer than this) or background against it. */
static float s_playerViewZ;
static int   s_zActive;   /* Z-buffer live for the WORLD this frame (zbuffer=1) -> world writes depth */
static int   s_zChara;    /* Z-buffer live for the character (zbuffer>=1): parts self-occlude per pixel */

/* Deferred draw list: (cell, buf) recorded during OT build, drawn at the
 * GsDrawOt point. The accumulation window is bounded by FLUSH, not by
 * FrameBegin: the game calls VSync (-> FrameBegin) BETWEEN the OT build and
 * GsDrawOt, so resetting the list on FrameBegin wiped the records before the
 * flush ever saw them. Instead the first WorldViewSet after a flush clears
 * the list. */
#define WORLD_DRAWLIST_MAX 64
static struct { int16_t cx, cz, buf; } s_drawList[WORLD_DRAWLIST_MAX];
static int s_drawCount;
static int s_flushed = 1;   /* 1 => next WorldViewSet starts a fresh list */
static int s_matPhase;      /* which half of each chunk's mats this frame
                             * composes into (the RSP may still be replaying
                             * the previous frame's half) */

/* Compose every instance's view matrix for this frame THROUGH THE GAME'S OWN
 * code (ShT3d_ComposeInstanceView -> Vw_CoordToWorldAndViewMatrices): the
 * result carries VbWvsMatrix -- with its 3/4 Y NTSC scale that GsWSMATRIX
 * does not have -- and the camera subtract, so the native world lands
 * EXACTLY where the per-prim path would land it. t3d then runs with an
 * identity camera (axis flip only, in WorldFrameStart's look_at). */
static void ComposeChunkViews(WChunk* c)
{
    T3DMat4FP* dst = c->mats + s_matPhase * c->instCount;
    int i, r, cc;

    for (i = 0; i < c->instCount; i++)
    {
        short vR[9];
        int   vT[3];
        T3DMat4 m;
        ShT3d_ComposeInstanceView((const short*)(c->rawRot + i * 9),
                                  (const int*)(c->rawTrans + i * 3), vR, vT);
        memset(&m, 0, sizeof m);
        /* Verts are raw Q8: the matrix carries the whole 1/8 (rotation and
         * translation), keeping view space in the small /8 fixed-point-safe
         * range at full vertex precision. viewRow stays unscaled: it is
         * applied to group bounds already in /8 space. */
        for (r = 0; r < 3; r++)
            for (cc = 0; cc < 3; cc++)
                m.m[cc][r] = (float)vR[r * 3 + cc] / 4096.0f / 8.0f;
        m.m[3][0] = (float)vT[0] / 8.0f;
        m.m[3][1] = (float)vT[1] / 8.0f;
        m.m[3][2] = (float)vT[2] / 8.0f;
        m.m[3][3] = 1.0f;
        t3d_mat4_to_fixed(&dst[i], &m);
        c->viewRow[i * 4 + 0] = (float)vR[6] / 4096.0f;
        c->viewRow[i * 4 + 1] = (float)vR[7] / 4096.0f;
        c->viewRow[i * 4 + 2] = (float)vR[8] / 4096.0f;
        c->viewRow[i * 4 + 3] = (float)vT[2] / 8.0f;
    }
    if (c->instCount > 0)
        data_cache_hit_writeback(dst, sizeof(T3DMat4FP) * c->instCount);
}

static void ShT3d_MemCheck(const char* where);

void ShT3d_WorldViewSet(const void* wsMatrix, int camX, int camY, int camZ,
                        int plX, int plY, int plZ,
                        int h, int ofx, int ofy)
{
    const int16_t* m = (const int16_t*)wsMatrix;
    int i;
    int firstOfFrame = 0;
    /* OT-build time: everything in the game-logic phase (player physics, the
     * collision ray-trace whose GTE call crashed, streaming) has already run
     * this frame. If the tripwire fires HERE but was clean at the prior frame's
     * render, the wild writer lives in that phase. */
    ShT3d_MemCheck("viewset(post-logic)");
    /* First camera handoff after a flush = start of a new frame's world;
     * clear the draw list here rather than on FrameBegin (which the game's
     * mid-frame VSync fires between the OT build and the GsDrawOt flush). */
    if (s_flushed)
    {
        s_drawCount = 0;
        s_flushed = 0;
        firstOfFrame = 1;
    }
    for (i = 0; i < 9; i++)
        s_wm[i] = m[i];
    s_camPos[0] = camX; s_camPos[1] = camY; s_camPos[2] = camZ;
    s_playerPos[0] = plX; s_playerPos[1] = plY; s_playerPos[2] = plZ;
    s_geomH   = h;
    s_geomOfx = ofx;
    s_geomOfy = ofy;
    s_haveView = 1;

    /* Once per frame, while the camera globals are the mesh-path values:
     * compose every resident chunk's instance view matrices and the player's
     * view depth through the game's own view path. */
    if (firstOfFrame)
    {
        static const short idR[9] = { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 };
        s_tileFrame++;   /* LRU clock: one tick per drawn frame (all passes share it) */
        s_matPhase ^= 1;
        for (i = 0; i < MAX_WCHUNKS; i++)
            if (s_chunks[i].inUse)
                ComposeChunkViews(&s_chunks[i]);
        if (plX == 0 && plY == 0 && plZ == 0)
        {
            /* No valid player yet: sentinel below every depth = nothing is
             * foreground, the whole world draws in the background pass. */
            s_playerViewZ = -1.0e30f;
        }
        else
        {
            short dR[9];
            int   pW[3], pT[3];
            pW[0] = plX; pW[1] = plY; pW[2] = plZ;
            ShT3d_ComposeInstanceView(idR, pW, dR, pT);
            s_playerViewZ = (float)pT[2] / 8.0f;
            /* [PROJCHK] (memwatch=1): does the PSX item path (GsWSMATRIX) put a
             * world point where the native world (VbWvsMatrix) does? nativeV vs
             * psxV are the SAME point's view coords through each path -- a
             * mismatch is the item drift. h/oX/oY/sX/sY/cX are the shared
             * projection the native proj is built to match PutVert with. */
            {
                extern int GpuNv2a_MemWatch(void);
                static int s_pc;
                if (GpuNv2a_MemWatch() && (s_pc++ & 31) == 0)
                {
                    extern void ShT3d_WorldScreenViewGet(int, int, int, int*);
                    extern void GpuXbox_GetViewTransform(float*, float*, float*, float*, int*);
                    int   psxT[3];
                    float oX, oY, sX, sY;
                    int   cX;
                    ShT3d_WorldScreenViewGet(plX, plY, plZ, psxT);
                    GpuXbox_GetViewTransform(&oX, &oY, &sX, &sY, &cX);
                    SH_DBG("[PROJCHK] nativeV=%d,%d,%d psxV=%d,%d,%d | h=%d oX=%d oY=%d sX=%d sY=%d cX=%d cam=%d,%d,%d",
                           pT[0], pT[1], pT[2], psxT[0], psxT[1], psxT[2],
                           s_geomH, (int)oX, (int)oY, (int)(sX * 100.0f), (int)(sY * 100.0f), cX,
                           (int)s_camPos[0], (int)s_camPos[1], (int)s_camPos[2]);
                }
            }
        }
    }
}

/* ------------------------------------------------------------- helpers */

/* SD first (mods), then the cart. root = "N64W" (world) or "N64C" (chars). */
static FILE* WOpenRoot(const char* root, const char* rel)
{
    char path[96];
    FILE* f;
    snprintf(path, sizeof path, "sd:/silenthill/gamedata/load/%s/%s", root, rel);
    f = fopen(path, "rb");
    if (f == NULL)
    {
        snprintf(path, sizeof path, "rom:/%s/%s", root, rel);
        f = fopen(path, "rb");
    }
    return f;
}

static FILE* WOpen(const char* rel) { return WOpenRoot("N64W", rel); }

static uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* -------------------------------------------------------------- tiles */

static int TileSlotFind(int sthIdx)
{
    int i;
    for (i = 0; i < (int)(sizeof s_tiles / sizeof s_tiles[0]); i++)
        if (s_tiles[i].sthIdx == sthIdx)
            return i;
    return -1;
}

/* LAZY, LRU. Returns the slot holding sthIdx, loading it on demand and, when
 * the pool is full, evicting the least-recently-DRAWN tile that was not needed
 * THIS frame. A tile the current frame still needs is never evicted -- if the
 * whole frame's working set exceeds the pool, the extra tile misses (drops)
 * instead. Called from BindWorldTile at draw time; no chunk-level refcount. */
static int TileAcquire(int sthIdx)
{
    uint8_t meta[16];
    int slot, i;
    WTile* t;

    slot = TileSlotFind(sthIdx);
    if (slot >= 0)
    {
        s_tiles[slot].lastUse = s_tileFrame;   /* touch for LRU */
        return slot;
    }
    if (s_sht == NULL || sthIdx >= s_shtTileCount)
        return -1;

    slot = -1;
    for (i = 0; i < TILE_SLOTS; i++)
        if (s_tiles[i].sthIdx < 0) { slot = i; break; }
    if (slot < 0)
    {
        uint32_t oldest = s_tileFrame;   /* only tiles NOT drawn this frame */
        for (i = 0; i < TILE_SLOTS; i++)
            if (s_tiles[i].lastUse < oldest) { oldest = s_tiles[i].lastUse; slot = i; }
        if (slot < 0)
        {
            static int s_full;
            if ((s_full++ & 511) == 0)
                SH_DBG("[T3DW] tile working set > %d (frame %u) -- doorway overlap",
                       TILE_SLOTS, (unsigned)s_tileFrame);
            return -1;
        }
        s_tileRam -= s_tiles[slot].pixLen;   /* evict the LRU tile */
        s_tiles[slot].sthIdx = -1;
        s_tiles[slot].pix    = NULL;
    }

    fseek(s_sht, 8 + sthIdx * 16, SEEK_SET);
    if (fread(meta, 1, 16, s_sht) != 16)
        return -1;

    /* SHT tile meta: fmt@0, w@2, h@4, pixOff@8, pixLen@12 (all BE). */
    t = &s_tiles[slot];
    t->fmt    = meta[0];
    t->w      = rd16(meta + 2);
    t->h      = rd16(meta + 4);
    {
        uint32_t pixOff = rd32(meta + 8);
        uint32_t pixLen = rd32(meta + 12);
        if (pixLen > TILE_SLOT_BYTES)
        {
            SH_DBG("[T3DW] tile %d oversize %u -- converter bug", sthIdx, (unsigned)pixLen);
            return -1;
        }
        t->pix = s_tilePool[slot];
        fseek(s_sht, pixOff, SEEK_SET);
        if (fread(t->pix, 1, pixLen, s_sht) != pixLen)
        {
            t->pix = NULL;
            return -1;
        }
        t->pixLen = pixLen;
    }
    data_cache_hit_writeback(t->pix, t->pixLen);
    t->sthIdx  = sthIdx;
    t->lastUse = s_tileFrame;
    s_tileRam += t->pixLen;
    return slot;
}

/* The shared item-pickup models (BG/BG_ITEM.PLM: ammo box, shells, health
 * drink, first-aid kit, ampoule, notepad) are what the game's global-pool
 * world objects (lmIdx 2) draw. mkworld --items bakes them as an objects-
 * only <PREFIX>ITEM.SHW pseudo-cell (tiles in the area SHT) with header cell
 * -127,-127; it lives here, always resident for the area, in its own small
 * arena outside the s_chunks slots -- ShT3d_WorldObjectDraw searches it too.
 * Need (ShwLoadBody formula) for the 7 models / 69 tris is ~4K. */
#define WOBJ_ITEM_CELL (-127)
static WChunk  s_itemChunk;
static uint8_t s_itemArena[8 * 1024] __attribute__((aligned(16)));
static void    ItemChunkLoad(const char* prefix);

static int s_palNeedKB;

static int ShtOpen(const char* prefix)
{
    uint8_t hdr[8];
    char name[16];
    int i;

    if (s_sht != NULL && strcmp(prefix, s_shtPrefix) == 0)
        return 1;
    if (s_sht != NULL)
        ShT3d_WorldReset();

    snprintf(name, sizeof name, "%s.SHT", prefix);
    s_sht = WOpen(name);
    if (s_sht == NULL)
        return 0;
    if (fread(hdr, 1, 8, s_sht) != 8 || rd32(hdr) != SHT_MAGIC)
    {
        fclose(s_sht);
        s_sht = NULL;
        return 0;
    }
    s_shtTileCount = rd16(hdr + 4);
    s_shtPalCount  = rd16(hdr + 6);
    strncpy(s_shtPrefix, prefix, sizeof s_shtPrefix - 1);

    /* Palettes stay resident: every block references them (ER 27 KB, SPR
     * 13 KB, THR 39 KB with the index). Allocated right after a transition
     * refilled the PSX texture cache, so the heap can be short: a failed
     * allocation turns the native world off for this area (PSX fallback)
     * instead of freading into NULL. */
    s_palOffsets = malloc(s_shtPalCount * 4);
    s_palWords   = malloc(s_shtPalCount * 2);
    if (s_palOffsets == NULL || s_palWords == NULL)
        goto palFail;
    {
        int total = 0;
        fseek(s_sht, 8 + s_shtTileCount * 16, SEEK_SET);
        for (i = 0; i < s_shtPalCount; i++)
        {
            uint8_t pm[8];
            fread(pm, 1, 8, s_sht);
            s_palWords[i]   = rd16(pm);
            s_palOffsets[i] = rd32(pm + 4);
            total += (s_palWords[i] + 3) & ~3;
        }
        s_pals = malloc_uncached(total * 2);
        if (s_pals == NULL)
        {
            s_palNeedKB = (total * 2 + s_shtPalCount * 6) / 1024;
            goto palFail;
        }
        total = 0;
        for (i = 0; i < s_shtPalCount; i++)
        {
            fseek(s_sht, s_palOffsets[i], SEEK_SET);
            fread((uint8_t*)s_pals + total * 2, 1, s_palWords[i] * 2, s_sht);
            s_palOffsets[i] = total;   /* now: word offset into s_pals */
            total += (s_palWords[i] + 3) & ~3;
        }
    }
    SH_DBG("[T3DW] %s.SHT open: %d tiles, %d palettes", prefix, s_shtTileCount, s_shtPalCount);
    ItemChunkLoad(prefix);   /* the area's shared item-pickup models, if baked */
    return 1;

palFail:
    {
        extern unsigned Xbox_MemFreeKB(void);
        static char s_failedPrefix[8];
        if (strcmp(s_failedPrefix, prefix) != 0)
        {
            snprintf(s_failedPrefix, sizeof s_failedPrefix, "%s", prefix);
            SH_DBG("[T3DW] %s.SHT: palette alloc failed (need ~%dKB, %uKB free) -- native world off, PSX fallback",
                   prefix, s_palNeedKB, Xbox_MemFreeKB());
        }
    }
    ShT3d_WorldReset();
    return 0;
}

/* ------------------------------------------------------------- chunks */

/* Optional OBJ1 trailer (mkworld collect_objects / collect_item_models): the
 * LAST objCount buffers/instances are name-keyed world objects -- the *_HID
 * item pickups a cell's game logic places at runtime, or every model of the
 * item pseudo-cell -- 8-char names at namesOff, 12-byte trailer at EOF. A
 * file without it (older bake) has no objects; those pickups stay PSX. */
static void ChunkReadObjTrailer(WChunk* c, FILE* f, int bufCount, int instCount)
{
    uint8_t tr[12];
    c->objCount = 0;
    if (fseek(f, -12, SEEK_END) == 0 && fread(tr, 1, 12, f) == 12 &&
        rd32(tr) == 0x4F424A31u /* 'OBJ1' */)
    {
        int      n        = rd16(tr + 4);
        uint32_t namesOff = rd32(tr + 8);
        if (n > 0 && n <= WOBJ_PER_CHUNK && n <= bufCount && n <= instCount &&
            fseek(f, (long)namesOff, SEEK_SET) == 0 &&
            fread(c->objNames, 1, 8 * n, f) == (size_t)(8 * n))
        {
            c->objCount    = n;
            c->objBufBase  = bufCount - n;
            c->objInstBase = instCount - n;
        }
    }
}

/* A resident chunk's arena pointers must all land inside its own pool block,
 * and the block inside a pool region. A fixed-slot arena that ended where
 * s_chunks began once let an overrun rewrite a chunk header (seen: bufs=0x3c,
 * a NULL deref in WorldFlushPass reading c->bufs[b]). Validate before every
 * use so a corrupt chunk is dropped for the frame instead of crashing, and
 * log it once so the next hardware run names which cell overran. */
static int ChunkArenaValid(const WChunk* c)
{
    const uint8_t* base;
    const uint8_t* end;
    const uint8_t* p[4];
    int i, inRegion = 0;
    if (c < s_chunks || c >= s_chunks + MAX_WCHUNKS)
        return 0;                       /* not a world slot (character chunk) */
    base = c->arena;
    end  = base + c->arenaBytes;
    for (i = 0; i < s_wregCount; i++)
        if (base >= s_wreg[i].base && end <= s_wreg[i].base + s_wreg[i].bytes)
            inRegion = 1;
    if (base == NULL || !inRegion)
        return 0;
    if (c->bufCount <= 0 || c->bufCount > 64 || c->instCount < 0 || c->instCount > 256)
        return 0;
    p[0] = (const uint8_t*)c->bufs;
    p[1] = (const uint8_t*)c->verts;
    p[2] = (const uint8_t*)c->cmds;
    p[3] = (const uint8_t*)c->mats;
    for (i = 0; i < 4; i++)
        if (p[i] < base || p[i] >= end)
            return 0;
    return 1;
}

/* Wild-writer tripwire (config memwatch=1). Two hardware crashes -- the
 * misaligned GTE store (gteRegs corrupt) and the RSP "read a zero word" halt
 * (rspq DRAM pointer corrupt) -- are BOTH stray-pointer writes to memory the
 * native renderer proves it never touches out of bounds. gteRegs and s_chunks
 * are .bss neighbours, so validate s_chunks (the closest reliably-checkable
 * victim) at each per-frame phase boundary: the FIRST phase that sees it
 * corrupt names the subsystem that wrote it (e.g. collision runs before the
 * OT-build WorldViewSet). Latches per episode so a persistent stomp logs once,
 * then re-arms when it reads clean again. Zero cost when memwatch=0. */
static void ShT3d_MemCheck(const char* where)
{
    static int s_tripped;
    int i, bad = -1, freeBad = -1;

    extern int GpuNv2a_MemWatch(void);
    if (!GpuNv2a_MemWatch())
        return;

    for (i = 0; i < MAX_WCHUNKS; i++)
    {
        WChunk* c = &s_chunks[i];
        if (c->inUse)
        {
            /* An in-use slot must resolve cleanly and carry a plausible cell. */
            if (!ChunkArenaValid(c) || c->cellX < -128 || c->cellX > 127 ||
                c->cellZ < -128 || c->cellZ > 127)
            { bad = i; break; }
        }
        else
        {
            /* A free slot is memset(0) at ChunkFree: any live pointer/count in
             * it means something wrote the header from outside the renderer. */
            if (c->bufs != NULL || c->bufCount != 0 || c->instCount != 0 ||
                c->cellX != 0 || c->cellZ != 0)
            { freeBad = i; break; }
        }
    }

    if (bad < 0 && freeBad < 0)
    {
        s_tripped = 0;   /* clean -- re-arm for the next distinct episode */
        return;
    }
    if (!s_tripped)
    {
        int i2 = bad >= 0 ? bad : freeBad;
        s_tripped = 1;
        SH_DBG("[MEMWATCH] CORRUPT @%s slot=%d %s cell=%d,%d bufs=%p bc=%d ic=%d",
               where, i2, bad >= 0 ? "inUse" : "free-stomped",
               s_chunks[i2].cellX, s_chunks[i2].cellZ,
               (void*)s_chunks[i2].bufs, s_chunks[i2].bufCount, s_chunks[i2].instCount);
    }
}

static void WPoolInit(void)
{
    extern int ShN64_PsxWindowTail(uint8_t** base, uint32_t* bytes);
    extern int ShN64_PsxLowRegion(uint8_t** base, uint32_t* bytes);
    uint8_t* b;
    uint32_t n, win = 0, low = 0;
    if (s_wregCount != 0)
        return;
    s_wreg[0].base  = s_wpoolBss;
    s_wreg[0].bytes = sizeof s_wpoolBss;
    s_wregCount     = 1;
    if (ShN64_PsxWindowTail(&b, &n) && n >= 32 * 1024)
    {
        s_wreg[s_wregCount].base  = b;
        s_wreg[s_wregCount].bytes = win = n & ~(uint32_t)15;
        s_wregCount++;
    }
    if (ShN64_PsxLowRegion(&b, &n) && n >= 32 * 1024)
    {
        s_wreg[s_wregCount].base  = b;
        s_wreg[s_wregCount].bytes = low = n & ~(uint32_t)15;
        s_wregCount++;
    }
    SH_DBG("[T3DW] chunk pool: bss %uK + PSX window %uK + low PSX RAM %uK",
           (unsigned)(s_wreg[0].bytes / 1024), (unsigned)(win / 1024), (unsigned)(low / 1024));
}

/* dso_n64.c is about to place a map module at [limit, window end): shrink
 * the region that holds `limit` and evict any chunk reaching past it. Normally
 * a no-op -- ShT3d_WorldReset already emptied the pool and reset the regions. */
static void ChunkFree(WChunk* c);

void ShT3d_WorldPoolClamp(uint8_t* limit)
{
    int r, i;
    for (r = 0; r < s_wregCount; r++)
    {
        uint8_t* b = s_wreg[r].base;
        if (limit <= b || limit > b + s_wreg[r].bytes + (256u * 1024u))
            continue;
        for (i = 0; i < MAX_WCHUNKS; i++)
        {
            WChunk* c = &s_chunks[i];
            if (c->inUse && c->arena != NULL && c->arena >= b &&
                c->arena + c->arenaBytes > limit)
            {
                SH_DBG("[T3DW] pool chunk %d,%d overlaps the incoming map module -- evicted",
                       c->cellX, c->cellZ);
                rspq_wait();
                ChunkFree(c);
            }
        }
        s_wreg[r].bytes = (uint32_t)(limit - b) & ~(uint32_t)15;
    }
}

/* Walk each region's gaps between live chunks in address order; best fit for
 * `need` (0 = only report the largest gap through *largest). */
static uint8_t* WPoolScan(uint32_t need, uint32_t* gapOut, uint32_t* largest)
{
    uint8_t* best = NULL;
    uint32_t bestSize = 0xFFFFFFFFu;
    int r;
    for (r = 0; r < s_wregCount; r++)
    {
        const uint8_t* rEnd = s_wreg[r].base + s_wreg[r].bytes;
        uint8_t*       cur  = s_wreg[r].base;
        for (;;)
        {
            const WChunk* nx = NULL;
            uint32_t gap;
            int i;
            for (i = 0; i < MAX_WCHUNKS; i++)
            {
                const WChunk* c = &s_chunks[i];
                if (!c->inUse || c->arena == NULL || c->arena < cur || c->arena >= rEnd)
                    continue;
                if (nx == NULL || c->arena < nx->arena)
                    nx = c;
            }
            gap = (uint32_t)((nx ? (const uint8_t*)nx->arena : rEnd) - cur);
            if (largest && gap > *largest)
                *largest = gap;
            if (need && gap >= need && gap < bestSize)
            {
                best     = cur;
                bestSize = gap;
            }
            if (nx == NULL)
                break;
            cur = (uint8_t*)(((uintptr_t)(nx->arena + nx->arenaBytes) + 15u) & ~(uintptr_t)15u);
        }
    }
    if (gapOut)
        *gapOut = best ? bestSize : 0;
    return best;
}

static uint8_t* WPoolAlloc(uint32_t need, uint32_t* gap)
{
    return WPoolScan(need, gap, NULL);
}

static uint32_t WPoolLargestGap(void)
{
    uint32_t l = 0;
    WPoolScan(0, NULL, &l);
    return l;
}

/* Pool defragmentation. Four exterior cells fit the pool's TOTAL free space
 * long before they fit its gaps (three resident cells left 87K + 41K + 130K
 * against a 154K fourth). A plan packs every live pool chunk plus the
 * newcomer, biggest first, best-fit into empty regions; WPoolRepack then
 * moves the chunks there and rebases their pointers. */
typedef struct
{
    WChunk*  c;      /* NULL = the newcomer */
    uint32_t size;
    uint8_t* dst;
} WPlan;

static int WPoolRegionOf(const uint8_t* p)
{
    int r;
    for (r = 0; r < s_wregCount; r++)
        if (p >= s_wreg[r].base && p < s_wreg[r].base + s_wreg[r].bytes)
            return r;
    return -1;
}

static int WPoolPlan(uint32_t need, WPlan* plan, int* nPlan)
{
    uint32_t fill[3] = {0, 0, 0};
    int      n = 0, i, j, r;

    if (s_wregCount == 0)
        return 0;
    for (i = 0; i < MAX_WCHUNKS; i++)
    {
        WChunk* c = &s_chunks[i];
        if (!c->inUse || c->arena == NULL || WPoolRegionOf(c->arena) < 0)
            continue;
        plan[n].c    = c;
        plan[n].size = (c->arenaBytes + 15u) & ~15u;
        plan[n].dst  = NULL;
        n++;
    }
    plan[n].c    = NULL;
    plan[n].size = (need + 15u) & ~15u;
    plan[n].dst  = NULL;
    n++;
    for (i = 1; i < n; i++)
    {
        WPlan k = plan[i];
        for (j = i - 1; j >= 0 && plan[j].size < k.size; j--)
            plan[j + 1] = plan[j];
        plan[j + 1] = k;
    }
    for (i = 0; i < n; i++)
    {
        int      best = -1;
        uint32_t bestLeft = 0xFFFFFFFFu;
        for (r = 0; r < s_wregCount; r++)
        {
            uint32_t left = s_wreg[r].bytes - fill[r];
            if (left >= plan[i].size && left < bestLeft)
            {
                best     = r;
                bestLeft = left;
            }
        }
        if (best < 0)
            return 0;
        plan[i].dst = s_wreg[best].base + fill[best];
        fill[best] += plan[i].size;
    }
    *nPlan = n;
    return 1;
}

static void WChunkRelocate(WChunk* c, uint8_t* dst)
{
    intptr_t d = (intptr_t)dst - (intptr_t)c->arena;
    if (d == 0)
        return;
    memmove(dst, c->arena, c->arenaBytes);
#define WRELOC(p) do { if ((p) != NULL) (p) = (void*)((intptr_t)(p) + d); } while (0)
    WRELOC(c->bufs);
    WRELOC(c->verts);
    WRELOC(c->mats);
    WRELOC(c->rawRot);
    WRELOC(c->rawTrans);
    WRELOC(c->viewRow);
    WRELOC(c->groupPos);
    WRELOC(c->tileRefs);
    WRELOC(c->cmds);
#undef WRELOC
    c->arena = dst;
    /* The RSP reads vertices and matrices from RAM, not the CPU cache. */
    data_cache_hit_writeback(dst, c->arenaBytes);
}

/* Whether `need` can be placed, directly or after a repack. */
static int WPoolFits(uint32_t need)
{
    WPlan plan[MAX_WCHUNKS + 1];
    int   n;
    return need <= WPoolLargestGap() || WPoolPlan(need, plan, &n);
}

static int WPoolRepack(uint32_t need)
{
    WPlan plan[MAX_WCHUNKS + 1];
    int   n, i, j, moved, left, total = 0;

    if (!WPoolPlan(need, plan, &n))
        return 0;
    rspq_wait();   /* in-flight RSP lists still point at the old places */
    do
    {
        moved = left = 0;
        for (i = 0; i < n; i++)
        {
            WChunk* c = plan[i].c;
            int blocked = 0;
            if (c == NULL || c->arena == plan[i].dst)
                continue;
            /* Move only onto memory no other chunk occupies RIGHT NOW; a
             * chunk already at its destination never overlaps another's. */
            for (j = 0; j < n && !blocked; j++)
            {
                const WChunk* o = plan[j].c;
                if (j == i || o == NULL)
                    continue;
                if (plan[i].dst < o->arena + o->arenaBytes &&
                    o->arena < plan[i].dst + plan[i].size)
                    blocked = 1;
            }
            if (blocked)
            {
                left++;
                continue;
            }
            WChunkRelocate(c, plan[i].dst);
            moved++;
            total++;
        }
    } while (moved != 0 && left != 0);
    SH_DBG("[T3DW] pool repack for %uK: %d chunk(s) moved%s, largest gap now %uK",
           (unsigned)(need / 1024), total, left ? " -- STALLED on a cycle" : "",
           (unsigned)(WPoolLargestGap() / 1024));
    return left == 0;
}

static void WPendingDrop(int cellX, int cellZ)
{
    int i;
    for (i = 0; i < s_wpendingCount; i++)
        if (s_wpending[i].cellX == cellX && s_wpending[i].cellZ == cellZ)
        {
            s_wpending[i] = s_wpending[--s_wpendingCount];
            i--;
        }
}

static void WPendingAdd(const char* base, int cellX, int cellZ, uint32_t need)
{
    WPendingDrop(cellX, cellZ);
    if (s_wpendingCount >= WPENDING_MAX)
        return;
    snprintf(s_wpending[s_wpendingCount].base, sizeof s_wpending[0].base, "%s", base);
    s_wpending[s_wpendingCount].cellX = cellX;
    s_wpending[s_wpendingCount].cellZ = cellZ;
    s_wpending[s_wpendingCount].need  = need;
    s_wpendingCount++;
}

/* A cell the game still has loaded but the pool could not place: try again
 * now that an eviction freed space. Loads are synchronous file reads, the
 * same cost the game's own IPD-loaded hook already pays. */
static void WPendingRetry(void)
{
    WPending list[WPENDING_MAX];
    int n = s_wpendingCount, i;
    memcpy(list, s_wpending, sizeof list);
    for (i = 0; i < n; i++)
    {
        char ipd[24];
        /* A cell that cannot fit the biggest gap would only re-read its
         * whole file to fail again (seen: every evict, 81 ms chunk frames). */
        if (list[i].need != 0 && !WPoolFits(list[i].need))
            continue;
        snprintf(ipd, sizeof ipd, "%s.IPD", list[i].base);
        ShT3d_WorldChunkLoaded(ipd, list[i].cellX, list[i].cellZ);
    }
}

static WChunk* ChunkFind(int cellX, int cellZ)
{
    int i;
    for (i = 0; i < MAX_WCHUNKS; i++)
        if (s_chunks[i].inUse && s_chunks[i].cellX == cellX && s_chunks[i].cellZ == cellZ)
        {
            if (!ChunkArenaValid(&s_chunks[i]))
            {
                static int s_corrupt;
                if ((s_corrupt++ & 63) == 0)
                    SH_DBG("[T3DW] CORRUPT chunk slot=%d cell=%d,%d bufs=%p bufCount=%d insts=%d -- dropped",
                           i, s_chunks[i].cellX, s_chunks[i].cellZ,
                           (void*)s_chunks[i].bufs, s_chunks[i].bufCount, s_chunks[i].instCount);
                return NULL;
            }
            return &s_chunks[i];
        }
    return NULL;
}

static void ChunkFree(WChunk* c)
{
    if (!c->inUse)
        return;
    /* Everything lives in the slot's arena. World tiles are LRU now (no per-
     * chunk refcount) -- a freed chunk's tiles simply age out and get evicted
     * when a later tile needs the slot, or are cleared wholesale on area reset. */
    memset(c, 0, sizeof *c);
}

static int s_cnTileMiss;   /* groups whose geometry was skipped this frame */

/* Replay one pass's command stream LIVE. Pre-recorded rspq blocks were the
 * design, but recording needs heap exactly when the game has none (libdragon
 * grows block buffers with UNCHECKED mallocs -- a starved heap is a crash,
 * a guarded one deferred forever). Replay costs ~1-3ms of t3d calls for a
 * visible room against the ~100ms the RSP path replaces, needs zero heap,
 * and tile misses self-heal frame to frame instead of baking into a block. */
/* A group (one OP_VERTS load) is foreground only when its ENTIRE bounding
 * box is nearer the camera than the player (worst-case corner via interval
 * arithmetic), by at least this margin (world/8: 8 = a quarter world unit).
 * Centroid classification clipped the player IMMEDIATELY: the floor he
 * stands on straddles his depth, its centroid reads "in front", and the far
 * half painted over his feet -- with the straddling set changing every step
 * ("random bits of Harry disappear"). A patch wholly in front (a pillar, the
 * counter edge) still occludes; a straddler stays background and can never
 * cut into him. */
#define WORLD_FG_BIAS 8.0f

/* Program the combiner and upload a tile's texels+palette to TMEM. Returns 0
 * (caller drops the group's draws) when the tile isn't resident -- drawing
 * with stale TMEM painted other rooms' art onto this one's furniture. Called
 * LAZILY from the first drawn OP_TRIS after an OP_TILE, so a tile whose every
 * group lands in the other pass never uploads at all: the eager version
 * re-uploaded the full tile set in BOTH passes, and with the RDP pipe already
 * ~119ms busy of a ~128ms frame that pushed heavy frames past rspq's 200ms
 * RSP watchdog ("__rsp_crash" mid-OT walk). */
/* decal_mode=2 diagnostic: the decal pass binds flat green, so a screenshot
 * shows which surfaces mkworld tagged as decals. */
static int s_bindTintDecal;

static int BindWorldTile(uint16_t tref, uint16_t palArg)
{
    int slot;
    /* rdpq's AUTO-sync only tracks its own triangles; the RSP's t3d
     * triangles are invisible to it, so every mode/TMEM change here must be
     * fenced BY HAND exactly as t3dmodel.c does (sync_pipe before mode
     * changes, sync_load before uploads). Racing them against in-flight
     * world triangles corrupted draws and eventually wedged the RDP -- the
     * rspq watchdog then reported the stall as an RSP crash from whichever
     * caller next needed a buffer. */
    rdpq_sync_pipe();
    if (tref == 0)
    {
        /* untextured group: shade-only, or it would sample whatever tile the
         * previous group left in TMEM */
        rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
        return 1;
    }
    if (palArg >= s_shtPalCount)
    {
        /* A garbage palette index would enqueue a TLUT upload with a wild
         * pointer AND a wild word count -- scribbling the command stream is
         * exactly the class behind the jump-to-1 crash. Drop the group. */
        static int s_palOob;
        if (s_palOob++ == 0)
            SH_DBG("[T3DW] pal idx %u >= %u -- group dropped (stream fault?)",
                   (unsigned)palArg, (unsigned)s_shtPalCount);
        return 0;
    }
    slot = TileAcquire(tref - 1);   /* lazy: load-on-draw + LRU, marks lastUse */
    if (slot < 0)
    {
        s_cnTileMiss++;
        return 0;
    }
    {
        WTile* t = &s_tiles[slot];
        surface_t surf = surface_make_linear(t->pix,
            t->fmt == 0 ? FMT_CI4 : FMT_CI8, t->w, t->h);
        /* sync_load before overwriting TMEM + sync_tile before reusing the
         * tile descriptor (t3dmodel.c's exact upload fencing). */
        rdpq_sync_load();
        rdpq_sync_tile();
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
        rdpq_tex_upload_tlut(s_pals + s_palOffsets[palArg], 0,
                             s_palWords[palArg]);
        rdpq_tex_upload(TILE0, &surf, NULL);
    }
    if (s_bindTintDecal)
    {
        rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
        rdpq_set_prim_color(RGBA32(0x00, 0xFF, 0x00, 0xFF));
    }
    return 1;
}

/* Character tiles: eager-resident (slot == tile index), own palettes. Same
 * hand fencing as BindWorldTile -- t3d triangles are invisible to rdpq's
 * auto-sync. */
static int BindCharTile(uint16_t tref, uint16_t palArg)
{
    int slot;
    rdpq_sync_pipe();
    {
        /* Diagnostic: draw the character solid/untextured to read the raw
         * silhouette (geometry vs texture). Auto-cycled mode wins; config
         * chara_debug forces it. */
        extern int GpuNv2a_CharaDebug(void);
        if (s_charDiagMode == 1 || GpuNv2a_CharaDebug())
        {
            rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
            return 1;
        }
    }
    if (tref == 0)
    {
        rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
        return 1;
    }
    slot = (int)tref - 1;
    if (slot >= s_cTileCount || s_cTiles[slot].pix == NULL || palArg >= s_cPalCount)
    {
        s_cnTileMiss++;
        return 0;
    }
    {
        WTile* t = &s_cTiles[slot];
        surface_t surf = surface_make_linear(t->pix,
            t->fmt == 0 ? FMT_CI4 : FMT_CI8, t->w, t->h);
        rdpq_sync_load();
        rdpq_sync_tile();
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
        rdpq_tex_upload_tlut(s_cPals + s_cPalOffsets[palArg], 0, s_cPalWords[palArg]);
        rdpq_tex_upload(TILE0, &surf, NULL);
    }
    return 1;
}

/* wantFg: 0 = draw only background groups (farther than the player), 1 =
 * only foreground (nearer), 2 = every group (the Z-buffer path; groupPos then
 * only feeds the fog cull). Splitting the same stream twice around the PSX
 * character OT reproduces the OT's back-to-front order with no Z buffer.
 * groupBase = this stream's first index into groupPos (see WBuf); groupPos
 * NULL = no centroid data, everything is background. viewRow = per-instance
 * composed view row 2 (game view space); each group classifies through the
 * row of its own OP_MATRIX instance. */
static void RunPass(const uint8_t* p, int cmdWords,
                    const T3DVertPacked* verts, const T3DMat4FP* mats,
                    const int16_t* groupPos, const float* viewRow,
                    int groupBase, int wantFg,
                    int semi, int (*bind)(uint16_t, uint16_t), int preamble)
{
    /* depthSkip defaults to wantFg so any geometry before the first OP_VERTS
     * classification draws in the background pass only -- never in both,
     * which would double-draw it. */
    int i = 0, pushed = 0, needSync = 0, tileSkip = 0, depthSkip = (wantFg == 1);
    int gIdx = groupBase;
    /* Vertex-cache fill cursor. The world emits one OP_VERTS per window (loads
     * at 0); a CHARACTER window emits several OP_VERTS -- one per owner bone,
     * each under its own OP_MATRIX -- that must ACCUMULATE into the same cache
     * so seam verts land beside the part's own. Reset after each OP_TRIS, so the
     * world's single-load windows stay at 0 unchanged. */
    int vertFill = 0;
    const float* curRow = NULL;   /* view row 2 of the current instance */
    /* Lazy tile state: OP_TILE only records; BindWorldTile runs at the first
     * OP_TRIS that actually draws under it. A pending tile overwritten by the
     * next OP_TILE was for a fully-skipped run of groups. */
    int tilePending = 0;
    uint16_t pendTref = 0, pendPal = 0;

    /* Pass-wide state the OTHER pass may have changed. sync_pipe first: the
     * previous stream's t3d triangles are invisible to rdpq's auto-sync.
     * preamble=0 when the caller runs many streams of the SAME pass back to
     * back (the Z path's tile-sorted records): the blender is unchanged, and
     * a pipe drain per record would cost what the sort saves. */
    if (preamble)
    {
        rdpq_sync_pipe();
        if (semi)
            rdpq_mode_blender(RDPQ_BLENDER_ADDITIVE);
        else
            rdpq_mode_blender(0);
        /* The fog blend cycle would pull an additive surface TOWARD the fog
         * colour and then add it: distant glows turned into grey blobs. With
         * the fog cycle off, tiny3d's keep factor (shade alpha) scales the
         * additive term to nothing instead, like the PSX path's fade. */
        if (s_fogOn)
            rdpq_mode_fog(semi ? 0 : RDPQ_FOG_STANDARD);
    }

    while (i < cmdWords)
    {
        uint16_t w  = rd16(p + i * 2);
        uint16_t opc = w >> 12;
        uint16_t arg = w & 0xFFF;
        i++;

        if (opc == OP_TILE)
        {
            uint16_t tref = rd16(p + i * 2);
            i++;
#if SH_T3DW_FLAT
            (void)tref; tileSkip = 0;   /* flat debug: draw every group solid */
            continue;
#endif
            pendTref    = tref;
            pendPal     = arg;
            tilePending = 1;
            tileSkip    = 0;   /* unknown until bound; the bind decides */
        }
        else if (opc == OP_MATRIX)
        {
            if (needSync) { t3d_tri_sync(); needSync = 0; }
            if (pushed)
                t3d_matrix_pop(1);
            t3d_matrix_push(&mats[arg]);
            pushed = 1;
            curRow = viewRow ? viewRow + (int)arg * 4 : NULL;
        }
        else if (opc == OP_VERTS)
        {
            uint16_t first = rd16(p + i * 2);
            int g = gIdx++;
            i++;
            /* Classify this <=32-vert patch in the GAME'S view space, by its
             * WORST-CASE (farthest) depth: AABB centre through the instance's
             * composed view row plus the interval term |row|.halfExtent. Only
             * a patch ENTIRELY nearer than the player is foreground -- a
             * straddler (the floor under his feet) stays background and can
             * never paint over him. */
            if (groupPos != NULL && curRow != NULL)
            {
                const int16_t* B = groupPos + g * 6;
                float r0 = curRow[0], r1 = curRow[1], r2 = curRow[2];
                float vzC = r0 * B[0] + r1 * B[1] + r2 * B[2] + curRow[3];
                float vzE = (r0 < 0 ? -r0 : r0) * B[3]
                          + (r1 < 0 ? -r1 : r1) * B[4]
                          + (r2 < 0 ? -r2 : r2) * B[5];
                if (s_fogCullZ > 0.0f && vzC - vzE > s_fogCullZ)
                {
                    depthSkip = 1;
                    s_cnFogCulled++;
                }
                else if (wantFg == 2)
                    depthSkip = 0;
                else
                    depthSkip = ((((vzC + vzE) < s_playerViewZ - WORLD_FG_BIAS) ? 1 : 0) != wantFg);
            }
            else
                depthSkip = (wantFg == 1);
            if (depthSkip)
                continue;
            /* Bind the pending tile BEFORE the vert load: for every drawn
             * group the upload precedes vertex DMA in exactly the order the
             * proven eager path used, while tiles whose groups never draw
             * still never upload. */
            if (tilePending)
            {
                if (s_lastBoundOk && pendTref == s_lastBoundTref && pendPal == s_lastBoundPal)
                {
                    /* identical tile+pal already resident -- no upload, no sync */
                    tileSkip = 0;
                    s_cnTileDedup++;
                }
                else
                {
                    if (needSync) { t3d_tri_sync(); needSync = 0; }
                    tileSkip = !bind(pendTref, pendPal);
                    s_lastBoundOk   = !tileSkip;
                    s_lastBoundTref = pendTref;
                    s_lastBoundPal  = pendPal;
                    if (!tileSkip) s_cnTileUp++;
                }
                tilePending = 0;
            }
            if (tileSkip)
                continue;
            if (needSync) { t3d_tri_sync(); needSync = 0; }
            t3d_vert_load(verts + first / 2, vertFill, arg);
            vertFill += arg;
        }
        else if (opc == OP_TRIS)
        {
            int n = arg, k;
            if (!tileSkip && !depthSkip)
            {
                for (k = 0; k < n; k++)
                {
                    /* packed u8 triples across u16 words */
                    int base = i * 2 + k * 3;
                    t3d_tri_draw(p[base], p[base + 1], p[base + 2]);
                    s_cnRunPassTris++;
                }
                needSync = 1;
            }
            i += (n * 3 + 1) / 2;
            vertFill = 0;   /* window closed; next OP_VERTS starts at cache 0 */
        }
        else if (opc == OP_END)
        {
            break;
        }
        else
        {
            SH_DBG("[T3DW] bad opcode %x", opc);
            break;
        }
    }
    if (needSync)
        t3d_tri_sync();
    if (pushed)
        t3d_matrix_pop(1);
}

static int ShwLoadBody(WChunk* c, uint8_t* arena, int arenaBytes, FILE* f,
                       const char* base, int bufCount, int instCount, int refCount,
                       uint32_t instOff, uint32_t refsOff, int cellX, int cellZ,
                       int charMode);

void ShT3d_WorldChunkLoaded(const char* ipdName, int cellX, int cellZ)
{
    char base[16], prefix[8], name[20];
    FILE* f;
    uint8_t hdr[0x14];
    long size;
    int i, n, bufCount, instCount, refCount;
    uint32_t instOff, refsOff;
    WChunk* c;

    if (!ShT3d_Ready())
        return;

    if (ChunkFind(cellX, cellZ) != NULL)
        return;
    /* The game re-announces every loaded cell whenever it inits one; a cell
     * already known not to fit the pool stays pending without a file read. */
    for (n = 0; n < s_wpendingCount; n++)
        if (s_wpending[n].cellX == cellX && s_wpending[n].cellZ == cellZ &&
            s_wpending[n].need != 0 && !WPoolFits(s_wpending[n].need))
            return;

    /* "ERFF00.IPD" -> base ERFF00, prefix ER (strip 4 hex coord chars). */
    for (n = 0; ipdName[n] && ipdName[n] != '.' && n < 15; n++)
        base[n] = ipdName[n];
    base[n] = 0;
    if (n <= 4)
        return;
    memcpy(prefix, base, n - 4);
    prefix[n - 4] = 0;

    if (!ShtOpen(prefix))
    {
        if (!(s_logOnce & 1))
        {
            s_logOnce |= 1;
            SH_DBG("[T3DW] no %s.SHT -- native world disabled for this area", prefix);
        }
        return;
    }

    snprintf(name, sizeof name, "%s.SHW", base);
    f = WOpen(name);
    if (f == NULL)
    {
        if (WFailLog())
            SH_DBG("[T3DW] %s: not found (sd:+rom:)", name);
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    (void)size;
    fseek(f, 0, SEEK_SET);
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || rd32(hdr) != SHW_MAGIC)
    {
        SH_DBG("[T3DW] %s: bad header/magic", name);
        fclose(f);
        return;
    }
    bufCount  = rd16(hdr + 6) & SHW_COUNT_MASK;
    instCount = rd16(hdr + 8);
    refCount  = rd16(hdr + 10);
    instOff   = rd32(hdr + 0xC);
    refsOff   = rd32(hdr + 0x10);

    /* The caller's name came from a ring-buffer queue entry that can have
     * been reused; the SHW carries its own cell coords, so a stale name
     * self-rejects here instead of drawing the wrong cell. */
    if ((int8_t)hdr[4] != cellX || (int8_t)hdr[5] != cellZ)
    {
        SH_DBG("[T3DW] %s cell mismatch (%d,%d vs %d,%d) -- skipped",
               name, (int)(int8_t)hdr[4], (int)(int8_t)hdr[5], cellX, cellZ);
        fclose(f);
        return;
    }

    /* find a free slot */
    c = NULL;
    for (i = 0; i < MAX_WCHUNKS; i++)
        if (!s_chunks[i].inUse)
        {
            c = &s_chunks[i];
            break;
        }
    if (c == NULL)
    {
        WPendingAdd(base, cellX, cellZ, 0);
        if (WFailLog())
            SH_DBG("[T3DW] %s: chunk slots full -- retried on evict", base);
        fclose(f);
        return;
    }
    memset(c, 0, sizeof *c);
    c->cellX = cellX;
    c->cellZ = cellZ;
    c->glbBaked    = (rd16(hdr + 6) & SHW_FLAG_GLB) != 0;
    c->sharedVerts = (rd16(hdr + 6) & SHW_FLAG_SHAREDV) != 0;

    WPoolInit();
    if (!ShwLoadBody(c, NULL, 0, f, base,
                     bufCount, instCount, refCount, instOff, refsOff, cellX, cellZ, 0))
    {
        fclose(f);
        c->inUse = 1;          /* let ChunkFree see a live chunk to unwind */
        c->bufCount = 0;
        ChunkFree(c);
        return;
    }
    ChunkReadObjTrailer(c, f, bufCount, instCount);
    /* First composition immediately (this frame's WorldViewSet has
     * already run); every later frame recomposes in WorldViewSet. */
    ComposeChunkViews(c);
    fclose(f);
    c->inUse = 1;
    WPendingDrop(cellX, cellZ);
    SH_DBG("[T3DW] chunk %s resident: bufs=%d insts=%d glb=%d sv=%d groups=%d tris=%d tiles=%d/%d miss=%d tileRam=%dK pool=%uK@r%d largestGap=%uK",
           base, bufCount, instCount, c->glbBaked, c->sharedVerts, c->groupCount, c->triCount,
           c->tileRefCount, refCount,
           s_cnTileMiss, s_tileRam / 1024, (unsigned)(c->arenaBytes / 1024),
           (c->arena >= s_wreg[0].base && c->arena < s_wreg[0].base + s_wreg[0].bytes) ? 0 : 1,
           (unsigned)(WPoolLargestGap() / 1024));
}

/* ShwLoadBody's staging: the buffer table (<= 64 x 20 B) and the instance/
 * vertex read window (<= 256 x 32 B, verts go through it 64 x 16 B at a time). */
static uint8_t s_loadTable[64 * 20];
static uint8_t s_loadTmp[256 * 32];

/* The SHW parser proper, shared by world chunks (charMode 0: cell-folded
 * instance translations, tiles ref-counted into the area pool) and native
 * characters (charMode 1: identity rest-pose instances the bone loop
 * overwrites every frame, tiles eager-resident in the character store). On
 * success c->bufCount is set and the tables freed; the caller closes the
 * file, composes/marks resident and logs. On failure the tables are freed
 * and 0 returned; the caller unwinds the chunk. */
static int ShwLoadBody(WChunk* c, uint8_t* arena, int arenaBytes, FILE* f,
                       const char* base, int bufCount, int instCount, int refCount,
                       uint32_t instOff, uint32_t refsOff, int cellX, int cellZ,
                       int charMode)
{
    int i;
    /* STREAM-PARSE: the file is ~87% vertex payload; never hold it whole.
     * Small tables first, then the big vertex block into the cleanest free
     * space, then verts through a staging window and commands through a
     * small transient. Peak heap = verts + ~4KB instead of file + verts
     * (the whole-file version fragmented until 48KB had no home at 136KB
     * free). Everything below cleans up through fail:. */
    {
        uint8_t* table = NULL;
        uint8_t* tmp   = NULL;
        int      total = 0;
        int      maxWords = 0;

        /* Fixed scratch, not malloc: at an exterior the heap runs to single
         * digits and a failed 4K staging malloc dropped whole cells to the
         * PSX path. The loader is never re-entered mid-load. */
        if (bufCount > 64 || instCount > 256)
            goto fail;
        table = s_loadTable;
        if (table == NULL)
            goto fail;
        fseek(f, 0x14, SEEK_SET);
        if (fread(table, 1, bufCount * 20, f) != (size_t)(bufCount * 20))
            goto fail;

        for (i = 0; i < bufCount; i++)
        {
            int ow = rd16(table + i * 20 + 2);
            int sw = rd16(table + i * 20 + 4);
            total += (rd16(table + i * 20) + 1) & ~1;
            if (ow > maxWords) maxWords = ow;
            if (sw > maxWords) maxWords = sw;
        }

        /* Carve verts + mats + refs + buf table + command streams from this
         * slot's fixed arena. cmdBytes counts every pass's words. */
        {
            int nInst     = instCount ? instCount : 1;
            int vertBytes = (int)sizeof(T3DVertPacked) * (total / 2 + 1);
            int matBytes  = (int)sizeof(T3DMat4FP) * nInst * 2; /* double-buffered */
            int rawRBytes = (int)sizeof(int16_t) * 9 * nInst;
            int rawTBytes = (int)sizeof(int32_t) * 3 * nInst;
            int rowBytes  = (int)sizeof(float) * 4 * nInst;
            int refBytes  = refCount ? refCount * 2 : 2;
            int bufBytes  = (int)sizeof(WBuf) * bufCount;
            int cmdBytes  = 0;
            int need;
            uint8_t* a = arena;
            for (i = 0; i < bufCount; i++)
            {
                int ow = rd16(table + i * 20 + 2);
                int sw = rd16(table + i * 20 + 4);
                int dw = rd16(table + i * 20 + 6);
                if (ow > 1) cmdBytes += (ow * 2 + 3) & ~3;
                if (sw > 1) cmdBytes += (sw * 2 + 3) & ~3;
                if (dw > 1) cmdBytes += (dw * 2 + 3) & ~3;
            }
            need = ((vertBytes + 15) & ~15) + ((matBytes + 15) & ~15)
                 + ((rawRBytes + 15) & ~15) + ((rawTBytes + 15) & ~15)
                 + ((rowBytes + 15) & ~15)
                 + ((refBytes + 15) & ~15) + ((bufBytes + 15) & ~15)
                 + ((cmdBytes + 15) & ~15);
            if (arena == NULL)
            {
                /* Pool chunk: ask for ~6% over the hard need so the soft
                 * group AABBs (~3% measured) usually land too. */
                uint32_t gap = 0;
                arena = WPoolAlloc((uint32_t)need + (uint32_t)need / 16, &gap);
                if (arena == NULL)
                    arena = WPoolAlloc((uint32_t)need, &gap);
                /* Fragmented but big enough in total: defragment, soft size
                 * first so the group AABBs still land. */
                if (arena == NULL && WPoolRepack((uint32_t)need + (uint32_t)need / 16))
                    arena = WPoolAlloc((uint32_t)need + (uint32_t)need / 16, &gap);
                if (arena == NULL && WPoolRepack((uint32_t)need))
                    arena = WPoolAlloc((uint32_t)need, &gap);
                if (arena == NULL)
                {
                    WPendingAdd(base, cellX, cellZ, (uint32_t)need);
                    if (WFailLog())
                        SH_DBG("[T3DW] %s: %dKB, no pool gap (largest %uKB) -- PSX fallback, retried on evict",
                               base, need / 1024, (unsigned)(WPoolLargestGap() / 1024));
                    goto fail;
                }
                arenaBytes = (int)gap;
                a = arena;
            }
            c->arena      = arena;
            c->arenaBytes = (uint32_t)arenaBytes;
            if (need > arenaBytes)
            {
                if (WFailLog())
                    SH_DBG("[T3DW] %s: %dKB over the %dKB arena -- PSX fallback",
                           base, need / 1024, arenaBytes / 1024);
                goto fail;
            }
            c->verts = (T3DVertPacked*)a;
            a += (vertBytes + 15) & ~15;
            c->mats = (T3DMat4FP*)a;
            a += (matBytes + 15) & ~15;
            c->rawRot = (int16_t*)a;
            a += (rawRBytes + 15) & ~15;
            c->rawTrans = (int32_t*)a;
            a += (rawTBytes + 15) & ~15;
            c->viewRow = (float*)a;
            a += (rowBytes + 15) & ~15;
            c->tileRefs = (uint16_t*)a;
            a += (refBytes + 15) & ~15;
            c->bufs = (WBuf*)a;
            a += (bufBytes + 15) & ~15;
            c->cmds = a;
            c->instCount = instCount;
            memset(c->bufs, 0, bufBytes);
        }

        /* Instances: keep the RAW Q12 rotation + Q8 world translation (cell
         * corner folded in). The drawable matrices are composed PER FRAME
         * from these through the game's own view path (ComposeChunkViews),
         * which is what makes the native world land exactly where the
         * per-prim path would land it. */
        tmp = s_loadTmp;
        fseek(f, instOff, SEEK_SET);
        if (fread(tmp, 1, instCount * 32, f) != (size_t)(instCount * 32))
            goto fail;
        for (i = 0; i < instCount; i++)
        {
            const uint8_t* ip = tmp + i * 32;
            int k;
            for (k = 0; k < 9; k++)
                c->rawRot[i * 9 + k] = (int16_t)rd16(ip + k * 2);
            c->rawTrans[i * 3 + 0] = (int32_t)rd32(ip + 20) + (charMode ? 0 : cellX * 10240);
            c->rawTrans[i * 3 + 1] = (int32_t)rd32(ip + 24);
            c->rawTrans[i * 3 + 2] = (int32_t)rd32(ip + 28) + (charMode ? 0 : cellZ * 10240);
        }

        /* Tile refs (arena-resident, carved above). RECORDED only -- world
         * tiles are loaded LAZILY at draw time (BindWorldTile -> TileAcquire)
         * and evicted LRU, so we no longer pre-load a chunk's whole tile set
         * (that eager residency is what overran the pool with two big rooms).
         * The list stays for the resident-count log; char tiles are eager in
         * their own store. */
        c->tileRefCount = 0;
        if (refCount * 2 > (int)sizeof s_loadTmp)
            goto fail;
        fseek(f, refsOff, SEEK_SET);
        if (fread(tmp, 1, refCount * 2, f) != (size_t)(refCount * 2))
            goto fail;
        for (i = 0; i < refCount; i++)
        {
            uint16_t idx = rd16(tmp + i * 2);
            if (!charMode && c->tileRefCount < refCount)
                c->tileRefs[c->tileRefCount++] = idx;
        }

        /* Verts: stream 64 file-verts (1KB) at a time through tmp. */
        {
            T3DVertPacked* vp = c->verts;
            int vi = 0;
            for (i = 0; i < bufCount; i++)
            {
                const uint8_t* bt = table + i * 20;
                int vcount = rd16(bt);
                uint32_t voff = rd32(bt + 8);
                int k = 0;
                vi = (vi + 1) & ~1;   /* each buffer starts on a pair */
                fseek(f, voff, SEEK_SET);
                while (k < vcount)
                {
                    int batch = vcount - k > 64 ? 64 : vcount - k;
                    int b;
                    if (fread(tmp, 1, batch * 16, f) != (size_t)(batch * 16))
                        goto fail;
                    for (b = 0; b < batch; b++, k++, vi++)
                    {
                        const uint8_t* v = tmp + b * 16;
                        int16_t* pos = (vi & 1) ? vp[vi / 2].posB : vp[vi / 2].posA;
                        int16_t* st  = (vi & 1) ? vp[vi / 2].stB : vp[vi / 2].stA;
                        uint32_t rgba;
                        int cr = v[6] * 2, cg = v[7] * 2, cb = v[8] * 2;
                        if (cr > 255) cr = 255;
                        if (cg > 255) cg = 255;
                        if (cb > 255) cb = 255;
                        /* RAW Q8 for world and character alike; the 1/8 to
                         * t3d units lives in the instance matrix (rotation
                         * /4096/8, translation /8), where the RSP applies it
                         * in fixed point. Dividing the verts here truncated
                         * every coordinate to 8 Q8 (a 16 Q8 dead band at 0),
                         * which snapped posters 12..63 Q8 in front of a wall
                         * onto or through the wall's own snapped plane: the
                         * poster z-fighting that no draw order could fix. */
                        pos[0] = (int16_t)rd16(v + 0);
                        pos[1] = (int16_t)rd16(v + 2);
                        pos[2] = (int16_t)rd16(v + 4);
                        rgba = ((uint32_t)cr << 24) | ((uint32_t)cg << 16) |
                               ((uint32_t)cb << 8) | (uint32_t)v[9];
                        if (vi & 1) { vp[vi / 2].rgbaB = rgba; vp[vi / 2].normB = 0; }
                        else        { vp[vi / 2].rgbaA = rgba; vp[vi / 2].normA = 0; }
                        st[0] = (int16_t)rd16(v + 10);
                        st[1] = (int16_t)rd16(v + 12);
                    }
                }
            }
        }

        data_cache_hit_writeback(c->verts,
            sizeof(T3DVertPacked) * (total / 2 + 1));

        /* Command streams: copy every pass into the arena; drawn by LIVE
         * replay (RunPass), so no rspq blocks and no heap at all. */
        {
            uint32_t cmdCur = 0;
            int vbase = 0;
            (void)maxWords;
            for (i = 0; i < bufCount; i++)
            {
                const uint8_t* bt = table + i * 20;
                int vcount    = rd16(bt);
                int opaWords  = rd16(bt + 2);
                int semiWords = rd16(bt + 4);
                int decWords  = rd16(bt + 6);
                uint32_t opaOff  = rd32(bt + 0x0C);
                uint32_t semiOff = rd32(bt + 0x10);
                /* Shared-vertex cells address one cell-wide array (all of it
                 * in buffer 0's block), so every stream's base is 0. */
                c->bufs[i].vbase = c->sharedVerts ? 0 : (uint16_t)vbase;
                if (opaWords > 1)
                {
                    fseek(f, opaOff, SEEK_SET);
                    if (fread(c->cmds + cmdCur, 1, opaWords * 2, f) != (size_t)(opaWords * 2))
                        goto fail;
                    c->bufs[i].opaOff   = cmdCur;
                    c->bufs[i].opaWords = (uint16_t)opaWords;
                    cmdCur += (opaWords * 2 + 3) & ~3;
                }
                if (decWords > 1)
                {
                    /* The decal stream sits right behind the opaque words. */
                    fseek(f, opaOff + (uint32_t)opaWords * 2, SEEK_SET);
                    if (fread(c->cmds + cmdCur, 1, decWords * 2, f) != (size_t)(decWords * 2))
                        goto fail;
                    c->bufs[i].decalOff   = cmdCur;
                    c->bufs[i].decalWords = (uint16_t)decWords;
                    cmdCur += (decWords * 2 + 3) & ~3;
                }
                if (semiWords > 1)
                {
                    fseek(f, semiOff, SEEK_SET);
                    if (fread(c->cmds + cmdCur, 1, semiWords * 2, f) != (size_t)(semiWords * 2))
                        goto fail;
                    c->bufs[i].semiOff   = cmdCur;
                    c->bufs[i].semiWords = (uint16_t)semiWords;
                    cmdCur += (semiWords * 2 + 3) & ~3;
                }
                vbase += (vcount + 1) & ~1;
            }

            /* Per-GROUP centroids for the painter's split. An OP_VERTS group
             * is a <=32-vert patch of wall/floor, so classifying groups --
             * instead of whole instances: this cell has FIVE instances, each
             * a room-sized blob whose single origin flipped the entire shell
             * over the characters -- approaches the PSX ordering table's
             * per-primitive granularity. Each centroid is the group's local
             * vert average pushed through its instance matrix into world/8,
             * appended after the command streams in the arena. If they don't
             * fit, groupPos stays NULL and every group draws in the
             * background pass -- the pre-split behaviour, never worse. */
            {
                uint32_t gcount = 0;
                int      bi, pass;

                for (bi = 0; bi < bufCount; bi++)
                    for (pass = 0; pass < 2; pass++)
                    {
                        int words = pass ? c->bufs[bi].semiWords : c->bufs[bi].opaWords;
                        const uint8_t* p = c->cmds + (pass ? c->bufs[bi].semiOff : c->bufs[bi].opaOff);
                        int k = 0;
                        if (pass) c->bufs[bi].semiGroupBase = (uint16_t)gcount;
                        else      c->bufs[bi].opaGroupBase  = (uint16_t)gcount;
                        if (words <= 1)
                            continue;
                        while (k < words)
                        {
                            uint16_t w   = rd16(p + k * 2);
                            uint16_t opc = w >> 12;
                            uint16_t arg = w & 0xFFF;
                            k++;
                            if (opc == OP_TILE)       k++;
                            else if (opc == OP_VERTS) { k++; gcount++; }
                            else if (opc == OP_TRIS)  { k += (arg * 3 + 1) / 2; c->triCount += arg; }
                            else if (opc == OP_END)   break;
                            /* OP_MATRIX: 1 word, nothing to skip */
                        }
                    }

                {
                    uint32_t gbytes = gcount * 6 * (uint32_t)sizeof(int16_t);
                    uint8_t* gp     = c->cmds + ((cmdCur + 15u) & ~15u);
                    if (gcount == 0 || gp + gbytes > arena + arenaBytes)
                    {
                        c->arenaBytes = (uint32_t)((c->cmds + ((cmdCur + 15u) & ~15u)) - arena);
                        c->groupPos = NULL;
                        if (gcount != 0)
                            SH_DBG("[T3DW] %s: %u group AABBs over the arena -- background-only",
                                   base, (unsigned)gcount);
                    }
                    else
                    {
                        int16_t* gpos = (int16_t*)gp;
                        uint32_t g    = 0;
                        c->arenaBytes = (uint32_t)(((gp + gbytes) - arena + 15u) & ~15u);
                        c->groupPos   = gpos;
                        c->groupCount = (int)gcount;
                        for (bi = 0; bi < bufCount; bi++)
                            for (pass = 0; pass < 2; pass++)
                            {
                                int words = pass ? c->bufs[bi].semiWords : c->bufs[bi].opaWords;
                                const uint8_t* p = c->cmds + (pass ? c->bufs[bi].semiOff : c->bufs[bi].opaOff);
                                int bvbase  = c->bufs[bi].vbase;
                                int k = 0;
                                if (words <= 1)
                                    continue;
                                while (k < words)
                                {
                                    uint16_t w   = rd16(p + k * 2);
                                    uint16_t opc = w >> 12;
                                    uint16_t arg = w & 0xFFF;
                                    k++;
                                    if (opc == OP_TILE)
                                        k++;
                                    else if (opc == OP_VERTS)
                                    {
                                        /* LOCAL AABB (the /8 vert space):
                                         * center + half extents. RunPass
                                         * pushes them through the frame's
                                         * composed view row for the
                                         * worst-case (farthest) depth. */
                                        int first = rd16(p + k * 2);
                                        int n     = arg;
                                        int mnx = 32767, mny = 32767, mnz = 32767;
                                        int mxx = -32768, mxy = -32768, mxz = -32768;
                                        int v;
                                        k++;
                                        for (v = 0; v < n; v++)
                                        {
                                            int vi = bvbase + first + v;
                                            const int16_t* q = (vi & 1) ? c->verts[vi / 2].posB
                                                                        : c->verts[vi / 2].posA;
                                            if (q[0] < mnx) mnx = q[0];
                                            if (q[0] > mxx) mxx = q[0];
                                            if (q[1] < mny) mny = q[1];
                                            if (q[1] > mxy) mxy = q[1];
                                            if (q[2] < mnz) mnz = q[2];
                                            if (q[2] > mxz) mxz = q[2];
                                        }
                                        if (n <= 0)
                                        {
                                            mnx = mny = mnz = 0;
                                            mxx = mxy = mxz = 0;
                                        }
                                        /* World bounds go to the /8 space
                                         * viewRow works in (verts are raw
                                         * Q8); the character's stay raw. */
                                        {
                                            int sh = charMode ? 0 : 3;
                                            int rnd = (1 << sh) - 1;
                                            gpos[g * 6 + 0] = (int16_t)(((mnx + mxx) / 2) >> sh);
                                            gpos[g * 6 + 1] = (int16_t)(((mny + mxy) / 2) >> sh);
                                            gpos[g * 6 + 2] = (int16_t)(((mnz + mxz) / 2) >> sh);
                                            gpos[g * 6 + 3] = (int16_t)((((mxx - mnx) / 2 + rnd) >> sh) + 1);
                                            gpos[g * 6 + 4] = (int16_t)((((mxy - mny) / 2 + rnd) >> sh) + 1);
                                            gpos[g * 6 + 5] = (int16_t)((((mxz - mnz) / 2 + rnd) >> sh) + 1);
                                        }
                                        g++;
                                    }
                                    else if (opc == OP_TRIS)
                                        k += (arg * 3 + 1) / 2;
                                    else if (opc == OP_END)
                                        break;
                                    /* OP_MATRIX: 1 word */
                                }
                            }
                    }
                }
            }
        }

        c->bufCount = bufCount;
        return 1;

fail:
        if (WFailLog())
            SH_DBG("[T3DW] %s: load failed (see prior line or alloc)", base);
        return 0;
    }
}

void ShT3d_WorldChunkEvict(int cellX, int cellZ)
{
    WChunk* c = ChunkFind(cellX, cellZ);
    if (c != NULL)
    {
        /* Blocks may still be referenced by the RSP for the current frame. */
        rspq_wait();
        ChunkFree(c);
    }
    WPendingDrop(cellX, cellZ);
    WPendingRetry();
}

void ShT3d_WorldReset(void)
{
    int i;
    rspq_wait();
    for (i = 0; i < MAX_WCHUNKS; i++)
        ChunkFree(&s_chunks[i]);
    s_wpendingCount = 0;
    ChunkFree(&s_itemChunk);     /* the area's item pseudo-cell goes with its SHT */
    for (i = 0; i < (int)(sizeof s_tiles / sizeof s_tiles[0]); i++)
        if (s_tiles[i].sthIdx >= 0)
        {
            s_tiles[i].pix = NULL;
            s_tiles[i].sthIdx = -1;
            s_tiles[i].refs = 0;
        }
    s_tileRam = 0;
    /* Every pool chunk is gone: re-read the regions at the next load, after
     * the incoming map module has taken its (size-dependent) share of the
     * PSX window. */
    s_wregCount = 0;
    if (s_sht)
    {
        fclose(s_sht);
        s_sht = NULL;
    }
    free(s_palOffsets); s_palOffsets = NULL;
    free(s_palWords);   s_palWords = NULL;
    if (s_pals) { free_uncached(s_pals); s_pals = NULL; }
    s_shtPrefix[0] = 0;
}

/* Load the area's item pseudo-cell (<PREFIX>ITEM.SHW, see s_itemChunk) into
 * its own arena. Called from ShtOpen once the area SHT is open (its tiles live
 * there); freed with the area in ShT3d_WorldReset. Absence is normal for an
 * area baked without --items: global-pool pickups then stay on the PSX path. */
static void ItemChunkLoad(const char* prefix)
{
    uint8_t  hdr[0x14];
    char     name[24];
    FILE*    f;
    int      bufCount, instCount, refCount;
    uint32_t instOff, refsOff;

    ChunkFree(&s_itemChunk);
    snprintf(name, sizeof name, "%sITEM.SHW", prefix);
    f = WOpen(name);
    if (f == NULL)
        return;
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || rd32(hdr) != SHW_MAGIC)
    {
        SH_DBG("[T3DWO] %s: bad header", name);
        fclose(f);
        return;
    }
    bufCount  = rd16(hdr + 6) & SHW_COUNT_MASK;
    instCount = rd16(hdr + 8);
    refCount  = rd16(hdr + 10);
    instOff   = rd32(hdr + 0xC);
    refsOff   = rd32(hdr + 0x10);
    memset(&s_itemChunk, 0, sizeof s_itemChunk);
    s_itemChunk.cellX = s_itemChunk.cellZ = WOBJ_ITEM_CELL;
    if (!ShwLoadBody(&s_itemChunk, s_itemArena, (int)sizeof s_itemArena, f, name,
                     bufCount, instCount, refCount, instOff, refsOff, 0, 0, 0))
    {
        fclose(f);
        memset(&s_itemChunk, 0, sizeof s_itemChunk);
        return;
    }
    ChunkReadObjTrailer(&s_itemChunk, f, bufCount, instCount);
    fclose(f);
    s_itemChunk.inUse = 1;
    SH_DBG("[T3DWO] %s resident: %d item models (objects=%d)", name, bufCount, s_itemChunk.objCount);
}

/* ------------------------------------------------------------- drawing */

static void WorldFrameStart(void)
{
    /* R = view rotation (world->view), Q12 row-major in s_wm; t = view
     * translation, Q8 in s_wt. The geometry lives in t3d units = world/8
     * (Q8 world >> 3), so the camera is derived in the same units. */
    float R[3][3];
    float eye[3], fwd[3], up[3];
    int i;

    /* Mirror the spike's ORDER exactly (the spike renders in gameplay, this
     * path did not): frame_start -> zbuf off -> viewport (proj/look/attach)
     * FIRST, then combiner/lights/drawflags, then draw. Setting lights or the
     * combiner before the viewport attach left them stale. */
    t3d_frame_start();
    ShT3d_MemCheck("framestart(render)");   /* wild-writer tripwire, see ShT3d_MemCheck */
    s_lastBoundOk = 0;   /* TMEM was clobbered since the last flush pass */
    {
        /* Stage 1: with a real Z-buffer attached (gpu_rdp.c), the world writes
         * true depth and self-occludes by Z instead of painter's tile order.
         * Without it, the old no-Z path (t3d_frame_start turns Z ON, and with
         * no buffer that scribbles RDRAM -- so force it OFF). */
        extern int GpuNv2a_ZBufMode(void);
        int zmode = GpuNv2a_ZBufMode();
        s_zActive = (zmode == 1);   /* world tile-sorted through Z */
        s_zChara  = (zmode >= 1);   /* character parts depth-tested against each other */
        rdpq_mode_zbuf(s_zActive, s_zActive);
    }

    for (i = 0; i < 9; i++)
        R[i / 3][i % 3] = (float)s_wm[i] / 4096.0f;
    /* Eye = camera world position (Q8) / 8 = world/8 t3d units (matches the
     * instance translation tx/8 and the >>3 vertices). */
    for (i = 0; i < 3; i++)
        eye[i] = (float)s_camPos[i] / 8.0f;
    /* forward = R^T*+Z (PSX looks down +Z); up = -R^T*+Y (PSX +Y is down).
     * Probe-only now: the DRAW no longer reconstructs the camera -- instance
     * matrices arrive per frame already IN VIEW SPACE (composed through the
     * game's own Vw_CoordToWorldAndViewMatrices), so t3d's camera is just the
     * fixed PSX->GL axis flip below. */
    for (i = 0; i < 3; i++)
    {
        fwd[i] =  R[2][i];
        up[i]  = -R[1][i];
    }
    (void)up;

    {
        static const uint8_t ambWhite[4] = {255, 255, 255, 255};
        static const uint8_t dirWhite[4] = {255, 255, 255, 255};
        float h = (float)s_geomH;
        /* The whole camera is the fixed PSX->GL axis flip: instance matrices
         * arrive already in the game's view space (y down, z forward); GL
         * wants y up, -z forward. This is the exact construction the working
         * reconstructed camera used, evaluated at R = identity, eye = 0. */
        T3DVec3 e   = {{ 0.0f, 0.0f, 0.0f }};
        T3DVec3 tg  = {{ 0.0f, 0.0f, 256.0f }};
        T3DVec3 u   = {{ 0.0f, -1.0f, 0.0f }};
        T3DVec3 ld  = {{ 0.0f, 0.0f, 1.0f }};

        if (!s_wvpInited) { s_wvp = t3d_viewport_create(); s_wvpInited = 1; }
        /* Land RSP pixels exactly where PutVert lands the PSX prims:
         *   px = ((gofx + h*vx/vz) + ofsX)*sclX + contentX
         *   py = ((gofy + h*vy/vz) + ofsY)*sclY
         * folded into the projection as scale (m[0][0]/m[1][1]) and an NDC
         * translate (m[2][0/1]) on the 320x240 target. The transform comes
         * LIVE from the prim path (GpuXbox_GetViewTransform) so disp-env
         * changes track automatically. In-game that is ofs=(160,112),
         * scl=(1, 240/224), content=0 => h/160 and h/112: the previous h/120
         * vertical drew the native world 7% squashed against the PSX-path
         * characters (Harry poking into doorways, items floating off the
         * native counter -- collision was never wrong, the picture was).
         * gofx/gofy are the game's geometry offset DELTA (0,0 in-game). */
        {
            /* nearP sets the Z-buffer's resolution: 16-bit hyperbolic depth
             * resolves dz ~= z^2 / (65536 * nearP). The reception's wall
             * decals (posters, framed maps, signs) sit 12..63 Q8 = 1.5..8 t3d
             * units in front of their wall (measured over ERFE00/ERFF00;
             * the exact-coplanar hits are a wall's own tessellation, not
             * decals). At the ~1450-unit camera distance nearP=4 resolved
             * only ~8 t3d units, so every decal under 64 Q8 Z-fought its
             * wall (the flicker). 32 resolves ~1 t3d unit = 8 Q8, separating
             * the whole band, and only clips geometry closer than ~6.6 cm
             * to the camera, which SH1's fixed cameras never are. Depth rows
             * only -- screen X/Y come from h/ofx and do not move.
             * 32 -> 64 (2026-09-16): the lobby still flickered; 57 of its decals
             * sit at ~13 Q8, right at 32's ~8 Q8 resolution -- marginal, so Z
             * interpolation rounding still flipped them. 64 resolves ~4 Q8 (the
             * whole >=10 Q8 band cleanly) and clips only inside ~13 cm. */
            /* BACK TO 4 (2026-09-16). 32 and 64 were a scale error: Harry is
             * ~54 t3d units tall (1.7 m), so 64 units is ~2 m -- the floor at
             * the screen edges and Harry's near limbs were being NEAR-CLIPPED
             * ("culling on the edges", "see through him everywhere"). At the
             * real scale near=4 already resolves ~1 Q8 at room distances, so
             * the 12..63 Q8 decal offsets were never a precision problem: the
             * poster flicker is the RDP's dz compare tolerance, fixed with a
             * DECAL-mode pass (OP_DECAL stream), not the near plane. */
            float nearP = WORLD_PROJ_NEAR, farP = WORLD_PROJ_FAR;
            float oX, oY, sX, sY;
            int   cX;
            T3DMat4 proj;
            extern void GpuXbox_GetViewTransform(float* ofsX, float* ofsY,
                                                 float* sclX, float* sclY,
                                                 int* contentX);
            GpuXbox_GetViewTransform(&oX, &oY, &sX, &sY, &cX);
            if (h < 1.0f)
                h = 1.0f;
            memset(&proj, 0, sizeof proj);
            proj.m[0][0] = h * sX / 160.0f;
            proj.m[1][1] = h * sY / 120.0f;
            proj.m[2][2] = farP / (nearP - farP);
            proj.m[2][3] = -1.0f;
            proj.m[3][2] = -2.0f * farP * nearP / (farP - nearP);
            proj.m[2][0] = -((((float)s_geomOfx + oX) * sX + (float)cX) - 160.0f) / 160.0f;
            proj.m[2][1] =  ((((float)s_geomOfy + oY) * sY) - 120.0f) / 120.0f;
            t3d_viewport_set_w_normalize(&s_wvp, nearP, farP);
            t3d_viewport_set_projection_matrix(&s_wvp, &proj);
            {
                extern int PcConfig_N64DecalBias(void);
                s_decalBias = PcConfig_N64DecalBias();
                if (s_decalBias > 0)
                {
                    if (!s_wvpDecalInited)
                    {
                        s_wvpDecal = t3d_viewport_create();
                        s_wvpDecalInited = 1;
                    }
                    /* z_ndc = (m22*z + m32) / -z, so +eps on m22 is -eps on
                     * z_ndc; the 16-bit depth spans the NDC range of 2. */
                    proj.m[2][2] += (float)s_decalBias * (2.0f / 32768.0f);
                    t3d_viewport_set_w_normalize(&s_wvpDecal, nearP, farP);
                    t3d_viewport_set_projection_matrix(&s_wvpDecal, &proj);
                }
            }
        }
        t3d_viewport_look_at(&s_wvp, &e, &tg, &u);
        if (s_decalBias > 0)
            t3d_viewport_look_at(&s_wvpDecal, &e, &tg, &u);
        t3d_viewport_attach(&s_wvp);
        WorldFogApply();

        rdpq_mode_filter(FILTER_POINT);
        {
            /* t3d_frame_start turns on AA_STANDARD, which runs every pixel
             * through the blender against the framebuffer for edge coverage.
             * The PSX had no antialiasing, the PSX path here runs with it off
             * (rdpq_set_mode_standard), and display_init is FILTERS_RESAMPLE
             * so the coverage the RDP computes is never displayed -- it only
             * cost fill bandwidth. */
            extern int GpuNv2a_WorldAa(void);
            int aa = GpuNv2a_WorldAa();
            rdpq_mode_antialias(aa == 1 ? AA_STANDARD : aa == 2 ? AA_REDUCED : AA_NONE);
        }
#if SH_T3DW_FLAT
        rdpq_mode_alphacompare(0);
        rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
#else
        rdpq_mode_alphacompare(1);
        rdpq_mode_tlut(TLUT_RGBA16);
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
#endif
        t3d_light_set_ambient(ambWhite);
        fm_vec3_norm(&ld, &ld);
        t3d_light_set_directional(0, dirWhite, &ld);
        t3d_light_set_count(1);
        {
            /* The PSX mesh emitters reject every world quad whose
             * nclip(v0,v1,v2) <= 0 (bodyprog_80055028.c), so world geometry
             * is single-sided by design and drawing both sides doubled the
             * fill. mkworld keeps the PSX winding; under the axis-flip camera
             * above t3d reads the PSX-visible side as BACK (Harry, same
             * winding, vanished under CULL_BACK until mkchara reversed his),
             * so the side the PSX rejects is t3d's FRONT: CULL_FRONT is the
             * PSX's own rejection here. Re-applied every pass: CharaFlush sets
             * Harry's CULL_BACK between the background and foreground. */
            extern int GpuNv2a_WorldCull(void);
            int cull = GpuNv2a_WorldCull();
            int df = T3D_FLAG_SHADED;
#if !SH_T3DW_FLAT
            df |= T3D_FLAG_TEXTURED;
#endif
            if (cull == 1)      df |= T3D_FLAG_CULL_FRONT;
            else if (cull == 2) df |= T3D_FLAG_CULL_BACK;
            if (s_zActive) df |= T3D_FLAG_DEPTH;
            t3d_state_set_drawflags(df);
        }

        {
            static int s_cs;
            if ((s_cs++ & 63) == 0)
                SH_DBG("[T3DCAM] camPos=%d,%d,%d eye=%d,%d,%d fwd=%d,%d,%d h=%d ofs=%d,%d pvz=%d cell0=%d,%d",
                       (int)s_camPos[0], (int)s_camPos[1], (int)s_camPos[2],
                       (int)eye[0], (int)eye[1], (int)eye[2],
                       (int)(fwd[0]*100), (int)(fwd[1]*100), (int)(fwd[2]*100),
                       s_geomH, s_geomOfx, s_geomOfy, (int)s_playerViewZ,
                       s_drawCount > 0 ? s_drawList[0].cx : 99,
                       s_drawCount > 0 ? s_drawList[0].cz : 99);
        }
    }

    s_worldStarted = 1;
}

/* RECORD-ONLY: the actual t3d draw is deferred to ShT3d_WorldFlush in
 * FrameEnd. Drawing here (during OT build) put t3d output on the framebuffer
 * before the PSX batch flushed and presented, and it never survived to the
 * screen -- the spike proved t3d output only reaches the frame when drawn in
 * FrameEnd. Nonzero tells the game to skip its PSX per-prim path for the
 * buffer's local models; 2 = the global-PLM instances are native as well. */
int ShT3d_WorldDrawBuffer(int cellX, int cellZ, int bufIdx)
{
    WChunk* c;

    if (!s_frameActive || !ShT3d_Ready() || !s_haveView)
        return 0;

    c = ChunkFind(cellX, cellZ);
    if (c == NULL || bufIdx >= c->bufCount)
    {
        s_cnFallback++;
        return 0;
    }
    if (c->bufs[bufIdx].opaWords == 0 && c->bufs[bufIdx].semiWords == 0)
    {
        /* No native data: either genuinely empty (PSX loop no-ops cheaply)
         * or global-PLM instances the converter skips -- both want fallback. */
        return 0;
    }

    if (s_drawCount < WORLD_DRAWLIST_MAX)
    {
        s_drawList[s_drawCount].cx  = (int16_t)cellX;
        s_drawList[s_drawCount].cz  = (int16_t)cellZ;
        s_drawList[s_drawCount].buf = (int16_t)bufIdx;
        s_drawCount++;
    }
    return c->glbBaked ? 2 : 1;
}

/* Replay every recorded buffer, but only the instances on the requested side
 * of the player (wantFg: 0 = background, 1 = foreground). The background pass
 * runs before the PSX character OT and the foreground pass after it, so a
 * character correctly occludes / is occluded by world geometry with no Z
 * buffer -- the PSX ordering table's back-to-front model, at instance
 * granularity. The draw list is cleared (s_flushed) only by the LAST pass. */
/* Z-buffer path (zbuffer=1). Under painter's order the 4KB TMEM re-uploaded
 * the frame's ~83 unique tiles ~215 times (dedup=0: depth order interleaves
 * them), and every upload drains the RDP pipe -- that serialisation WAS the
 * 182ms RDP-bound frame. With depth from the Z-buffer the opaque geometry
 * can instead be drawn sorted BY TILE across every block: one record per
 * OP_TILE run of every opaque stream, sorted by (tile, pal), replayed back
 * to back so RunPass's consecutive-bind dedup collapses each tile to ONE
 * upload. The background/foreground painter split is then unnecessary: the
 * background call draws all opaque (Z-write), the character flush Z-writes
 * between, and the foreground call draws the semitransparent streams in
 * submission order (Z-test, NO write) so PSX blend order survives. */
typedef struct
{
    uint16_t       tref, pal, buf, words;
    uint16_t       gBase;    /* groupPos index of the record's first OP_VERTS */
    WChunk*        c;
    const uint8_t* p;
} ZRec;
#define ZREC_MAX 1024
static ZRec s_zrec[ZREC_MAX];
static int  s_cnZRecOverflow;

static int ZRecCmp(const void* a, const void* b)
{
    const ZRec* x = (const ZRec*)a;
    const ZRec* y = (const ZRec*)b;
    if (x->tref != y->tref) return (int)x->tref - (int)y->tref;
    return (int)x->pal - (int)y->pal;
}

static int ZRecPush(int n, WChunk* c, int b, const uint8_t* p, int start, int end,
                    uint16_t tref, uint16_t pal, int gBase)
{
    if (n >= ZREC_MAX) { s_cnZRecOverflow++; return n; }
    s_zrec[n].gBase = (uint16_t)gBase;
    s_zrec[n].tref  = tref;
    s_zrec[n].pal   = pal;
    s_zrec[n].buf   = (uint16_t)b;
    s_zrec[n].words = (uint16_t)(end - start);
    s_zrec[n].c     = c;
    s_zrec[n].p     = p + start * 2;
    return n + 1;
}

/* Split one command stream at every OP_TILE into records [tile, next tile).
 * Word strides mirror RunPass exactly; a record never includes OP_END. */
static int ZSplitStream(WChunk* c, int b, const uint8_t* p, int words, int n)
{
    int      i = 0, start = -1;
    int      g = c->bufs[b].opaGroupBase, gStart = g;
    uint16_t tref = 0, pal = 0;
    while (i < words)
    {
        uint16_t w = rd16(p + i * 2), opc = w >> 12, arg = w & 0xFFF;
        if (opc == OP_TILE)
        {
            if (start >= 0)
                n = ZRecPush(n, c, b, p, start, i, tref, pal, gStart);
            start  = i;
            gStart = g;
            tref   = rd16(p + (i + 1) * 2);
            pal    = arg;
            i += 2;
        }
        else if (opc == OP_MATRIX) i += 1;
        else if (opc == OP_VERTS)  { i += 2; g++; }
        else if (opc == OP_TRIS)   i += 1 + (arg * 3 + 1) / 2;
        else break;                 /* OP_END (or a bad opcode) */
    }
    if (start >= 0)
        n = ZRecPush(n, c, b, p, start, i, tref, pal, gStart);
    return n;
}

static void WorldFlushPass(int wantFg)
{
    int i;

    if (!wantFg)
    {
        static int s_fl;
        if ((s_fl++ & 127) == 0)
            SH_DBG("[T3DWF] flush call: drawCount=%d ready=%d haveView=%d zsort=%d",
                   s_drawCount, ShT3d_Ready(), s_haveView, s_zActive);
    }

    if (s_drawCount == 0 || !ShT3d_Ready() || !s_haveView)
    {
        if (wantFg)
        {
            s_flushed   = 1;
            s_drawCount = 0;
        }
        return;
    }

    WorldFrameStart();

    if (s_zActive)
    {
        int first = 1;
        if (!wantFg)
        {
            int n = 0;
            for (i = 0; i < s_drawCount; i++)
            {
                WChunk* c = ChunkFind(s_drawList[i].cx, s_drawList[i].cz);
                int b = s_drawList[i].buf;
                if (c == NULL || b >= c->bufCount || c->bufs[b].opaWords <= 1)
                    continue;
                n = ZSplitStream(c, b, c->cmds + c->bufs[b].opaOff,
                                 c->bufs[b].opaWords, n);
            }
            qsort(s_zrec, n, sizeof(ZRec), ZRecCmp);
            for (i = 0; i < n; i++)
            {
                const ZRec* r = &s_zrec[i];
                RunPass(r->p, r->words, r->c->verts + r->c->bufs[r->buf].vbase / 2,
                        r->c->mats + s_matPhase * r->c->instCount, r->c->groupPos,
                        r->c->viewRow, r->gBase, 2, 0, BindWorldTile, first);
                first = 0;
                s_cnBlocks++;
            }
            /* Decals after every opaque surface they could sit on. decal_mode
             * 0 (default) keeps the standard compare: a decal is strictly in
             * front of its wall, so it passes, and nothing later repaints it.
             * 1 = ZMODE_DECAL for A/B on hardware -- it needs |z - zbuf| <= dz,
             * which a decal several units in front of a face-on wall FAILS. */
            {
                extern int PcConfig_N64DecalMode(void);
                int dm = PcConfig_N64DecalMode();
                if (dm == 1)
                    rdpq_mode_zmode(ZMODE_DECAL);
                if (dm == 2)
                {
                    s_bindTintDecal = 1;
                    s_lastBoundOk   = 0;   /* force a bind so the tint applies */
                }
                {
                    /* Every decal stream opens with OP_MATRIX, so its
                     * instances re-push under the biased camera. */
                    int biased = 0;
                    for (i = 0; i < s_drawCount; i++)
                    {
                        WChunk* c = ChunkFind(s_drawList[i].cx, s_drawList[i].cz);
                        int b = s_drawList[i].buf;
                        if (c == NULL || b >= c->bufCount || c->bufs[b].decalWords <= 1)
                            continue;
                        if (!biased && s_decalBias > 0)
                        {
                            /* RunPass ends synced with its matrix popped. */
                            t3d_viewport_attach(&s_wvpDecal);
                            biased = 1;
                        }
                        RunPass(c->cmds + c->bufs[b].decalOff, c->bufs[b].decalWords,
                                c->verts + c->bufs[b].vbase / 2,
                                c->mats + s_matPhase * c->instCount, NULL, c->viewRow,
                                0, 0, 0, BindWorldTile, first);
                        first = 0;
                        s_cnBlocks++;
                    }
                    if (biased)
                        t3d_viewport_attach(&s_wvp);
                }
                if (dm == 1)
                    rdpq_mode_zmode(ZMODE_STANDARD);
                if (dm == 2)
                {
                    s_bindTintDecal = 0;
                    s_lastBoundOk   = 0;
                }
            }
        }
        else
        {
            /* Semitransparent: Z-tested against opaque world + character, not
             * written, in submission order. */
            rdpq_mode_zbuf(true, false);
            for (i = 0; i < s_drawCount; i++)
            {
                WChunk* c = ChunkFind(s_drawList[i].cx, s_drawList[i].cz);
                int b = s_drawList[i].buf;
                if (c == NULL || b >= c->bufCount || c->bufs[b].semiWords <= 1)
                    continue;
                RunPass(c->cmds + c->bufs[b].semiOff, c->bufs[b].semiWords,
                        c->verts + c->bufs[b].vbase / 2,
                        c->mats + s_matPhase * c->instCount, c->groupPos, c->viewRow,
                        c->bufs[b].semiGroupBase, 2, 1, BindWorldTile, first);
                first = 0;
                s_cnBlocks++;
            }
        }
    }
    else
    {
        for (i = 0; i < s_drawCount; i++)
        {
            WChunk* c = ChunkFind(s_drawList[i].cx, s_drawList[i].cz);
            int b = s_drawList[i].buf;
            const T3DVertPacked* bverts;
            if (c == NULL || b >= c->bufCount)
                continue;
            bverts = c->verts + c->bufs[b].vbase / 2;
            if (c->bufs[b].opaWords > 1)
            {
                RunPass(c->cmds + c->bufs[b].opaOff, c->bufs[b].opaWords, bverts,
                        c->mats + s_matPhase * c->instCount, c->groupPos,
                        c->viewRow, c->bufs[b].opaGroupBase, wantFg, 0, BindWorldTile, 1);
                s_cnBlocks++;
            }
            /* No Z: decals are plain opaque geometry, background pass only
             * (groupPos NULL + wantFg -> RunPass skips them in the fg call). */
            if (c->bufs[b].decalWords > 1)
            {
                RunPass(c->cmds + c->bufs[b].decalOff, c->bufs[b].decalWords, bverts,
                        c->mats + s_matPhase * c->instCount, NULL,
                        c->viewRow, 0, wantFg, 0, BindWorldTile, 1);
                s_cnBlocks++;
            }
            if (c->bufs[b].semiWords > 1)
            {
                RunPass(c->cmds + c->bufs[b].semiOff, c->bufs[b].semiWords, bverts,
                        c->mats + s_matPhase * c->instCount, c->groupPos,
                        c->viewRow, c->bufs[b].semiGroupBase, wantFg, 1, BindWorldTile, 1);
                s_cnBlocks++;
            }
        }
    }
    if (wantFg)
    {
        s_flushed = 1;
        /* Consume the list at the end of the frame's LAST pass. A frame
         * whose OT build never ran WorldViewSet (menus, the inventory) then
         * flushes NOTHING, instead of replaying the last gameplay frame's
         * world behind an isolated screen -- which both painted the world
         * under the inventory and raced its heavy item-texture streaming. */
        s_drawCount = 0;
    }

    /* Fence the pass boundary: the PSX walk's first rdpq mode change is
     * auto-synced only against rdpq's OWN prims -- our t3d triangles are
     * invisible to that tracker, so drain the pipe by hand before handing
     * the stream back. */
    rdpq_sync_pipe();

    /* t3d_frame_start reprogrammed rdpq modes; the PSX OT walk that follows
     * memoises its mode application, so tell it the mode is dirty or the
     * first PSX prim inherits t3d's combiner/blender. */
    {
        extern void GpuNv2a_PsxModeInvalidate(void);
        GpuNv2a_PsxModeInvalidate();
    }
}

/* Background world: drawn from game_main BEFORE GsDrawOt(OT0), so it lands
 * under the characters/items on the live, about-to-present surface. */
void ShT3d_WorldFlush(void)
{
    WorldFlushPass(0);
}

/* Foreground world: drawn AFTER GsDrawOt(OT0) (characters) and before OT2
 * (2D UI), so geometry nearer than the player occludes him. */
void ShT3d_WorldFlushForeground(void)
{
    WorldFlushPass(1);
}

void ShT3d_NotifyFrameBegin(void)
{
    s_frameActive  = 1;
    s_worldStarted = 0;
    /* NOT s_drawCount = 0 here: FrameBegin fires on a mid-frame VSync between
     * the OT build and the GsDrawOt flush. The list is bounded by s_flushed. */
}

/* "Will native draw this frame": the draw is deferred to FrameEnd, but the
 * draw LIST is filled during OT build (WorldDrawBuffer), which runs before
 * Gfx_2dEffectsDraw -- so s_drawCount is the correct suppression signal for
 * the fog/brightness quads. (s_cnBlocks isn't incremented until the flush.) */
int ShT3d_WorldDrewThisFrame(void)
{
    return s_drawCount > 0;
}

void ShT3d_NotifyFrameEnd(void)
{
    static int s_census;
    s_frameActive = 0;
    if (s_worldStarted && (s_census++ & 127) == 0)
        SH_DBG("[T3DW] blocks=%d fallback=%d tileRam=%dK tileUp=%d dedup=%d fog=%d fogFull=%d fogCull=%d culled=%d",
               s_cnBlocks, s_cnFallback, s_tileRam / 1024, s_cnTileUp, s_cnTileDedup,
               s_fogOn, (int)s_fogFullZ, (int)s_fogCullZ, s_cnFogCulled);
    s_cnFogCulled = 0;
    s_cnBlocks = s_cnFallback = 0;
    {
        /* A frame that drew no world (menus, 2D screens) must not leave the
         * PSX path in fog-blend mode. */
        extern int g_N64FogBlend;
        if (!s_fogSetThisFrame)
            g_N64FogBlend = 0;
        s_fogSetThisFrame = 0;
    }
    s_cnTileUp = s_cnTileDedup = 0;
}

/* ============================================================ characters */

/* Eager-load a character SHT: every tile into the char pool (never evicted),
 * palettes resident. Same file layout as the area SHT (see ShtOpen). */
static int CharaShtLoad(const char* name)
{
    char    fn[24];
    uint8_t hdr[8];
    FILE*   f;
    int     i, fileTiles, tiles, pals, total;

    snprintf(fn, sizeof fn, "%s.SHT", name);
    f = WOpenRoot("N64C", fn);
    if (f == NULL)
        return 0;
    if (fread(hdr, 1, 8, f) != 8 || rd32(hdr) != SHT_MAGIC)
    {
        fclose(f);
        return 0;
    }
    fileTiles = rd16(hdr + 4);
    pals      = rd16(hdr + 6);
    tiles     = fileTiles;
    if (tiles > CTILE_SLOTS)
    {
        SH_DBG("[T3DC] %s: %d tiles > %d slots -- excess groups will drop", name, tiles, CTILE_SLOTS);
        tiles = CTILE_SLOTS;
    }
    for (i = 0; i < tiles; i++)
    {
        uint8_t  meta[16];
        WTile*   t = &s_cTiles[i];
        uint32_t pixOff, pixLen;
        fseek(f, 8 + i * 16, SEEK_SET);
        if (fread(meta, 1, 16, f) != 16) { fclose(f); return 0; }
        t->fmt = meta[0];
        t->w   = rd16(meta + 2);
        t->h   = rd16(meta + 4);
        pixOff = rd32(meta + 8);
        pixLen = rd32(meta + 12);
        if (pixLen > TILE_SLOT_BYTES) { fclose(f); return 0; }
        t->pix = s_cPool[i];
        fseek(f, pixOff, SEEK_SET);
        if (fread(t->pix, 1, pixLen, f) != pixLen) { t->pix = NULL; fclose(f); return 0; }
        t->pixLen = pixLen;
        data_cache_hit_writeback(t->pix, pixLen);
        t->sthIdx = i;
        t->refs   = 1;
    }
    s_cTileCount = tiles;

    s_cPalOffsets = malloc(pals * 4);
    s_cPalWords   = malloc(pals * 2);
    if (s_cPalOffsets == NULL || s_cPalWords == NULL) { fclose(f); return 0; }
    total = 0;
    fseek(f, 8 + fileTiles * 16, SEEK_SET);
    for (i = 0; i < pals; i++)
    {
        uint8_t pm[8];
        if (fread(pm, 1, 8, f) != 8) { fclose(f); return 0; }
        s_cPalWords[i]   = rd16(pm);
        s_cPalOffsets[i] = rd32(pm + 4);
        total += (s_cPalWords[i] + 3) & ~3;
    }
    s_cPals = malloc_uncached(total * 2);
    if (s_cPals == NULL) { fclose(f); return 0; }
    total = 0;
    for (i = 0; i < pals; i++)
    {
        fseek(f, s_cPalOffsets[i], SEEK_SET);
        fread((uint8_t*)s_cPals + total * 2, 1, s_cPalWords[i] * 2, f);
        s_cPalOffsets[i] = total;   /* now: word offset into s_cPals */
        total += (s_cPalWords[i] + 3) & ~3;
    }
    s_cPalCount = pals;
    fclose(f);
    SH_DBG("[T3DC] %s.SHT: %d tiles (%dKB) %d pals", name, tiles,
           (tiles * TILE_SLOT_BYTES) / 1024, pals);
    return 1;
}

/* Load ONE character natively (tried once; a missing asset leaves it on the
 * PSX path for good, logged). The SHW's cell field is the character marker
 * mkchara.py writes (-128,-128). */
static int CharaLoad(const char* name)
{
    char     fn[24];
    uint8_t  hdr[0x14];
    FILE*    f;
    int      bufCount, instCount, refCount;
    uint32_t instOff, refsOff;

    if (s_charLoaded)
        return 1;
    if (s_charLoadTried)
        return 0;
    s_charLoadTried = 1;

    if (!CharaShtLoad(name))
    {
        SH_DBG("[T3DC] no N64C/%s.SHT -- %s stays on the PSX path", name, name);
        return 0;
    }
    snprintf(fn, sizeof fn, "%s.SHW", name);
    f = WOpenRoot("N64C", fn);
    if (f == NULL)
    {
        SH_DBG("[T3DC] no N64C/%s.SHW", name);
        return 0;
    }
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || rd32(hdr) != SHW_MAGIC ||
        (int8_t)hdr[4] != -128)
    {
        SH_DBG("[T3DC] %s.SHW: bad header (not a character SHW)", name);
        fclose(f);
        return 0;
    }
    bufCount  = rd16(hdr + 6) & SHW_COUNT_MASK;
    instCount = rd16(hdr + 8);
    refCount  = rd16(hdr + 10);
    instOff   = rd32(hdr + 0xC);
    refsOff   = rd32(hdr + 0x10);
    if (instCount > 32)
    {
        SH_DBG("[T3DC] %s: %d parts > 32 (mask width)", name, instCount);
        fclose(f);
        return 0;
    }
    memset(&s_charChunk, 0, sizeof s_charChunk);
    s_charChunk.cellX = s_charChunk.cellZ = -128;
    if (!ShwLoadBody(&s_charChunk, s_charArena, CHAR_ARENA_BYTES, f, name,
                     bufCount, instCount, refCount, instOff, refsOff, 0, 0, 1))
    {
        fclose(f);
        return 0;
    }
    fclose(f);

    /* Per-part local centroid = average of the part's group-AABB centres
     * (groupPos, raw vert space). Groups run buf0-opa, buf0-semi, buf1-opa ...
     * so part i owns [bufs[i].opaGroupBase, bufs[i+1].opaGroupBase). Falls back
     * to (0,0,0) = the joint if groupPos did not fit the arena. */
    {
        WChunk* c = &s_charChunk;
        int i;
        for (i = 0; i < c->instCount && i < 32; i++)
        {
            long sx = 0, sy = 0, sz = 0;
            int g0 = 0, g1 = 0, g, n;
            if (c->groupPos != NULL)
            {
                g0 = c->bufs[i].opaGroupBase;
                g1 = (i + 1 < c->bufCount) ? c->bufs[i + 1].opaGroupBase : c->groupCount;
            }
            n = g1 - g0;
            for (g = g0; g < g1; g++)
            {
                sx += c->groupPos[g * 6 + 0];
                sy += c->groupPos[g * 6 + 1];
                sz += c->groupPos[g * 6 + 2];
            }
            s_charPartCent[i][0] = (int16_t)(n > 0 ? sx / n : 0);
            s_charPartCent[i][1] = (int16_t)(n > 0 ? sy / n : 0);
            s_charPartCent[i][2] = (int16_t)(n > 0 ? sz / n : 0);
        }
    }

    /* A part the animation never writes collapses to a point far behind the
     * eye (PSX view -z is behind the camera after the GL flip), so a stale
     * pose can never show. */
    {
        T3DMat4 hm;
        memset(&hm, 0, sizeof hm);
        hm.m[3][2] = -20000.0f;
        hm.m[3][3] = 1.0f;
        t3d_mat4_to_fixed(&s_charHidden, &hm);
    }
    s_charChunk.inUse = 1;
    s_charLoaded = 1;
    /* Weapon count rides in the header's cellZ byte (-128 + N, see mkchara);
     * the weapons are the LAST N parts. An older bake reads N = 0. */
    {
        int weapons = (int)(int8_t)hdr[5] + 128;
        s_charWeaponBase = (weapons == CHAR_WEAPON_COUNT && instCount > weapons)
                               ? instCount - weapons : -1;
    }
    SH_DBG("[T3DC] %s resident: parts=%d groups=%d tiles=%d pals=%d weaponBase=%d",
           name, instCount, s_charChunk.groupCount, s_cTileCount, s_cPalCount,
           s_charWeaponBase);
    return 1;
}

/* Bracket a character's bone loop. Returns 1 when THIS character draws
 * natively (the bone loop then feeds ShT3d_CharaBone and skips the software
 * GTE per-part draw), 0 for the PSX path. Harry only for now; other charas
 * reuse the path once converted. */
int ShT3d_CharaDrawBegin(int isHarry)
{
    extern int GpuNv2a_NativeCharaEnabled(void);
    s_charActive = 0;
    if (!isHarry || !GpuNv2a_NativeCharaEnabled() || !ShT3d_Ready())
        return 0;
    if (!CharaLoad("HERO"))
        return 0;
    s_charActive = 1;
    s_charPhase ^= 1;     /* the RSP may still replay last frame's half */
    s_charMask   = 0;
    s_charRHandPart = -1; /* re-found by this frame's bone loop; s_charHeldWeapon was
                           * already set by WorldGfx_HeldItemDraw earlier this frame */
    s_charDiagMode = 0;   /* always textured; config chara_debug forces flat */
    return 1;
}

/* Returns the drawn character's mean part-centroid view depth (raw Q8, the
 * PSX OT's unit) so the caller can place an OT0 marker where his prims would
 * have sorted, or -1 when he did not draw natively this frame. */
int ShT3d_CharaDrawEnd(void)
{
    const WChunk* c = &s_charChunk;
    float sum = 0.0f;
    int   i, n = 0;
    int   was = s_charActive;
    s_charActive = 0;
    if (!was || s_charMask == 0)
        return -1;
    for (i = 0; i < c->instCount && i < 32; i++)
        if (s_charMask & (1u << i))
        {
            sum += c->viewRow[i * 4 + 3];
            n++;
        }
    if (n == 0 || sum <= 0.0f)
        return -1;
    return (int)(sum / (float)n);
}

/* The bone loop hands over each part's GAME view matrix (Vw_CoordToWorldAnd
 * ViewMatrices of the bone coord: Q12 rotation m9 row-major, Q8 translation
 * t3) -- exactly what ComposeChunkViews derives for a world instance, so the
 * part lands where the PSX path would land it. */
int ShT3d_CharaBone(int partIdx, int boneIdx, const short* m9, const int* t3)
{
    WChunk*    c = &s_charChunk;
    T3DMat4    m;
    T3DMat4FP* dst;
    int        r, cc;

    if (!s_charActive || partIdx < 0 || partIdx >= c->instCount)
        return 0;
    /* Only the ACTIVE 10RHAND* variant reaches here (the hidden ones are
     * skipped by the bone loop's field_0 gate), so this is the part whose
     * matrix the held weapon must share. */
    if (boneIdx == CHAR_RIGHT_HAND_BONE)
        s_charRHandPart = partIdx;
    /* Scale the WHOLE transform (rotation AND translation) by 1/8 so the
     * character lands in the world's /8 view space. The world pre-divides its
     * verts by 8 at bake; the character keeps RAW verts (dividing THEM by 8
     * rounds his ~+-120 features to zero -> collapsed geometry), so the /8 goes
     * into the matrix instead: view = (R/8)*vert_raw + T/8 = (R*vert_raw+T)/8.
     * The 1/8 cancels in the perspective divide, so his SCREEN position is
     * identical to before -- but his DEPTH now matches the world's, which is
     * what lets the Z-buffer occlude him against walls (zbuffer=1). Verts stay
     * raw = full precision (the RSP does the /8 at fixed-point). */
    memset(&m, 0, sizeof m);
    {
        extern int GpuNv2a_CharaXpose(void);
        int xpose = !GpuNv2a_CharaXpose();   /* default: transpose (matches the world) */
        for (r = 0; r < 3; r++)
            for (cc = 0; cc < 3; cc++)
                if (xpose) m.m[cc][r] = (float)m9[r * 3 + cc] / 4096.0f / 8.0f;
                else       m.m[r][cc] = (float)m9[r * 3 + cc] / 4096.0f / 8.0f;
    }
    m.m[3][0] = (float)t3[0] / 8.0f;
    m.m[3][1] = (float)t3[1] / 8.0f;
    m.m[3][2] = (float)t3[2] / 8.0f;
    m.m[3][3] = 1.0f;
    dst = c->mats + s_charPhase * c->instCount + partIdx;
    t3d_mat4_to_fixed(dst, &m);
    /* [T3DCB] one-shot: is this part's view matrix SANE? Compare to the
     * player split depth (s_playerViewZ, world/8) -- a part translation Z
     * near that = correct; huge/tiny/negative = the parts land off-screen or
     * behind the eye, which is invisible-but-submitted. Chest (0) + a limb. */
    {
        /* Log ALL parts of the first frame (23 in bone order): their view
         * translations should trace a standing HARRY -- head high, feet low,
         * hands at the sides. Scattered => a placement/mapping bug; coherent
         * but still broken on screen => rendering (backfaces/order). */
        static int s_cb;
        if (s_cb < 24)
        {
            s_cb++;
            /* FULL matrix (all 9 rot shorts + 3 trans) so the runtime geometry
             * can be reproduced offline vs the ilm_obj bake -- if THIS renders
             * coherent, the bug is downstream (cull/t3d), not the transform. */
            SH_DBG("[T3DCBM] part=%2d m9=%d,%d,%d,%d,%d,%d,%d,%d,%d t=%d,%d,%d",
                   partIdx, (int)m9[0], (int)m9[1], (int)m9[2],
                   (int)m9[3], (int)m9[4], (int)m9[5],
                   (int)m9[6], (int)m9[7], (int)m9[8],
                   (int)t3[0], (int)t3[1], (int)t3[2]);
        }
    }
    c->viewRow[partIdx * 4 + 0] = (float)m9[6] / 4096.0f;
    c->viewRow[partIdx * 4 + 1] = (float)m9[7] / 4096.0f;
    c->viewRow[partIdx * 4 + 2] = (float)m9[8] / 4096.0f;
    /* Sort key = the part's CENTROID view depth, not its joint (t3[2]).
     * depth = centroid_local . viewRow2 + t3[2]. The gun's mass extends
     * forward of the hand joint, so this sorts it in front of the torso the
     * way the PSX ordering table did -- joint-Z sorted it with the torso and
     * it clipped through. Centroid is (0,0,0) fallback => identical to before. */
    {
        const int16_t* cen = s_charPartCent[partIdx];
        c->viewRow[partIdx * 4 + 3] =
            (float)cen[0] * (float)m9[6] / 4096.0f +
            (float)cen[1] * (float)m9[7] / 4096.0f +
            (float)cen[2] * (float)m9[8] / 4096.0f +
            (float)t3[2];
    }
    s_charMask |= 1u << partIdx;
    return 1;
}

/* Draw the character where its OT prims used to sit: after the background
 * world pass, before GsDrawOt(OT0) (items/effects still composite on top),
 * before the foreground world pass (which occludes him). Tile-grouped: the
 * stream is sorted by (tile, pal, part) at convert time, so each of his tiles
 * loads ONCE per frame instead of once per primitive. */
void ShT3d_CharaFlush(void)
{
    WChunk*    c = &s_charChunk;
    T3DMat4FP* mats;
    int        b, i, drawn = 0;
    static int s_census;

    if (!s_charLoaded || s_charMask == 0 || !s_haveView || !ShT3d_Ready())
        return;
    mats = c->mats + s_charPhase * c->instCount;
    for (i = 0; i < c->instCount; i++)
    {
        if (s_charMask & (1u << i))
            drawn++;
        else
            memcpy(&mats[i], &s_charHidden, sizeof mats[i]);
    }
    /* The equipped weapon (a baked extra part, see s_charWeaponBase) rides
     * the ACTIVE right-hand variant's matrix -- the exact matrix
     * WorldGfx_HeldItemDraw would have attached it with on the PSX path, so it
     * lands in the hand by construction AND is Z-tested against the body.
     * Its sort depth is recomputed from ITS OWN centroid (the barrel reaches
     * forward of the grip) with the hand's rotation row and translation. */
    if (s_charHeldWeapon >= 0 && s_charWeaponBase >= 0 && s_charRHandPart >= 0)
    {
        int wb = s_charWeaponBase + s_charHeldWeapon;
        if (wb < c->instCount && wb < 32 && !(s_charMask & (1u << wb)))
        {
            const float*   rr = &c->viewRow[s_charRHandPart * 4];
            const int16_t* ch = s_charPartCent[s_charRHandPart];
            const int16_t* cw = s_charPartCent[wb];
            float tz = rr[3] - ((float)ch[0] * rr[0] + (float)ch[1] * rr[1] + (float)ch[2] * rr[2]);
            memcpy(&mats[wb], &mats[s_charRHandPart], sizeof mats[wb]);
            c->viewRow[wb * 4 + 0] = rr[0];
            c->viewRow[wb * 4 + 1] = rr[1];
            c->viewRow[wb * 4 + 2] = rr[2];
            c->viewRow[wb * 4 + 3] = (float)cw[0] * rr[0] + (float)cw[1] * rr[1] +
                                     (float)cw[2] * rr[2] + tz;
            s_charMask |= 1u << wb;
            drawn++;
        }
    }
    data_cache_hit_writeback(mats, sizeof(T3DMat4FP) * c->instCount);

    {
        int tris0 = s_cnRunPassTris, miss0 = s_cnTileMiss;
        /* Draw parts BACK-TO-FRONT (painter's -- no Z buffer): each buffer is
         * one rigid part (mkchara: buf == inst == part index), so sort the
         * buffers by their part's composed view depth (viewRow[.z], world/8)
         * FARTHEST first and nearer parts paint over. Without this the parts
         * draw in the stream's tile order and punch through each other. */
        int order[64], nb = c->bufCount, oi, oj;
        if (nb > 64) nb = 64;
        for (oi = 0; oi < nb; oi++) order[oi] = oi;
        for (oi = 1; oi < nb; oi++)
        {
            int   key = order[oi];
            float kz  = c->viewRow[key * 4 + 3];
            oj = oi - 1;
            while (oj >= 0 && c->viewRow[order[oj] * 4 + 3] < kz)
            { order[oj + 1] = order[oj]; oj--; }
            order[oj + 1] = key;
        }
        WorldFrameStart();
        /* The character gets the Z-buffer even when the world stays
         * painter's (zbuffer=2): the PSX sorted his polygons into the OT one
         * by one, so a holstered gun inside the trousers sorted behind them.
         * Per-PART painter's order (below) cannot express that; per-pixel
         * depth can. The Z image was cleared to far this frame and nothing
         * before him wrote it, so he only ever tests against himself. */
        if (s_zChara)
            rdpq_mode_zbuf(true, true);
        {
            extern int GpuNv2a_CharaCull(void);
            /* Backface cull (config chara_cull: -1 auto, 0 none, 1 back, 2
             * front). Without Z a closed mesh paints far faces over near ones,
             * so cull BACK (mkchara reversed the winding for exactly that).
             * With Z the depth test hides back faces anyway, and culling only
             * risks holes wherever our screen-space winding disagrees with
             * the PSX's nclip for a part -- so auto means none there. */
            int cull = GpuNv2a_CharaCull();
            int df = T3D_FLAG_SHADED | T3D_FLAG_TEXTURED;
            if (cull < 0)
                cull = s_zChara ? 0 : 1;
            if (cull == 1)      df |= T3D_FLAG_CULL_BACK;
            else if (cull == 2) df |= T3D_FLAG_CULL_FRONT;
            if (s_zChara)       df |= T3D_FLAG_DEPTH;   /* Z-write: occludes / is occluded */
            t3d_state_set_drawflags(df);
        }
        for (oi = 0; oi < nb; oi++)
        {
            const T3DVertPacked* bverts;
            b = order[oi];
            /* Skip parts the animation did NOT write this frame (not in the
             * mask). Their matrix is s_charHidden (collapsed behind the eye),
             * but a part's prims can reference SEAM verts owned by a DIFFERENT,
             * visible bone (e.g. the hidden empty-hand 10RHAND welds to the
             * visible forearm 09RZEN): those seam verts stay put while the
             * part's own verts collapse, stretching a triangle across the whole
             * screen (the "spike from his hand"). A huge/degenerate tri also
             * risks overrunning the RDP command buffer -> the RSP crash. Drawing
             * a hidden part is pointless anyway (it is collapsed), so skip it. */
            if (!(s_charMask & (1u << b)))
                continue;
            bverts = c->verts + c->bufs[b].vbase / 2;
            if (c->bufs[b].opaWords > 1)
                RunPass(c->cmds + c->bufs[b].opaOff, c->bufs[b].opaWords, bverts,
                        mats, NULL, c->viewRow, 0, 0, 0, BindCharTile, 1);
            if (c->bufs[b].semiWords > 1)
                RunPass(c->cmds + c->bufs[b].semiOff, c->bufs[b].semiWords, bverts,
                        mats, NULL, c->viewRow, 0, 0, 1, BindCharTile, 1);
        }
        if ((s_census & 127) == 0)
            SH_DBG("[T3DCB2] chara drew %d tris, %d tile-misses (depth-sorted %d parts)",
                   s_cnRunPassTris - tris0, s_cnTileMiss - miss0, nb);
    }
    /* Fence: the PSX walk's first mode change is auto-synced only against
     * rdpq's own prims, never our t3d triangles. */
    rdpq_sync_pipe();
    {
        extern void GpuNv2a_PsxModeInvalidate(void);
        GpuNv2a_PsxModeInvalidate();
    }
    if ((s_census++ & 127) == 0)
        SH_DBG("[T3DC] native chara: parts=%d/%d groups=%d weapon=%d", drawn, c->instCount,
               c->groupCount, s_charHeldWeapon);
    s_charMask = 0;
    s_charHeldWeapon = -1;   /* re-armed by next frame's WorldGfx_HeldItemDraw */
}

/* Called from WorldGfx_HeldItemDraw (N64) BEFORE its PSX per-prim draw with the
 * slot of the equipped HERO-textured weapon (mkchara --weapon order: knife
 * hammer axe handgun rifle shotgun), -1 for anything else (pipe, cutscene
 * props). Returns 1 when the native character will draw it this frame, so the
 * caller skips the depth-less PSX draw and the weapon is not painted twice.
 * Runs before ShT3d_CharaDrawBegin in the same frame, so it gates on the
 * resident asset rather than s_charActive; the first frame after a fresh
 * CharaLoad falls back to PSX once. */
int ShT3d_HeldItemNative(int slot)
{
    extern int GpuNv2a_NativeCharaEnabled(void);
    s_charHeldWeapon = -1;
    if (slot < 0 || slot >= CHAR_WEAPON_COUNT || s_charWeaponBase < 0 ||
        !s_charLoaded || !GpuNv2a_NativeCharaEnabled() || !ShT3d_Ready())
        return 0;
    s_charHeldWeapon = slot;
    return 1;
}

/* ---------------------------------------------------------- world objects */
/* The cell's un-instanced LM models (mkworld OBJ1 trailer: the *_HID item
 * pickups game logic places at runtime) are baked as extra buffers with a
 * placeholder instance each. Gfx_WorldObjectDraw offers every object's NAME
 * and view matrix here; a match in its own chunk is recorded and drawn by
 * ShT3d_WorldObjectsFlush with the world's Z-buffer, so an ammo box sits ON
 * the counter and is occluded like any wall -- instead of being painted over
 * everything by the depth-less PSX OT (the "pickups float / shift with the
 * camera"). Anything unmatched stays on the PSX path. */
#define WOBJ_MAX 29     /* WORLD_OBJECT_COUNT_MAX */
typedef struct { WChunk* c; int buf, inst; short m9[9]; int t3[3]; } WObjRec;
static WObjRec s_wobj[WOBJ_MAX];
static int     s_wobjCount;

/* 8-char LM names, NUL- or space-padded on either side. */
static int NameEq8(const char* a, const char* b)
{
    int i;
    for (i = 0; i < 8; i++)
    {
        char x = (a[i] == ' ') ? 0 : a[i];
        char y = (b[i] == ' ') ? 0 : b[i];
        if (x != y)
            return 0;
        if (x == 0)
            return 1;
    }
    return 1;
}

int ShT3d_WorldObjectDraw(const char* name8, int cellX, int cellZ, const short* m9, const int* t3)
{
    int pass, i, k;
    if (!s_haveView || !ShT3d_Ready() || s_wobjCount >= WOBJ_MAX)
        return 0;
    /* Own chunk first (a name like ITEM_HID repeats across cells); any
     * resident chunk as a fallback so a cell-coordinate mismatch degrades to
     * "first match" rather than back to the PSX path. */
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i <= MAX_WCHUNKS; i++)
        {
            /* slot MAX_WCHUNKS = the area's item pseudo-cell (lmIdx-2 objects
             * arrive keyed to its -127,-127 sentinel cell) */
            WChunk* c = (i < MAX_WCHUNKS) ? &s_chunks[i] : &s_itemChunk;
            if (!c->inUse || c->objCount == 0)
                continue;
            if (pass == 0 && (c->cellX != cellX || c->cellZ != cellZ))
                continue;
            for (k = 0; k < c->objCount; k++)
                if (NameEq8(c->objNames[k], name8))
                {
                    WObjRec* r = &s_wobj[s_wobjCount++];
                    r->c    = c;
                    r->buf  = c->objBufBase + k;
                    r->inst = c->objInstBase + k;
                    memcpy(r->m9, m9, sizeof r->m9);
                    memcpy(r->t3, t3, sizeof r->t3);
                    return 1;
                }
        }
    return 0;
}

/* Drawn right after the character (game_main), Z-tested against the
 * background world and before the foreground pass. */
void ShT3d_WorldObjectsFlush(void)
{
    int i, r, cc, drawn = 0;
    static int s_census;

    if (s_wobjCount == 0)
        return;
    if (!s_haveView || !ShT3d_Ready())
    {
        s_wobjCount = 0;
        return;
    }
    WorldFrameStart();      /* world lights, cull, Z mode and draw flags */
    for (i = 0; i < s_wobjCount; i++)
    {
        WObjRec*   o    = &s_wobj[i];
        WChunk*    c    = o->c;
        T3DMat4FP* mats = c->mats + s_matPhase * c->instCount;
        T3DMat4    m;
        const T3DVertPacked* bverts;

        if (!c->inUse || o->buf >= c->bufCount || o->inst >= c->instCount)
            continue;
        /* ComposeChunkViews' convention: raw Q8 verts, the 1/8 in the matrix
         * (rotation and translation). The placeholder matrix that compose
         * wrote this frame is overwritten here, before use. */
        memset(&m, 0, sizeof m);
        for (r = 0; r < 3; r++)
            for (cc = 0; cc < 3; cc++)
                m.m[cc][r] = (float)o->m9[r * 3 + cc] / 4096.0f / 8.0f;
        m.m[3][0] = (float)o->t3[0] / 8.0f;
        m.m[3][1] = (float)o->t3[1] / 8.0f;
        m.m[3][2] = (float)o->t3[2] / 8.0f;
        m.m[3][3] = 1.0f;
        t3d_mat4_to_fixed(&mats[o->inst], &m);
        data_cache_hit_writeback(&mats[o->inst], sizeof mats[o->inst]);
        c->viewRow[o->inst * 4 + 0] = (float)o->m9[6] / 4096.0f;
        c->viewRow[o->inst * 4 + 1] = (float)o->m9[7] / 4096.0f;
        c->viewRow[o->inst * 4 + 2] = (float)o->m9[8] / 4096.0f;
        c->viewRow[o->inst * 4 + 3] = (float)o->t3[2] / 8.0f;

        bverts = c->verts + c->bufs[o->buf].vbase / 2;
        if (c->bufs[o->buf].opaWords > 1)
            RunPass(c->cmds + c->bufs[o->buf].opaOff, c->bufs[o->buf].opaWords, bverts,
                    mats, NULL, c->viewRow, 0, 0, 0, BindWorldTile, 1);
        if (c->bufs[o->buf].semiWords > 1)
        {
            rdpq_mode_zbuf(true, false);
            RunPass(c->cmds + c->bufs[o->buf].semiOff, c->bufs[o->buf].semiWords, bverts,
                    mats, NULL, c->viewRow, 0, 0, 1, BindWorldTile, 1);
            rdpq_mode_zbuf(s_zActive, s_zActive);
        }
        drawn++;
    }
    /* Same fence as CharaFlush: the PSX walk's next mode change is auto-
     * synced only against rdpq's own prims, never our t3d triangles. */
    rdpq_sync_pipe();
    {
        extern void GpuNv2a_PsxModeInvalidate(void);
        GpuNv2a_PsxModeInvalidate();
    }
    if ((s_census++ & 127) == 0)
        SH_DBG("[T3DWO] native world objects: %d/%d drawn", drawn, s_wobjCount);
    s_wobjCount = 0;
}
