/*
 * cd_n64.c - PSX libcd on N64.
 *
 * MILESTONE 2b SCOPE: no disc. Every read reports failure rather than returning
 * a zeroed buffer, because a zeroed sector is indistinguishable to the game
 * from a real one -- it would boot into a world built out of nothing and the
 * first symptom would be somewhere far from here.
 *
 * The real backing is decided but not written: the SummerCart64's SD card via
 * libdragon's fat.h. That maps onto this interface almost exactly, because
 * fsqueue only ever wants a seekable block device with a directory, which is
 * what the PSX CD was. A dragonfs ROM build for a real cartridge is the same
 * code with a different fopen.
 */
#include <libdragon.h>

#include <stdio.h>

#include "sh_log.h"

#define CDL_COMPLETE 2

typedef struct { unsigned char minute, second, sector, track; } CdlLOC_n64;

static int s_warned;

static void Cd_WarnOnce(const char* what)
{
    if (s_warned)
        return;
    s_warned = 1;
    SH_DBG("[CD] %s: no disc source on this port yet (SD card via fat.h is next)", what);
}

void Cd_N64Init(void)
{
    SH_DBG("[CD] no disc source configured");
}

void CdInit(void) { Cd_N64Init(); }

/* Real: pure arithmetic, no device involved. The game converts logical block
 * numbers to BCD minute/second/sector positions all over the FS queue, and
 * getting this wrong would misplace every read even once a disc exists. */
void* CdIntToPos(int i, void* p)
{
    CdlLOC_n64* loc = (CdlLOC_n64*)p;
    int         m, s, f;

    if (loc == NULL)
        return NULL;

    /* PSX sector addressing starts at 2 seconds, per Red Book. */
    i += 150;
    f = i % 75;  i /= 75;
    s = i % 60;  i /= 60;
    m = i;

    loc->minute = (unsigned char)(((m / 10) << 4) | (m % 10));
    loc->second = (unsigned char)(((s / 10) << 4) | (s % 10));
    loc->sector = (unsigned char)(((f / 10) << 4) | (f % 10));
    loc->track  = 0;
    return loc;
}

int CdControl(unsigned char com, unsigned char* param, unsigned char* result)
{
    (void)com; (void)param; (void)result;
    Cd_WarnOnce("CdControl");
    return 0;                 /* 0 = command not accepted */
}

int CdControlB(unsigned char com, unsigned char* param, unsigned char* result)
{
    (void)com; (void)param; (void)result;
    Cd_WarnOnce("CdControlB");
    return 0;
}

int CdRead(int sectors, unsigned long* buf, int mode)
{
    (void)sectors; (void)buf; (void)mode;
    Cd_WarnOnce("CdRead");
    return 0;                 /* 0 = read not started */
}

int   CdReadSync(int mode, unsigned char* result) { (void)mode; (void)result; return -1; }
int   CdSync(int mode, unsigned char* result)     { (void)mode; (void)result; return CDL_COMPLETE; }
void* CdSearchFile(void) { return 0; }   /* the game uses its static file table */

/* The raw-sector entry points the reused xbox_port code calls. */
int Cd_XboxReadRaw(unsigned int lbn, unsigned char* buf, int sectors)
{
    (void)lbn; (void)buf; (void)sectors;
    Cd_WarnOnce("ReadRaw");
    return 0;
}

FILE* Cd_XboxGetBinFile(void) { return NULL; }
void  Cd_XboxInit(void)       { Cd_N64Init(); }
