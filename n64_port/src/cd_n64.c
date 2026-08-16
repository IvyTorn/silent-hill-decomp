/*
 * cd_n64.c - PSX CD-ROM (libcd) on N64, backed by the game's BIN disc image.
 *
 * Implements the synchronous slice of libcd the FS queue uses: CdControl
 * (CdlSetloc) sets the read sector, CdRead copies the 2048-byte user data out of
 * each 2352-byte Mode-2/Form-1 sector (data at offset 24: 12 sync + 3 addr +
 * 1 mode + 8 subheader). The PSX async model collapses to synchronous reads
 * here, so CdReadSync/CdSync report "complete" immediately.
 *
 * TWO SOURCES, tried in order:
 *
 *   sd:/   the flashcart's SD card (SummerCart64, ED64). This is the real one.
 *          A 616 MB disc image is nothing to an SD card, and libdragon's FAT
 *          layer gives it a normal fopen/fseek/fread, which is all the FS queue
 *          has ever wanted -- the PSX CD was a seekable block device with a
 *          directory and so is this.
 *
 *   rom:/  a DragonFS image built into the cartridge. Bounded by the 64 MB
 *          cartridge address window, so it cannot hold the full disc, but it is
 *          the only source an emulator can see: ares has no flashcart SD. A
 *          trimmed image (no XA, no FMV) is what this is for.
 *
 * Self-contained (local CdlLOC + Cdl* constants) rather than including
 * <libcd.h>: this TU is HAL and includes libdragon headers, whose kernel.h
 * collides with psyq's. C links by name and CdlLOC is the standard
 * {minute,second,sector,track} BCD layout, so this matches the game's libcd
 * prototypes at the ABI level.
 */
#include <libdragon.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_log.h"

#define CDL_SETLOC      0x02
#define CDL_COMPLETE    0x02
#define BIN_SECTOR_SIZE 2352
#define BIN_DATA_OFFSET 24
#define BIN_DATA_SIZE   2048

typedef unsigned char u8_;
typedef struct { u8_ minute, second, sector, track; } CdlLOC;

/* Where the BIN actually opened (for the boot banner / log). */
char g_CdBinPath[256] = "NOT FOUND";

static FILE* s_bin;
static int   s_curSector;
static int   s_inited;
static int   s_warned;

/* ---------------------------------------------------------------- pack */

/* A .shpak is the same disc addressed sparsely: only the sectors the selected
 * directories occupy, cooked to 2048 bytes, with an index that preserves the
 * original LBAs so the game's file table still resolves. See
 * n64_port/tools/mkdiscpack.py. The whole disc minus XA is 78 MB cooked, which
 * fits an SD card but not the 64 MB cartridge window, and an emulator has no SD
 * at all -- this is what makes both testable.
 *
 * Header and index are big-endian, so on this machine they need no swapping. */
#define PACK_MAGIC 0x5348504Bu   /* "SHPK" */

typedef struct
{
    unsigned int startLba;
    unsigned int count;
    unsigned int sectorIndex;
} PackRun;

static PackRun*     s_packRuns;
static unsigned int s_packRunCount;
static unsigned int s_packDataOffset;

/* Binary search: the runs are sorted and disjoint by construction. Returns the
 * pack-relative sector index for an LBA, or -1 when the pack does not carry it
 * (a directory that was left out). */
static long Pack_LbaToIndex(unsigned int lba)
{
    unsigned int lo = 0;
    unsigned int hi = s_packRunCount;

    while (lo < hi)
    {
        unsigned int mid = lo + (hi - lo) / 2;
        const PackRun* r = &s_packRuns[mid];

        if (lba < r->startLba)
            hi = mid;
        else if (lba >= r->startLba + r->count)
            lo = mid + 1;
        else
            return (long)(r->sectorIndex + (lba - r->startLba));
    }
    return -1;
}

/* Returns 1 if the open file is a pack (and loads its index), 0 if it looks
 * like a plain BIN. */
static int Pack_TryLoad(FILE* f)
{
    unsigned int hdr[6];
    size_t       bytes;

    if (fseek(f, 0, SEEK_SET) != 0)
        return 0;
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
        return 0;
    if (hdr[0] != PACK_MAGIC)
        return 0;

    s_packRunCount   = hdr[2];
    s_packDataOffset = hdr[4];

    if (s_packRunCount == 0 || s_packRunCount > 65536)
    {
        SH_DBG("[CD] pack run count %u is implausible - ignoring", s_packRunCount);
        return 0;
    }

    bytes = (size_t)s_packRunCount * sizeof(PackRun);
    s_packRuns = (PackRun*)malloc(bytes);
    if (s_packRuns == NULL)
    {
        SH_DBG("[CD] pack index needs %u bytes - out of heap", (unsigned)bytes);
        return 0;
    }

    if (fread(s_packRuns, 1, bytes, f) != bytes)
    {
        SH_DBG("[CD] pack index truncated");
        free(s_packRuns);
        s_packRuns = NULL;
        return 0;
    }

    SH_DBG("[CD] pack: %u runs, data at %u", s_packRunCount, s_packDataOffset);
    return 1;
}

static int DecodeBcd(u8_ b)  { return (b >> 4) * 10 + (b & 0x0F); }
static u8_ EncodeBcd(int i)  { return (u8_)(((i / 10) << 4) | (i % 10)); }

CdlLOC* CdIntToPos(int i, CdlLOC* p)
{
    if (p == NULL)
        return NULL;

    i += 150;                              /* LBA -> absolute MSF (2-second lead-in) */
    p->sector = EncodeBcd(i % 75);
    p->second = EncodeBcd((i / 75) % 60);
    p->minute = EncodeBcd((i / 75) / 60);
    p->track  = 0;
    return p;
}

static int CdPosToInt(const CdlLOC* p)
{
    return 75 * (60 * DecodeBcd(p->minute) + DecodeBcd(p->second)) + DecodeBcd(p->sector) - 150;
}

/* ------------------------------------------------------------- mounting */

static int TryOpen(const char* path)
{
    s_bin = fopen(path, "rb");
    if (!s_bin)
        return 0;

    strncpy(g_CdBinPath, path, sizeof(g_CdBinPath) - 1);
    g_CdBinPath[sizeof(g_CdBinPath) - 1] = '\0';
    SH_DBG("[CD] disc image: %s", path);

    /* Decided by CONTENT, not by extension: a pack renamed disc.bin still has
     * to work, and a BIN that happens to be named .shpak must not be read
     * through an index it does not have. */
    Pack_TryLoad(s_bin);
    return 1;
}

void Cd_N64Init(void)
{
    /* Both prefixes, in preference order. sd: first so a card present in the
     * flashcart always wins over whatever was baked into the ROM -- otherwise
     * a trimmed test image would silently shadow the real disc. */
    static const char* const names[] = {
        "sd:/silenthill/Silent Hill (USA).bin",
        "sd:/silenthill/disc.bin",
        "sd:/silenthill/disc.shpak",
        "sd:/Silent Hill (USA).bin",
        "rom:/disc.shpak",
        "rom:/disc.bin",
    };
    unsigned i;

    if (s_inited)
        return;
    s_inited = 1;

    /* Mount both before probing. Either can legitimately be absent: a cartridge
     * with no SD slot in use, or a ROM with no filesystem attached. */
    if (debug_init_sdfs("sd:/", -1))
        SH_DBG("[CD] sd: mounted");
    else
        SH_DBG("[CD] sd: not available");

    if (dfs_init(DFS_DEFAULT_LOCATION) == DFS_ESUCCESS)
        SH_DBG("[CD] rom: mounted");
    else
        SH_DBG("[CD] rom: no dfs in cart");

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        if (TryOpen(names[i]))
            return;
        SH_DBG("[CD] not found: %s", names[i]);
    }

    strncpy(g_CdBinPath, "NOT FOUND", sizeof(g_CdBinPath) - 1);
    SH_DBG("[CD] NO disc image - see paths tried above");
}

void CdInit(void) { Cd_N64Init(); }

/* ---------------------------------------------------------------- reads */

int CdControl(unsigned char com, unsigned char* param, unsigned char* result)
{
    (void)result;
    if (com == CDL_SETLOC && param)
        s_curSector = CdPosToInt((const CdlLOC*)param);
    return 1;
}

int CdControlB(unsigned char com, unsigned char* param, unsigned char* result)
{
    return CdControl(com, param, result);
}

/* 8 sectors, not the PSP port's 32. The gulp buffer is bss on a machine with
 * about a megabyte to spare, and 32 would be 75 KB of it; 8 is 18 KB and still
 * turns a thousand-sector file into 125 reads rather than 1000. The 304 bytes
 * of sync/header/ECC per sector are what stop us reading straight into dst. */
#define CD_GULP_SECTORS 8
static unsigned char s_gulpBuf[CD_GULP_SECTORS * BIN_SECTOR_SIZE];

int CdRead(int sectors, unsigned long* buf, int mode)
{
    unsigned char* dst  = (unsigned char*)buf;
    int            done = 0;

    (void)mode;

    if (!s_bin || !buf || sectors <= 0)
    {
        if (!s_warned)
        {
            s_warned = 1;
            SH_DBG("[CD] read with no disc image mounted");
        }
        return 1;
    }

    if (s_packRuns != NULL)
    {
        /* Pack: already cooked, so the payload streams straight into dst with
         * no per-sector unpacking. Runs are contiguous in the file, so this
         * only re-seeks when the LBA walks off the end of one. */
        while (done < sectors)
        {
            long idx = Pack_LbaToIndex((unsigned int)(s_curSector + done));

            if (idx < 0)
            {
                if (!s_warned)
                {
                    s_warned = 1;
                    SH_DBG("[CD] lba %d not in pack - rebuild with more --dirs",
                           s_curSector + done);
                }
                break;
            }

            if (fseek(s_bin, (long)s_packDataOffset + idx * BIN_DATA_SIZE, SEEK_SET) != 0)
                break;
            if (fread(dst + (size_t)done * BIN_DATA_SIZE, 1, BIN_DATA_SIZE, s_bin)
                    != BIN_DATA_SIZE)
                break;
            done++;
        }
    }
    else if (fseek(s_bin, (long)s_curSector * BIN_SECTOR_SIZE, SEEK_SET) == 0)
    {
        while (done < sectors)
        {
            int    want = sectors - done;
            int    got;
            size_t bytes;
            int    i;

            if (want > CD_GULP_SECTORS)
                want = CD_GULP_SECTORS;

            bytes = fread(s_gulpBuf, 1, (size_t)want * BIN_SECTOR_SIZE, s_bin);
            got   = (int)(bytes / BIN_SECTOR_SIZE);
            if (got <= 0)
                break;                     /* short read / EOF: keep what we have */

            for (i = 0; i < got; i++)
                memcpy(dst + (size_t)(done + i) * BIN_DATA_SIZE,
                       s_gulpBuf + (size_t)i * BIN_SECTOR_SIZE + BIN_DATA_OFFSET,
                       BIN_DATA_SIZE);
            done += got;
        }
    }

    s_curSector += sectors;
    return 1;
}

int   CdReadSync(int mode, unsigned char* result) { (void)mode; (void)result; return 0; }  /* 0 = done */
int   CdSync(int mode, unsigned char* result)     { (void)mode; (void)result; return CDL_COMPLETE; }
void* CdSearchFile(void) { return 0; }   /* the game uses its static file table */

/* Raw sector access (XA streaming). XA audio lives in Mode-2/Form-2 sectors:
 * 2324-byte payloads with the 8-byte subheader at raw offset 16, which the
 * cooked 2048-byte path above cannot carry. s_curSector is untouched and CdRead
 * re-seeks absolutely on every call, so interleaving is safe. */
int Cd_XboxReadRaw(unsigned int lbn, unsigned char* buf, int sectors)
{
    if (!s_bin || !buf || sectors <= 0)
        return 0;
    if (fseek(s_bin, (long)lbn * BIN_SECTOR_SIZE, SEEK_SET) != 0)
        return 0;
    return fread(buf, BIN_SECTOR_SIZE, (size_t)sectors, s_bin) == (size_t)sectors;
}

FILE* Cd_XboxGetBinFile(void)
{
    Cd_N64Init();
    return s_bin;
}

void Cd_XboxInit(void) { Cd_N64Init(); }
