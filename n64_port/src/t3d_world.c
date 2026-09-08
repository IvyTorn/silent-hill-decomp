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
#define SHT_MAGIC 0x53485431u /* 'SHT1' */

#define OP_TILE   0x1
#define OP_MATRIX 0x2
#define OP_VERTS  0x3
#define OP_TRIS   0x4
#define OP_END    0xF

/* Diagnostic: draw the whole world as flat shaded (vertex colour), no texture,
 * to isolate geometry-projects from texture problems. 0 = real textured path. */
#define SH_T3DW_FLAT 0

/* Four cells around the player go native, the rest stay on the PSX fallback;
 * grows once the strip-IPD funding lands. */
#define MAX_WCHUNKS   3

/* Chunk data (verts + matrices + tile refs) lives in a FIXED per-slot arena,
 * not the heap: at map time the game runs the heap down to double digits and
 * fragments the rest, which first starved these allocations and then crashed
 * libdragon mid-block-recording (rspq_next_buffer memsets an unchecked
 * malloc). The arena caps what a cell may need; bigger cells stay PSX. */
#define WCHUNK_ARENA_BYTES (60 * 1024)  /* 56K left no room for the group
                                         * centroids (ERFF00: 144 groups =
                                         * 1.7K over); heap had 242K free */

/* Every SHT tile is at most 2048 bytes BY CONSTRUCTION (the TMEM budget:
 * 4096 CI4 texels or 2048 CI8 texels, both 2KB), so tile pixels live in a
 * static fixed-slot pool -- 83 small heap mallocs per chunk fragmented the
 * heap until the 48KB vertex block could not be placed at 136KB free. */
#define TILE_SLOT_BYTES 2048
#define TILE_SLOTS      96

typedef struct
{
    int16_t  sthIdx;    /* -1 = free */
    int16_t  refs;
    uint8_t  fmt;       /* 0 = CI4, 1 = CI8 */
    uint8_t  pad;
    uint16_t w, h;
    uint32_t pixLen;
    void*    pix;       /* slot in s_tilePool, written back for RDP DMA */
} WTile;

typedef struct
{
    /* Byte offsets into the chunk's arena cmd region; 0 words = empty. */
    uint32_t opaOff, semiOff;
    uint16_t opaWords, semiWords;
    uint16_t vbase;      /* first vert (pair-aligned) of this buffer */
    /* First OP_VERTS-group index of each stream in the chunk's groupPos
     * array; RunPass counts groups upward from here as it replays. */
    uint16_t opaGroupBase, semiGroupBase;
    uint16_t pad;
} WBuf;

typedef struct
{
    int            cellX, cellZ;
    int            inUse;
    int            bufCount;
    int            instCount;
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
    int            tileRefCount;
    int            triCount;   /* all passes, all buffers: the chunk's fill upper bound */
} WChunk;

static WChunk  s_chunks[MAX_WCHUNKS];
static uint8_t s_chunkArena[MAX_WCHUNKS][WCHUNK_ARENA_BYTES] __attribute__((aligned(16)));
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
#define CTILE_SLOTS      22
#define CHAR_ARENA_BYTES (40 * 1024)
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

static FILE*   s_sht;             /* area tile store, kept open */
static char    s_shtPrefix[8];
static uint16_t s_shtTileCount;
static uint16_t s_shtPalCount;
static uint16_t* s_pals;          /* all palettes resident, uncached */
static uint32_t* s_palOffsets;    /* file offset + word count per palette */
static uint16_t* s_palWords;

static T3DViewport s_wvp;
static int     s_wvpInited;
static int     s_frameActive;     /* between NotifyFrameBegin/End */
static int     s_worldStarted;    /* per-frame world state applied */

/* one-shot + census diagnostics */
static int     s_logOnce;
static int     s_cnBlocks, s_cnFallback;

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
static int   s_zActive;   /* Z-buffer live this frame (gpu_rdp.c) -> world writes depth */

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
        for (r = 0; r < 3; r++)
            for (cc = 0; cc < 3; cc++)
                m.m[cc][r] = (float)vR[r * 3 + cc] / 4096.0f;
        /* Verts are local/8, so the view translation converts the same way;
         * the whole pipe stays in the small /8 fixed-point-safe range. */
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

void ShT3d_WorldViewSet(const void* wsMatrix, int camX, int camY, int camZ,
                        int plX, int plY, int plZ,
                        int h, int ofx, int ofy)
{
    const int16_t* m = (const int16_t*)wsMatrix;
    int i;
    int firstOfFrame = 0;
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

static int TileAcquire(int sthIdx)
{
    uint8_t meta[16];
    int slot;
    WTile* t;

    slot = TileSlotFind(sthIdx);
    if (slot >= 0)
    {
        s_tiles[slot].refs++;
        return slot;
    }
    if (s_sht == NULL || sthIdx >= s_shtTileCount)
        return -1;

    for (slot = 0; slot < (int)(sizeof s_tiles / sizeof s_tiles[0]); slot++)
        if (s_tiles[slot].sthIdx < 0)
            break;
    if (slot >= (int)(sizeof s_tiles / sizeof s_tiles[0]))
    {
        SH_DBG("[T3DW] tile slots FULL acquiring %d", sthIdx);
        return -1;
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
    t->sthIdx = sthIdx;
    t->refs   = 1;
    s_tileRam += t->pixLen;
    return slot;
}

static void TileRelease(int sthIdx)
{
    int slot = TileSlotFind(sthIdx);
    if (slot < 0)
        return;
    if (--s_tiles[slot].refs <= 0)
    {
        s_tileRam -= s_tiles[slot].pixLen;
        s_tiles[slot].pix    = NULL;
        s_tiles[slot].sthIdx = -1;
    }
}

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

    /* Palettes stay resident: they are tiny and every block references them. */
    s_palOffsets = malloc(s_shtPalCount * 4);
    s_palWords   = malloc(s_shtPalCount * 2);
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
    return 1;
}

/* ------------------------------------------------------------- chunks */

static WChunk* ChunkFind(int cellX, int cellZ)
{
    int i;
    for (i = 0; i < MAX_WCHUNKS; i++)
        if (s_chunks[i].inUse && s_chunks[i].cellX == cellX && s_chunks[i].cellZ == cellZ)
            return &s_chunks[i];
    return NULL;
}

static void ChunkFree(WChunk* c)
{
    int i;
    if (!c->inUse)
        return;
    /* Everything lives in the slot's arena; only tile refs need releasing. */
    for (i = 0; i < c->tileRefCount; i++)
        TileRelease(c->tileRefs[i]);
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
    slot = TileSlotFind(tref - 1);
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
 * only foreground (nearer). Splitting the same stream twice around the PSX
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
    int i = 0, pushed = 0, needSync = 0, tileSkip = 0, depthSkip = wantFg;
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
                float vzMax = r0 * B[0] + r1 * B[1] + r2 * B[2] + curRow[3]
                            + (r0 < 0 ? -r0 : r0) * B[3]
                            + (r1 < 0 ? -r1 : r1) * B[4]
                            + (r2 < 0 ? -r2 : r2) * B[5];
                depthSkip = (((vzMax < s_playerViewZ - WORLD_FG_BIAS) ? 1 : 0) != wantFg);
            }
            else
                depthSkip = wantFg;
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
    bufCount  = rd16(hdr + 6);
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
        if (WFailLog())
            SH_DBG("[T3DW] %s: chunk slots full", base);
        fclose(f);
        return;
    }
    memset(c, 0, sizeof *c);
    c->cellX = cellX;
    c->cellZ = cellZ;

    if (!ShwLoadBody(c, s_chunkArena[(int)(c - s_chunks)], WCHUNK_ARENA_BYTES, f, base,
                     bufCount, instCount, refCount, instOff, refsOff, cellX, cellZ, 0))
    {
        fclose(f);
        c->inUse = 1;          /* let ChunkFree see a live chunk to unwind */
        c->bufCount = 0;
        ChunkFree(c);
        return;
    }
    /* First composition immediately (this frame's WorldViewSet has
     * already run); every later frame recomposes in WorldViewSet. */
    ComposeChunkViews(c);
    fclose(f);
    c->inUse = 1;
    SH_DBG("[T3DW] chunk %s resident: bufs=%d insts=%d groups=%d tris=%d tiles=%d/%d miss=%d tileRam=%dK",
           base, bufCount, instCount, c->groupCount, c->triCount, c->tileRefCount, refCount,
           s_cnTileMiss, s_tileRam / 1024);
}

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

        table = malloc(bufCount * 20);
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
                if (ow > 1) cmdBytes += (ow * 2 + 3) & ~3;
                if (sw > 1) cmdBytes += (sw * 2 + 3) & ~3;
            }
            need = ((vertBytes + 15) & ~15) + ((matBytes + 15) & ~15)
                 + ((rawRBytes + 15) & ~15) + ((rawTBytes + 15) & ~15)
                 + ((rowBytes + 15) & ~15)
                 + ((refBytes + 15) & ~15) + ((bufBytes + 15) & ~15)
                 + ((cmdBytes + 15) & ~15);
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
        tmp = malloc(instCount * 32 > 4096 ? (size_t)instCount * 32 : 4096);
        if (tmp == NULL)
            goto fail;
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

        /* Tile refs (arena-resident, carved above). */
        c->tileRefCount = 0;
        fseek(f, refsOff, SEEK_SET);
        if (fread(tmp, 1, refCount * 2, f) != (size_t)(refCount * 2))
            goto fail;
        for (i = 0; i < refCount; i++)
        {
            uint16_t idx = rd16(tmp + i * 2);
            if (!charMode && TileAcquire(idx) >= 0)   /* char tiles live in their own store */
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
                        /* Q8 world (D*256) -> world/8 t3d units (D*32): >>3.
                         * Matches the instance translation (tx/8) and the eye
                         * (camPos/8); keeps every coordinate small enough that
                         * t3d's fixed-point transform does not overflow. */
                        /* World verts are Q8 (D*256) -> /8 = D*32 to fit t3d's
                         * fixed-point range. CHARACTER verts are small bone-
                         * local values (~+-120 raw); /8 rounds their fine
                         * features to zero -> degenerate triangles / "missing
                         * pieces". Perspective projection is scale-invariant,
                         * so raw renders identically but keeps full precision.
                         * The bone translation stays raw to match (CharaBone). */
                        if (charMode)
                        {
                            pos[0] = (int16_t)rd16(v + 0);
                            pos[1] = (int16_t)rd16(v + 2);
                            pos[2] = (int16_t)rd16(v + 4);
                        }
                        else
                        {
                            pos[0] = (int16_t)((int16_t)rd16(v + 0) / 8);
                            pos[1] = (int16_t)((int16_t)rd16(v + 2) / 8);
                            pos[2] = (int16_t)((int16_t)rd16(v + 4) / 8);
                        }
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
                uint32_t opaOff  = rd32(bt + 0x0C);
                uint32_t semiOff = rd32(bt + 0x10);
                c->bufs[i].vbase = (uint16_t)vbase;
                if (opaWords > 1)
                {
                    fseek(f, opaOff, SEEK_SET);
                    if (fread(c->cmds + cmdCur, 1, opaWords * 2, f) != (size_t)(opaWords * 2))
                        goto fail;
                    c->bufs[i].opaOff   = cmdCur;
                    c->bufs[i].opaWords = (uint16_t)opaWords;
                    cmdCur += (opaWords * 2 + 3) & ~3;
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
                        c->groupPos = NULL;
                        if (gcount != 0)
                            SH_DBG("[T3DW] %s: %u group AABBs over the arena -- background-only",
                                   base, (unsigned)gcount);
                    }
                    else
                    {
                        int16_t* gpos = (int16_t*)gp;
                        uint32_t g    = 0;
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
                                        gpos[g * 6 + 0] = (int16_t)((mnx + mxx) / 2);
                                        gpos[g * 6 + 1] = (int16_t)((mny + mxy) / 2);
                                        gpos[g * 6 + 2] = (int16_t)((mnz + mxz) / 2);
                                        gpos[g * 6 + 3] = (int16_t)((mxx - mnx) / 2 + 1);
                                        gpos[g * 6 + 4] = (int16_t)((mxy - mny) / 2 + 1);
                                        gpos[g * 6 + 5] = (int16_t)((mxz - mnz) / 2 + 1);
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
        free(tmp);
        free(table);
        return 1;

fail:
        if (WFailLog())
            SH_DBG("[T3DW] %s: load failed (see prior line or alloc)", base);
        free(tmp);
        free(table);
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
}

void ShT3d_WorldReset(void)
{
    int i;
    rspq_wait();
    for (i = 0; i < MAX_WCHUNKS; i++)
        ChunkFree(&s_chunks[i]);
    for (i = 0; i < (int)(sizeof s_tiles / sizeof s_tiles[0]); i++)
        if (s_tiles[i].sthIdx >= 0)
        {
            s_tiles[i].pix = NULL;
            s_tiles[i].sthIdx = -1;
            s_tiles[i].refs = 0;
        }
    s_tileRam = 0;
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
    s_lastBoundOk = 0;   /* TMEM was clobbered since the last flush pass */
    {
        /* Stage 1: with a real Z-buffer attached (gpu_rdp.c), the world writes
         * true depth and self-occludes by Z instead of painter's tile order.
         * Without it, the old no-Z path (t3d_frame_start turns Z ON, and with
         * no buffer that scribbles RDRAM -- so force it OFF). */
        extern int GpuNv2a_ZBufActive(void);
        s_zActive = GpuNv2a_ZBufActive();
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
            float nearP = 4.0f, farP = 50000.0f;
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
        }
        t3d_viewport_look_at(&s_wvp, &e, &tg, &u);
        t3d_viewport_attach(&s_wvp);

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
 * FrameEnd. Returning 1 still tells the game to skip its PSX per-prim path. */
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
    return 1;
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
                    uint16_t tref, uint16_t pal)
{
    if (n >= ZREC_MAX) { s_cnZRecOverflow++; return n; }
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
    uint16_t tref = 0, pal = 0;
    while (i < words)
    {
        uint16_t w = rd16(p + i * 2), opc = w >> 12, arg = w & 0xFFF;
        if (opc == OP_TILE)
        {
            if (start >= 0)
                n = ZRecPush(n, c, b, p, start, i, tref, pal);
            start = i;
            tref  = rd16(p + (i + 1) * 2);
            pal   = arg;
            i += 2;
        }
        else if (opc == OP_MATRIX) i += 1;
        else if (opc == OP_VERTS)  i += 2;
        else if (opc == OP_TRIS)   i += 1 + (arg * 3 + 1) / 2;
        else break;                 /* OP_END (or a bad opcode) */
    }
    if (start >= 0)
        n = ZRecPush(n, c, b, p, start, i, tref, pal);
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
                        r->c->mats + s_matPhase * r->c->instCount, NULL,
                        r->c->viewRow, 0, 0, 0, BindWorldTile, first);
                first = 0;
                s_cnBlocks++;
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
                        c->mats + s_matPhase * c->instCount, NULL, c->viewRow,
                        0, 0, 1, BindWorldTile, first);
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
        SH_DBG("[T3DW] blocks=%d fallback=%d tileRam=%dK tileUp=%d dedup=%d",
               s_cnBlocks, s_cnFallback, s_tileRam / 1024, s_cnTileUp, s_cnTileDedup);
    s_cnBlocks = s_cnFallback = 0;
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
    bufCount  = rd16(hdr + 6);
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
    SH_DBG("[T3DC] %s resident: parts=%d groups=%d tiles=%d pals=%d",
           name, instCount, s_charChunk.groupCount, s_cTileCount, s_cPalCount);
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
    s_charDiagMode = 0;   /* always textured; config chara_debug forces flat */
    return 1;
}

void ShT3d_CharaDrawEnd(void)
{
    s_charActive = 0;
}

/* The bone loop hands over each part's GAME view matrix (Vw_CoordToWorldAnd
 * ViewMatrices of the bone coord: Q12 rotation m9 row-major, Q8 translation
 * t3) -- exactly what ComposeChunkViews derives for a world instance, so the
 * part lands where the PSX path would land it. */
int ShT3d_CharaBone(int partIdx, const short* m9, const int* t3)
{
    WChunk*    c = &s_charChunk;
    T3DMat4    m;
    T3DMat4FP* dst;
    int        r, cc;

    if (!s_charActive || partIdx < 0 || partIdx >= c->instCount)
        return 0;
    memset(&m, 0, sizeof m);
    {
        extern int GpuNv2a_CharaXpose(void);
        int xpose = !GpuNv2a_CharaXpose();   /* default: transpose (matches the world) */
        for (r = 0; r < 3; r++)
            for (cc = 0; cc < 3; cc++)
                if (xpose) m.m[cc][r] = (float)m9[r * 3 + cc] / 4096.0f;
                else       m.m[r][cc] = (float)m9[r * 3 + cc] / 4096.0f;
    }
    /* RAW translation (NOT /8): the character verts are loaded raw (see
     * ShwLoadBody charMode) so the R*vert + T sum must be in the same raw
     * scale. Perspective is scale-invariant, so this projects the same as the
     * world's /8 but keeps the character's fine geometry from collapsing. */
    m.m[3][0] = (float)t3[0];
    m.m[3][1] = (float)t3[1];
    m.m[3][2] = (float)t3[2];
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
    c->viewRow[partIdx * 4 + 3] = (float)t3[2];   /* raw, matches m.m[3][2]; used only for the part depth sort */
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
        /* Cull backfaces for the character (WorldFrameStart left culling OFF
         * for the flat, single-sided world). A closed character mesh drawn
         * with no Z buffer paints its far faces over its near ones without
         * this. Config chara_cull flips the winding if the model vanishes. */
        {
            extern int GpuNv2a_CharaCull(void);
            /* Backface cull (config chara_cull: 0 none, 1 back, 2 front). With
             * the pool-resolved geometry each closed part is single-sided-correct
             * under CULL_BACK; set chara_cull=2 if a build ever renders inside-out. */
            int cull = GpuNv2a_CharaCull();
            int df = T3D_FLAG_SHADED | T3D_FLAG_TEXTURED;
            if (cull == 1)      df |= T3D_FLAG_CULL_BACK;
            else if (cull == 2) df |= T3D_FLAG_CULL_FRONT;
            if (s_zActive)      df |= T3D_FLAG_DEPTH;   /* Z-write: occludes / is occluded */
            t3d_state_set_drawflags(df);
        }
        for (oi = 0; oi < nb; oi++)
        {
            const T3DVertPacked* bverts;
            b = order[oi];
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
        SH_DBG("[T3DC] native chara: parts=%d/%d groups=%d", drawn, c->instCount, c->groupCount);
    s_charMask = 0;
}
