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
    T3DMat4FP*     mats;       /* -> own arena slot, after verts */
    float*         groupPos;   /* per-OP_VERTS-group world/8 centroids, after
                                * the cmd streams in the arena; NULL = none
                                * fitted (every group classifies background) */
    int            groupCount;
    uint16_t*      tileRefs;   /* -> own arena slot, after mats */
    uint8_t*       cmds;       /* -> own arena slot: all passes' streams */
    int            tileRefCount;
} WChunk;

static WChunk  s_chunks[MAX_WCHUNKS];
static uint8_t s_chunkArena[MAX_WCHUNKS][WCHUNK_ARENA_BYTES] __attribute__((aligned(16)));
static WTile   s_tiles[TILE_SLOTS];
static uint8_t s_tilePool[TILE_SLOTS][TILE_SLOT_BYTES] __attribute__((aligned(16)));
static int     s_tileRam;

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

/* Camera basis in world/8 units + the player's view-space depth, recomputed
 * per flush in WorldFrameStart and read by RunPass to classify each instance
 * as foreground (nearer than the player) or background. */
static float s_eyeF[3], s_fwdF[3], s_playerViewZ;

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

void ShT3d_WorldViewSet(const void* wsMatrix, int camX, int camY, int camZ,
                        int plX, int plY, int plZ,
                        int h, int ofx, int ofy)
{
    const int16_t* m = (const int16_t*)wsMatrix;
    int i;
    /* First camera handoff after a flush = start of a new frame's world;
     * clear the draw list here rather than on FrameBegin (which the game's
     * mid-frame VSync fires between the OT build and the GsDrawOt flush). */
    if (s_flushed)
    {
        s_drawCount = 0;
        s_flushed = 0;
    }
    for (i = 0; i < 9; i++)
        s_wm[i] = m[i];
    s_camPos[0] = camX; s_camPos[1] = camY; s_camPos[2] = camZ;
    s_playerPos[0] = plX; s_playerPos[1] = plY; s_playerPos[2] = plZ;
    s_geomH   = h;
    s_geomOfx = ofx;
    s_geomOfy = ofy;
    s_haveView = 1;
}

/* ------------------------------------------------------------- helpers */

static FILE* WOpen(const char* rel)
{
    char path[96];
    FILE* f;
    snprintf(path, sizeof path, "sd:/silenthill/gamedata/load/N64W/%s", rel);
    f = fopen(path, "rb");
    if (f == NULL)
    {
        snprintf(path, sizeof path, "rom:/N64W/%s", rel);
        f = fopen(path, "rb");
    }
    return f;
}

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
/* A group (one OP_VERTS load) is foreground only when its centroid is nearer
 * the camera than the player by more than this margin (world/8: 16 = half a
 * world unit). Without it the floor patch at the player's own feet sits right
 * on the split plane and flickers over him frame to frame. */
#define WORLD_FG_BIAS 16.0f

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
    if (tref == 0)
    {
        /* untextured group: shade-only, or it would sample whatever tile the
         * previous group left in TMEM */
        rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
        return 1;
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
        /* rdpq_sync_tile before overwriting TMEM: t3d_tri_draw is async on
         * the RSP/RDP, so without the sync the load races the previous
         * group's rasterisation (t3dmodel.c does this before every upload). */
        rdpq_sync_tile();
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
        rdpq_tex_upload_tlut(s_pals + s_palOffsets[palArg], 0,
                             s_palWords[palArg]);
        rdpq_tex_upload(TILE0, &surf, NULL);
    }
    return 1;
}

/* wantFg: 0 = draw only background groups (farther than the player), 1 =
 * only foreground (nearer). Splitting the same stream twice around the PSX
 * character OT reproduces the OT's back-to-front order with no Z buffer.
 * groupBase = this stream's first index into groupPos (see WBuf); groupPos
 * NULL = no centroid data, everything is background. */
static void RunPass(const uint8_t* p, int cmdWords,
                    const T3DVertPacked* verts, const T3DMat4FP* mats,
                    const float* groupPos, int groupBase, int wantFg,
                    int semi)
{
    /* depthSkip defaults to wantFg so any geometry before the first OP_VERTS
     * classification draws in the background pass only -- never in both,
     * which would double-draw it. */
    int i = 0, pushed = 0, needSync = 0, tileSkip = 0, depthSkip = wantFg;
    int gIdx = groupBase;
    /* Lazy tile state: OP_TILE only records; BindWorldTile runs at the first
     * OP_TRIS that actually draws under it. A pending tile overwritten by the
     * next OP_TILE was for a fully-skipped run of groups. */
    int tilePending = 0;
    uint16_t pendTref = 0, pendPal = 0;

    /* Pass-wide state the OTHER pass may have changed. */
    if (semi)
        rdpq_mode_blender(RDPQ_BLENDER_ADDITIVE);
    else
        rdpq_mode_blender(0);

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
        }
        else if (opc == OP_VERTS)
        {
            uint16_t first = rd16(p + i * 2);
            int g = gIdx++;
            i++;
            /* Classify this <=32-vert patch: view-space depth = forward .
             * (centroid - eye), world/8. Clearly nearer than the player =
             * foreground (drawn after the character OT), else background. */
            if (groupPos != NULL)
            {
                const float* P = groupPos + g * 3;
                float vz = s_fwdF[0] * (P[0] - s_eyeF[0])
                         + s_fwdF[1] * (P[1] - s_eyeF[1])
                         + s_fwdF[2] * (P[2] - s_eyeF[2]);
                depthSkip = (((vz < s_playerViewZ - WORLD_FG_BIAS) ? 1 : 0) != wantFg);
            }
            else
                depthSkip = wantFg;
            /* tileSkip is NOT checked here: with lazy binding a missing tile
             * isn't known until the bind at OP_TRIS; the wasted vert load on
             * that rare path (miss=0 in every log) is cheaper than binding
             * tiles for groups that never draw. */
            if (depthSkip)
                continue;
            if (needSync) { t3d_tri_sync(); needSync = 0; }
            t3d_vert_load(verts + first / 2, 0, arg);
        }
        else if (opc == OP_TRIS)
        {
            int n = arg, k;
            if (!depthSkip)
            {
                if (tilePending)
                {
                    if (needSync) { t3d_tri_sync(); needSync = 0; }
                    tileSkip = !BindWorldTile(pendTref, pendPal);
                    tilePending = 0;
                }
                if (!tileSkip)
                {
                    for (k = 0; k < n; k++)
                    {
                        /* packed u8 triples across u16 words */
                        int base = i * 2 + k * 3;
                        t3d_tri_draw(p[base], p[base + 1], p[base + 2]);
                    }
                    needSync = 1;
                }
            }
            i += (n * 3 + 1) / 2;
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
            int vertBytes = (int)sizeof(T3DVertPacked) * (total / 2 + 1);
            int matBytes  = (int)sizeof(T3DMat4FP) * (instCount ? instCount : 1);
            int refBytes  = refCount ? refCount * 2 : 2;
            int bufBytes  = (int)sizeof(WBuf) * bufCount;
            int cmdBytes  = 0;
            int need;
            uint8_t* a = s_chunkArena[(int)(c - s_chunks)];
            for (i = 0; i < bufCount; i++)
            {
                int ow = rd16(table + i * 20 + 2);
                int sw = rd16(table + i * 20 + 4);
                if (ow > 1) cmdBytes += (ow * 2 + 3) & ~3;
                if (sw > 1) cmdBytes += (sw * 2 + 3) & ~3;
            }
            need = ((vertBytes + 15) & ~15) + ((matBytes + 15) & ~15)
                 + ((refBytes + 15) & ~15) + ((bufBytes + 15) & ~15)
                 + ((cmdBytes + 15) & ~15);
            if (need > WCHUNK_ARENA_BYTES)
            {
                if (WFailLog())
                    SH_DBG("[T3DW] %s: %dKB over the %dKB arena -- PSX fallback",
                           base, need / 1024, WCHUNK_ARENA_BYTES / 1024);
                goto fail;
            }
            c->verts = (T3DVertPacked*)a;
            a += (vertBytes + 15) & ~15;
            c->mats = (T3DMat4FP*)a;
            a += (matBytes + 15) & ~15;
            c->tileRefs = (uint16_t*)a;
            a += (refBytes + 15) & ~15;
            c->bufs = (WBuf*)a;
            a += (bufBytes + 15) & ~15;
            c->cmds = a;
            c->instCount = instCount;
            memset(c->bufs, 0, bufBytes);
        }

        /* Instances -> fixed-point matrices. rot Q12/4096 with the 1/8 world
         * scale folded in; translation Q8/8 with the cell corner added. */
        tmp = malloc(instCount * 32 > 4096 ? (size_t)instCount * 32 : 4096);
        if (tmp == NULL)
            goto fail;
        fseek(f, instOff, SEEK_SET);
        if (fread(tmp, 1, instCount * 32, f) != (size_t)(instCount * 32))
            goto fail;
        for (i = 0; i < instCount; i++)
        {
            const uint8_t* ip = tmp + i * 32;
            T3DMat4 m;
            int r, cc;
            int32_t tx = (int32_t)rd32(ip + 20) + cellX * 10240;
            int32_t ty = (int32_t)rd32(ip + 24);
            int32_t tz = (int32_t)rd32(ip + 28) + cellZ * 10240;
            /* TRUE rotation: instance rot is Q12 (4096 = 1.0). Vertices are
             * scaled to world/8 at load (see below) so they share the matrix
             * translation's scale -- both in world/8 t3d units, small enough
             * for t3d's fixed-point pipeline. (The old /32768 folded a 1/8
             * scale in to compensate for raw Q8 vertices, but that left the
             * vertices themselves world*256 -- 8x past t3d's usable range, so
             * the RSP overflowed them off-screen.) */
            memset(&m, 0, sizeof m);
            for (r = 0; r < 3; r++)
                for (cc = 0; cc < 3; cc++)
                    m.m[cc][r] = (float)(int16_t)rd16(ip + (r * 3 + cc) * 2) / 4096.0f;
            m.m[3][0] = (float)tx / 8.0f;
            m.m[3][1] = (float)ty / 8.0f;
            m.m[3][2] = (float)tz / 8.0f;
            m.m[3][3] = 1.0f;
            t3d_mat4_to_fixed(&c->mats[i], &m);
        }
        data_cache_hit_writeback(c->mats,
            sizeof(T3DMat4FP) * (instCount ? instCount : 1));

        /* Tile refs (arena-resident, carved above). */
        c->tileRefCount = 0;
        fseek(f, refsOff, SEEK_SET);
        if (fread(tmp, 1, refCount * 2, f) != (size_t)(refCount * 2))
            goto fail;
        for (i = 0; i < refCount; i++)
        {
            uint16_t idx = rd16(tmp + i * 2);
            if (TileAcquire(idx) >= 0)
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
                        pos[0] = (int16_t)((int16_t)rd16(v + 0) / 8);
                        pos[1] = (int16_t)((int16_t)rd16(v + 2) / 8);
                        pos[2] = (int16_t)((int16_t)rd16(v + 4) / 8);
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
                uint8_t* arena  = s_chunkArena[(int)(c - s_chunks)];
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
                            else if (opc == OP_TRIS)  k += (arg * 3 + 1) / 2;
                            else if (opc == OP_END)   break;
                            /* OP_MATRIX: 1 word, nothing to skip */
                        }
                    }

                {
                    uint32_t gbytes = gcount * 3 * (uint32_t)sizeof(float);
                    uint8_t* gp     = c->cmds + ((cmdCur + 15u) & ~15u);
                    if (gcount == 0 || gp + gbytes > arena + WCHUNK_ARENA_BYTES)
                    {
                        c->groupPos = NULL;
                        if (gcount != 0)
                            SH_DBG("[T3DW] %s: %u group centroids over the arena -- background-only",
                                   base, (unsigned)gcount);
                    }
                    else
                    {
                        float*   gpos = (float*)gp;
                        uint32_t g    = 0;
                        c->groupPos   = gpos;
                        c->groupCount = (int)gcount;
                        for (bi = 0; bi < bufCount; bi++)
                            for (pass = 0; pass < 2; pass++)
                            {
                                int words = pass ? c->bufs[bi].semiWords : c->bufs[bi].opaWords;
                                const uint8_t* p = c->cmds + (pass ? c->bufs[bi].semiOff : c->bufs[bi].opaOff);
                                int bvbase  = c->bufs[bi].vbase;
                                int curInst = -1;
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
                                    else if (opc == OP_MATRIX)
                                        curInst = (int)arg;
                                    else if (opc == OP_VERTS)
                                    {
                                        int     first = rd16(p + k * 2);
                                        int     n     = arg;
                                        int32_t sx = 0, sy = 0, sz = 0;
                                        float   lx, ly, lz, wx, wy, wz;
                                        int     v;
                                        k++;
                                        for (v = 0; v < n; v++)
                                        {
                                            int vi = bvbase + first + v;
                                            const int16_t* q = (vi & 1) ? c->verts[vi / 2].posB
                                                                        : c->verts[vi / 2].posA;
                                            sx += q[0]; sy += q[1]; sz += q[2];
                                        }
                                        if (n > 0) { lx = (float)sx / n; ly = (float)sy / n; lz = (float)sz / n; }
                                        else       { lx = ly = lz = 0.0f; }
                                        if (curInst >= 0 && curInst < instCount)
                                        {
                                            /* T3DMat4FP is the float matrix verbatim in
                                             * 16.16 (m[col].i/f[row]); world[r] =
                                             * sum_c M(c,r)*local[c] + M(3,r), the same
                                             * multiply the RSP applies to the verts. */
                                            const T3DMat4FP* fm = &c->mats[curInst];
#define MFP(cc, r) ((float)fm->m[cc].i[r] + (float)fm->m[cc].f[r] * (1.0f / 65536.0f))
                                            wx = MFP(0,0)*lx + MFP(1,0)*ly + MFP(2,0)*lz + MFP(3,0);
                                            wy = MFP(0,1)*lx + MFP(1,1)*ly + MFP(2,1)*lz + MFP(3,1);
                                            wz = MFP(0,2)*lx + MFP(1,2)*ly + MFP(2,2)*lz + MFP(3,2);
#undef MFP
                                        }
                                        else { wx = lx; wy = ly; wz = lz; }
                                        gpos[g * 3 + 0] = wx;
                                        gpos[g * 3 + 1] = wy;
                                        gpos[g * 3 + 2] = wz;
                                        g++;
                                    }
                                    else if (opc == OP_TRIS)
                                        k += (arg * 3 + 1) / 2;
                                    else if (opc == OP_END)
                                        break;
                                }
                            }
                    }
                }
            }
        }

        free(tmp);
        free(table);
        fclose(f);
        c->bufCount = bufCount;
        c->inUse = 1;
        SH_DBG("[T3DW] chunk %s resident: bufs=%d insts=%d groups=%d tiles=%d/%d miss=%d tileRam=%dK",
               base, bufCount, instCount, c->groupCount, c->tileRefCount, refCount,
               s_cnTileMiss, s_tileRam / 1024);
        return;

fail:
        if (WFailLog())
            SH_DBG("[T3DW] %s: load failed (see prior line or alloc)", base);
        free(tmp);
        free(table);
        fclose(f);
        c->inUse = 1;          /* let ChunkFree see a live chunk to unwind */
        c->bufCount = 0;
        ChunkFree(c);
        return;
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
    rdpq_mode_zbuf(false, false);            /* no Z buffer attached yet */

    for (i = 0; i < 9; i++)
        R[i / 3][i % 3] = (float)s_wm[i] / 4096.0f;
    /* Eye = camera world position (Q8) / 8 = world/8 t3d units (matches the
     * instance translation tx/8 and the >>3 vertices). */
    for (i = 0; i < 3; i++)
        eye[i] = (float)s_camPos[i] / 8.0f;
    /* forward = R^T*+Z (PSX looks down +Z); up = -R^T*+Y (PSX +Y is down). */
    for (i = 0; i < 3; i++)
    {
        fwd[i] =  R[2][i];
        up[i]  = -R[1][i];
    }

    /* Cache the basis + the player's view-space depth for RunPass's per-
     * instance foreground/background split (world/8 units throughout). */
    for (i = 0; i < 3; i++)
    {
        s_eyeF[i] = eye[i];
        s_fwdF[i] = fwd[i];
    }
    if (s_playerPos[0] == 0 && s_playerPos[1] == 0 && s_playerPos[2] == 0)
    {
        /* No valid player yet (pre-spawn / transition frame). A sentinel below
         * every instance depth keeps the split degenerate-safe: nothing is
         * foreground, so the whole world draws in the background pass -- the
         * original behaviour, never world-over-characters. */
        s_playerViewZ = -1.0e30f;
    }
    else
    {
        s_playerViewZ =
              fwd[0] * ((float)s_playerPos[0] / 8.0f - eye[0])
            + fwd[1] * ((float)s_playerPos[1] / 8.0f - eye[1])
            + fwd[2] * ((float)s_playerPos[2] / 8.0f - eye[2]);
    }

    {
        static const uint8_t ambWhite[4] = {255, 255, 255, 255};
        static const uint8_t dirWhite[4] = {255, 255, 255, 255};
        float h = (float)s_geomH;
        T3DVec3 e   = {{ eye[0], eye[1], eye[2] }};
        T3DVec3 tg  = {{ eye[0] + fwd[0]*256.0f, eye[1] + fwd[1]*256.0f, eye[2] + fwd[2]*256.0f }};
        T3DVec3 u   = {{ up[0], up[1], up[2] }};
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
#if SH_T3DW_FLAT
        t3d_state_set_drawflags(T3D_FLAG_SHADED);
#else
        t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_TEXTURED);
#endif

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
static void WorldFlushPass(int wantFg)
{
    int i;

    if (!wantFg)
    {
        static int s_fl;
        if ((s_fl++ & 127) == 0)
            SH_DBG("[T3DWF] flush call: drawCount=%d ready=%d haveView=%d",
                   s_drawCount, ShT3d_Ready(), s_haveView);
    }

    if (s_drawCount == 0 || !ShT3d_Ready() || !s_haveView)
    {
        if (wantFg)
            s_flushed = 1;
        return;
    }

    WorldFrameStart();

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
                    c->mats, c->groupPos, c->bufs[b].opaGroupBase, wantFg, 0);
            s_cnBlocks++;
        }
        if (c->bufs[b].semiWords > 1)
        {
            RunPass(c->cmds + c->bufs[b].semiOff, c->bufs[b].semiWords, bverts,
                    c->mats, c->groupPos, c->bufs[b].semiGroupBase, wantFg, 1);
            s_cnBlocks++;
        }
    }
    if (wantFg)
        s_flushed = 1;

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
        SH_DBG("[T3DW] blocks=%d fallback=%d tileRam=%dK",
               s_cnBlocks, s_cnFallback, s_tileRam / 1024);
    s_cnBlocks = s_cnFallback = 0;
}
