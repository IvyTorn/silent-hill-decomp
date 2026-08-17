/*
 * sh_log_n64.c - SH_DBG's N64 backend.
 *
 * SH_DBG is fprintf(g_ShDebugLog, ...) and nothing else, so the whole job is
 * pointing that FILE* somewhere real. On a cartridge there is no writable
 * filesystem to open a SilentHill.log on, so g_ShDebugLog is a funopen stream
 * that fans out to two places: libdragon's stderr (IS-Viewer, and USB carried
 * back to sc64deployer from the SummerCart64) and the on-screen ring below.
 * Every existing SH_DBG in the shared tree reaches both with no call-site
 * changes.
 *
 * The FILE* cannot be a static initialiser: newlib resolves stderr through
 * _impure_ptr at runtime. Hence the work in SH_DebugLogInit.
 */
#include <libdragon.h>
#include <stdio.h>

#include "sh_log.h"
#include "sh_log_n64.h"

FILE* g_ShDebugLog = NULL;
int   g_ShDebugEchoStdout = 0;
void (*g_ShOverlayPushLine)(const char*) = NULL;
void (*g_ShOverlayToastLine)(const char*) = NULL;

/* Log-volume gate; see Sh_LogAllow. Declared here because the ring filter
 * below also honours it. */
int g_XboxLogDiag = 0;

/* ------------------------------------------------------- on-screen log */

/* Every other port can read its log back off a disk. This one cannot, and the
 * emulator makes it worse: ares surfaces IS-Viewer only in a GUI window of its
 * own, so a headless run sees nothing at all. Without this the only way to
 * locate a hang was to rebuild with a probe and diff two screenshots.
 *
 * The ring holds the last SH_LOG_ROWS lines; gpu_rdp.c paints it over the frame
 * when ShLogN64_ScreenEnabled(). Off once there is a picture worth looking at. */
#define SH_LOG_ROWS 22
/* 40, not more: libdragon's built-in font is 8px wide and the screen is 320,
 * so anything longer runs off the right edge and renders as overlapping mush. */
#define SH_LOG_COLS 40

static char s_rows[SH_LOG_ROWS][SH_LOG_COLS];
static int  s_head;                 /* next row to write */
static int  s_filled;
static int  s_screenLog = 1;

/* Dropped from the ON-SCREEN ring only; every one still reaches stderr, so
 * IS-Viewer and the USB capture keep full fidelity. These are the high-volume
 * per-load and per-frame lines, and the ring is 22 rows -- three TIM loads bury
 * the boot summary, the heap report, and the [DSO]/[MAP-LOAD] lines that say
 * whether an overlay actually came in.
 *
 * This lives here rather than in Sh_LogAllow because half of them never go
 * through SH_DBG at all: fsqueue_3.c writes "[BOOT0/TIM]" with a bare
 * fprintf(g_ShDebugLog, ...), which no gate on the format string can see. That
 * is why muting them there had no effect. */
static const char* const s_ringMuted[] = {
    "[OTS", "[OTT", "[ABR", "[FOGPAD", "[BIDI", "[UIDIAG",
    "[MCFSM", "[MCRD", "[BOOT0", "[FSQ", "[UPD", "[FT]",
};

static int ShLog_RingMuted(const char* line)
{
    unsigned i;

    for (i = 0; i < sizeof(s_ringMuted) / sizeof(s_ringMuted[0]); i++)
    {
        const char* p = s_ringMuted[i];
        const char* f = line;
        while (*p != '\0' && *p == *f) { p++; f++; }
        if (*p == '\0')
            return 1;
    }
    return 0;
}

/* One logical line: filtered as a whole, then wrapped across as many 39-column
 * rows as it needs. */
void ShLogN64_PushWrapped(const char* line)
{
    int len, off;

    if (line == NULL)
        return;
    if (!g_XboxLogDiag && ShLog_RingMuted(line))
        return;

    len = 0;
    while (line[len] != '\0')
        len++;

    for (off = 0; off < len; off += SH_LOG_COLS - 1)
        ShLogN64_Push(line + off);
    if (len == 0)
        ShLogN64_Push("");
}

void ShLogN64_Push(const char* line)
{
    int i;

    if (line == NULL)
        return;

    for (i = 0; i < SH_LOG_COLS - 1 && line[i] != '\0' && line[i] != '\n'; i++)
        s_rows[s_head][i] = line[i];
    s_rows[s_head][i] = '\0';

    s_head = (s_head + 1) % SH_LOG_ROWS;
    if (s_filled < SH_LOG_ROWS)
        s_filled++;
}

int         ShLogN64_ScreenEnabled(void) { return s_screenLog; }
void        ShLogN64_ScreenEnable(int on) { s_screenLog = on; }
int         ShLogN64_Rows(void) { return s_filled; }

/* Oldest first, so the caller draws top-to-bottom in arrival order. */
const char* ShLogN64_Row(int i)
{
    int start;

    if (i < 0 || i >= s_filled)
        return "";

    start = (s_filled == SH_LOG_ROWS) ? s_head : 0;
    return s_rows[(start + i) % SH_LOG_ROWS];
}

/* Log-volume gate, inherited from the Xbox port where the per-frame diagnostic
 * probes flooded a multi-day session to 136 MB. The cost here is worse than an
 * HDD's: IS-Viewer and USB are slow enough that a chatty frame shows up as a
 * hitch. Defaults OFF; the essential boot/CD/FS/error lines are not in the
 * gated set, so they log regardless. */

/* The Xbox gate drops the per-frame probes by fmt PREFIX. fmt is always a
 * compile-time literal at the call site, so a dropped line costs a pointer
 * compare and never formats.
 *
 * Kept SMALL and unchanged from the Xbox's set: anything dropped here is never
 * emitted at all, on any channel. Choosing what the 22-line SCREEN shows is a
 * different question, and it is the ring filter's (ShLog_RingMuted) -- which
 * also catches the lines that never reach this function, because half the
 * chatty ones write straight to g_ShDebugLog with a bare fprintf. */
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
            case 'M': if (fmt[2] == 'E' && fmt[3] == 'M' && fmt[4] == ']') return 0; break;
            default: break;
        }
    }
    return 1;
}

/* Splits the stream into lines for the ring and forwards the bytes to stderr,
 * which is where libdragon's IS-Viewer and USB writers already sit. Both
 * destinations get everything; neither knows about the other. */
static int ShLog_Write(void* cookie, const char* buf, int len)
{
    /* A WHOLE logical line, not a 39-char screen row. The ring filter matches on
     * a prefix, so it has to see the start of the line -- and a long [OTS] line
     * chopped into three rows only carries its prefix on the first, which let
     * every continuation through and buried the screen in exactly the noise the
     * filter existed to remove. Decide once per line, then wrap for display. */
    static char line[256];
    static int  n;
    int         i;

    (void)cookie;

    for (i = 0; i < len; i++)
    {
        if (buf[i] != '\n' && n < (int)sizeof(line) - 1)
        {
            line[n++] = buf[i];
            continue;
        }

        if (buf[i] != '\n')
            line[n++] = buf[i];
        line[n] = '\0';

        if (n > 0)
            ShLogN64_PushWrapped(line);
        n = 0;
    }

    fwrite(buf, 1, (size_t)len, stderr);
    return len;
}

void SH_DebugLogInit(void)
{
    if (g_ShDebugLog != NULL)
        return;

    /* funopen rather than plain stderr: the on-screen ring needs a copy, and
     * hooking stdio directly would REPLACE libdragon's __debug_write (a single
     * slot) and take IS-Viewer and USB down with it. */
    g_ShDebugLog = funopen(NULL, NULL, ShLog_Write, NULL, NULL);
    if (g_ShDebugLog == NULL)
        g_ShDebugLog = stderr;

    /* Unbuffered: the session ends by the user pulling power on a console, so
     * anything still sitting in a buffer is lost. That exposure is the one that
     * cost the 360 port a zero-byte log. */
    setvbuf(g_ShDebugLog, NULL, _IONBF, 0);
}

void SH_DebugLogFlush(void)
{
    if (g_ShDebugLog != NULL)
        fflush(g_ShDebugLog);
}
