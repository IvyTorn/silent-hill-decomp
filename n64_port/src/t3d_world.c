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

/* Four cells around the player go native, the rest stay on the PSX fallback;
 * grows once the strip-IPD funding lands. */
#define MAX_WCHUNKS   3

/* Chunk data (verts + matrices + tile refs) lives in a FIXED per-slot arena,
 * not the heap: at map time the game runs the heap down to double digits and
 * fragments the rest, which first starved these allocations and then crashed
 * libdragon mid-block-recording (rspq_next_buffer memsets an unchecked
 * malloc). The arena caps what a cell may need; bigger cells stay PSX. */
#define WCHUNK_ARENA_BYTES (56 * 1024)

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
    uint16_t pad;
} WBuf;

typedef struct
{
    int            cellX, cellZ;
    int            inUse;
    int            bufCount;
    WBuf*          bufs;       /* -> own arena slot */
    T3DVertPacked* verts;      /* -> own arena slot, cache-written-back */
    T3DMat4FP*     mats;       /* -> own arena slot, after verts */
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

/* Camera handed over by the game side each frame (Ipd_ChunkDraw). */
static const void* s_wsMatrix;    /* MATRIX*: s16 m[3][3] Q12 + s32 t[3] Q8 */
static int s_geomH, s_geomOfx, s_geomOfy;

void ShT3d_WorldViewSet(const void* wsMatrix, int h, int ofx, int ofy)
{
    s_wsMatrix = wsMatrix;
    s_geomH    = h;
    s_geomOfx  = ofx;
    s_geomOfy  = ofy;
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
static void RunPass(const uint8_t* p, int cmdWords,
                    const T3DVertPacked* verts, const T3DMat4FP* mats,
                    int semi)
{
    int i = 0, pushed = 0, needSync = 0, skip = 0;

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
            if (needSync) { t3d_tri_sync(); needSync = 0; }
            skip = 0;
            if (tref == 0)
            {
                /* untextured group: shade-only, or it would sample whatever
                 * tile the previous group left in TMEM */
                rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
            }
            else
            {
                int slot = TileSlotFind(tref - 1);
                if (slot >= 0)
                {
                    WTile* t = &s_tiles[slot];
                    surface_t surf = surface_make_linear(t->pix,
                        t->fmt == 0 ? FMT_CI4 : FMT_CI8, t->w, t->h);
                    rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
                    rdpq_tex_upload_tlut(s_pals + s_palOffsets[arg], 0,
                                         s_palWords[arg]);
                    rdpq_tex_upload(TILE0, &surf, NULL);
                }
                else
                {
                    /* Missing tile: DROP the group's geometry. Drawing it
                     * with stale TMEM painted other rooms' art (a wanted
                     * poster) onto this one's furniture. A hole is honest
                     * and the census counts it. */
                    skip = 1;
                    s_cnTileMiss++;
                }
            }
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
            i++;
            if (skip)
                continue;
            if (needSync) { t3d_tri_sync(); needSync = 0; }
            t3d_vert_load(verts + first / 2, 0, arg);
        }
        else if (opc == OP_TRIS)
        {
            int n = arg, k;
            if (!skip)
            {
                for (k = 0; k < n; k++)
                {
                    /* packed u8 triples across u16 words */
                    int base = i * 2 + k * 3;
                    t3d_tri_draw(p[base], p[base + 1], p[base + 2]);
                }
                needSync = 1;
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
            memset(&m, 0, sizeof m);
            for (r = 0; r < 3; r++)
                for (cc = 0; cc < 3; cc++)
                    m.m[cc][r] = (float)(int16_t)rd16(ip + (r * 3 + cc) * 2) / 32768.0f;
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
        }

        free(tmp);
        free(table);
        fclose(f);
        c->bufCount = bufCount;
        c->inUse = 1;
        SH_DBG("[T3DW] chunk %s resident: bufs=%d insts=%d tiles=%d/%d miss=%d tileRam=%dK",
               base, bufCount, instCount, c->tileRefCount, refCount,
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
    const int16_t* wm;
    const int32_t* wt;
    T3DMat4 view, proj;
    float ofsX, ofsY, sclX, sclY;
    int r, cc;

    extern void GpuXbox_GetViewTransform(float* ofsX, float* ofsY,
                                         float* sclX, float* sclY, int* contentX);
    int contentX;

    t3d_frame_start();
    /* No Z buffer attached: Z modes would make the RDP scribble RDRAM. */
    rdpq_mode_zbuf(false, false);
    rdpq_mode_filter(FILTER_POINT);          /* PSX look, and TMEM tiles have
                                                no border texels for bilinear */
    rdpq_mode_alphacompare(1);
    rdpq_mode_tlut(TLUT_RGBA16);             /* every tile is CI4/CI8 */
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
    t3d_light_set_count(0);
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_TEXTURED);

    /* View: GsWSMATRIX rows are Q12, t is Q8. Column-major out. */
    wm = (const int16_t*)s_wsMatrix;
    wt = (const int32_t*)((const uint8_t*)s_wsMatrix + 20); /* MATRIX: 9*s16 + pad + 3*s32 */
    for (cc = 0; cc < 4; cc++)
        for (r = 0; r < 4; r++)
            view.m[cc][r] = 0.0f;
    for (r = 0; r < 3; r++)
        for (cc = 0; cc < 3; cc++)
            view.m[cc][r] = (float)wm[r * 3 + cc] / 4096.0f;
    view.m[3][0] = (float)wt[0] / 8.0f;
    view.m[3][1] = (float)wt[1] / 8.0f;
    view.m[3][2] = (float)wt[2] / 8.0f;
    view.m[3][3] = 1.0f;

    /* Projection: PSX +Z-forward with the draw-env transform folded in.
     * px = ((ofx + h*xv/zv) + OFS_X)*SCL_X + CX  ->  ndcX = (px-160)/160
     * py = ((ofy + h*yv/zv) + OFS_Y)*SCL_Y       ->  ndcY = (120-py)/120 */
    GpuXbox_GetViewTransform(&ofsX, &ofsY, &sclX, &sclY, &contentX);
    {
        float h = (float)s_geomH;
        float cx = (((float)s_geomOfx + ofsX) * sclX + (float)contentX - 160.0f) / 160.0f;
        float cy = (120.0f - ((float)s_geomOfy + ofsY) * sclY) / 120.0f;
        float zn = 2.0f, zf = 1600.0f;   /* t3d units (Q8/8) */
        memset(&proj, 0, sizeof proj);
        proj.m[0][0] = h * sclX / 160.0f;
        proj.m[1][1] = -h * sclY / 120.0f;
        proj.m[2][0] = cx;
        proj.m[2][1] = cy;
        proj.m[2][2] = (zf + zn) / (zf - zn);
        proj.m[3][2] = -2.0f * zf * zn / (zf - zn);
        proj.m[2][3] = 1.0f;
    }

    if (!s_wvpInited)
    {
        s_wvp = t3d_viewport_create();
        s_wvpInited = 1;
    }
    t3d_viewport_set_projection_matrix(&s_wvp, &proj);
    t3d_viewport_set_view_matrix(&s_wvp, &view);
    t3d_viewport_attach(&s_wvp);

    s_worldStarted = 1;
}

int ShT3d_WorldDrawBuffer(int cellX, int cellZ, int bufIdx)
{
    WChunk* c;

    if (!s_frameActive || !ShT3d_Ready() || s_wsMatrix == NULL)
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

    if (!s_worldStarted)
        WorldFrameStart();

    {
        const T3DVertPacked* bverts = c->verts + c->bufs[bufIdx].vbase / 2;
        if (c->bufs[bufIdx].opaWords > 1)
        {
            RunPass(c->cmds + c->bufs[bufIdx].opaOff, c->bufs[bufIdx].opaWords,
                    bverts, c->mats, 0);
            s_cnBlocks++;
        }
        if (c->bufs[bufIdx].semiWords > 1)
        {
            RunPass(c->cmds + c->bufs[bufIdx].semiOff, c->bufs[bufIdx].semiWords,
                    bverts, c->mats, 1);
            s_cnBlocks++;
        }
    }
    return 1;
}

void ShT3d_NotifyFrameBegin(void)
{
    s_frameActive  = 1;
    s_worldStarted = 0;
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
