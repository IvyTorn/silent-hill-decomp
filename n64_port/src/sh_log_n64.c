/*
 * sh_log_n64.c - SH_DBG's N64 backend.
 *
 * SH_DBG is fprintf(g_ShDebugLog, ...) and nothing else, so the whole job is
 * pointing that FILE* somewhere real. On a cartridge there is no writable
 * filesystem to open a SilentHill.log on -- but libdragon already routes stderr
 * to its debug channels, which is exactly what debugf() is a wrapper over. So
 * g_ShDebugLog IS stderr, and every existing SH_DBG in the shared tree reaches
 * IS-Viewer (visible in ares and every modern emulator) and USB (carried back
 * to sc64deployer from the SummerCart64) with no changes at the call sites.
 *
 * stderr cannot be a static initialiser: newlib resolves it through
 * _impure_ptr at runtime. Hence the assignment in SH_DebugLogInit.
 */
#include <libdragon.h>
#include <stdio.h>

#include "sh_log.h"

FILE* g_ShDebugLog = NULL;
int   g_ShDebugEchoStdout = 0;
void (*g_ShOverlayPushLine)(const char*) = NULL;
void (*g_ShOverlayToastLine)(const char*) = NULL;

/* Log-volume gate, inherited from the Xbox port where the per-frame diagnostic
 * probes flooded a multi-day session to 136 MB. The cost here is worse than an
 * HDD's: IS-Viewer and USB are slow enough that a chatty frame shows up as a
 * hitch. Defaults OFF; the essential boot/CD/FS/error lines are not in the
 * gated set, so they log regardless. */
int g_XboxLogDiag = 0;

/* The Xbox gate drops the per-frame probes by fmt PREFIX. fmt is always a
 * compile-time literal at the call site, so a dropped line costs a pointer
 * compare and never formats. */
int Sh_LogAllow(const char* fmt)
{
    if (g_XboxLogDiag)
        return 1;

    if (fmt == NULL)
        return 1;

    if (fmt[0] == '[')
    {
        switch (fmt[1])
        {
            case 'U': if (fmt[2] == 'P') return 0; break;   /* [UPD] */
            case 'F': if (fmt[2] == 'T') return 0; break;   /* [FT]  */
            case 'M': if (fmt[2] == 'E') return 0; break;   /* [MEM] */
            default: break;
        }
    }
    return 1;
}

void SH_DebugLogInit(void)
{
    if (g_ShDebugLog != NULL)
        return;

    g_ShDebugLog = stderr;
    /* Unbuffered: the session ends by the user pulling power on a console, so
     * anything still sitting in a buffer is lost. There is no file whose size
     * has to be rewritten here, but the exposure to a hard power-off is the
     * same one that cost the 360 port a zero-byte log. */
    setvbuf(g_ShDebugLog, NULL, _IONBF, 0);
}

void SH_DebugLogFlush(void)
{
    if (g_ShDebugLog != NULL)
        fflush(g_ShDebugLog);
}
