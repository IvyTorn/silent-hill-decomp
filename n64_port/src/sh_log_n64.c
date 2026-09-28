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
    "[BOOT0", "[FSQ", "[UPD", "[FT]", "[MCFSM", "[MCRD", "[SH_AUDIO", "[FONTDUMP",
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
    /* Every per-frame or per-primitive probe, by prefix. On this port a line
     * is an SD-card write plus a USB write, so a probe that fires per frame is
     * a frame-time cost, not a log-size cost: the session that measured the
     * world at 1 fps carried 22770 lines, two of them ([GOLD], [TLUT]) firing
     * every frame. Per-window censuses ([PROF], [GPU], [OTT], [OTS], [WORLD],
     * [CAMPOS], [ROAD]) and one-shots stay. log_diag=1 restores everything. */
    static const char* const GATED[] = {
        "[UPD]", "[UPD2]", "[FT]", "[MEM]",
        "[GOLD]", "[TLUT]", "[TPGE]", "[BIGPRIM]", "[MESH]", "[MESHD]",
        "[BF]", "[BF4]", "[XF]", "[XF0]", "[UT]", "[UT2]", "[UT3]", "[MV]",
        "[BONE2]", "[LIT]", "[LITM]", "[TINT]", "[ANIMB]", "[CAM]",
        "[PIPE]", "[PIPE2]", "[WVS]", "[WALLSTOP]", "[WALL-HIT]", "[PADR]",
        "[FONTDUMP]", "[MGLY]", "[MGLY2]", "[FLEX]", "[MUZZLE]", "[TXTPG]",
        "[TXSPR]", "[UIDIAG]", "[KO]", "[VKO2]", "[VKO3]", "[VKO4]",
        "[FOGST]", "[FOGPAD]", "[FSQ]", "[STORE]", "[MCFSM]",
        "[RAIN]", "[SS]", "[FXDROP]", "[BATCH]", "[ZETA]", "[ITEMZ]",
    };
    int i;

    if (g_XboxLogDiag)
        return 1;
    if (fmt == NULL || fmt[0] != '[')
        return 1;

    for (i = 0; i < (int)(sizeof(GATED) / sizeof(GATED[0])); i++)
    {
        const char* g = GATED[i];
        const char* f = fmt;
        while (*g && *g == *f) { g++; f++; }
        if (*g == '\0')
            return 0;
    }
    return 1;
}

/* Splits the stream into lines for the ring and forwards the bytes to stderr,
 * which is where libdragon's IS-Viewer and USB writers already sit. Both
 * destinations get everything; neither knows about the other. */
/* Below this, a run is a boot that never got anywhere and its log must not
 * evict the previous one. A title-screen-only boot is around 8 KB; a run that
 * reaches gameplay passes this within the first room. */
#define SH_LOG_ROTATE_MIN_BYTES (24 * 1024)

/* >0: KB rotated to .prev.log. <0: -(KB)-1, kept because it was too short.
 * 0: nothing there. Reported once the mirror is open. */
static int   s_rotated;

static FILE* s_sdMirror;
static int   s_sdWanted;   /* mirror requested: reopen attempts may continue */

/* The mirror's stdio buffer. With the default (tiny) buffer, mid-frame
 * fwrites spilled to the card whenever it happened to fill: 14 ms frame
 * stalls at random. 16 KB holds over a second of log; the card is touched
 * only by the once-per-second commit below. */
static char  s_sdBuf[16 * 1024];

static FILE* ShLogN64_SdOpen(const char* mode)
{
    FILE* f = fopen("sd:/silenthill/silenthill.log", mode);
    if (f != NULL)
        setvbuf(f, s_sdBuf, _IOFBF, sizeof(s_sdBuf));
    return f;
}

/* What the log costs the frame, per sink (reset each frame, read by [PROF]).
 * stderr is libdragon's IS-Viewer + USB writers; on a flashcart with no host
 * attached the USB writer can wait for its timeout. */
unsigned long long g_ProfLogStderrTicks, g_ProfLogSdTicks;
int                g_ProfLogLines;

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

    g_ProfLogLines++;
    {
        unsigned long long _t0 = get_ticks();
        fwrite(buf, 1, (size_t)len, stderr);
        g_ProfLogStderrTicks += get_ticks() - _t0;
    }
    {
        unsigned long long _t0 = get_ticks();
        if (s_sdMirror != NULL)
            fwrite(buf, 1, (size_t)len, s_sdMirror);
        /* Self-driven commit: the external once-per-second flush rides VSync's
         * vblank counter, and any code path that stalls or bypasses VSync starves
         * it -- the FAT size then freezes at the last fclose and the log READS as
         * dead while lines keep flowing. Committing from inside the writer makes
         * the cadence unstarvable: any line more than a second after the last
         * commit cycles the file. Never call SH_DebugLogFlush here (fflush of the
         * stream this callback serves would re-enter it). */
        if (s_sdWanted)
        {
            static uint32_t s_lastCommitMs;
            uint32_t nowMs = (uint32_t)get_ticks_ms();
            if (nowMs - s_lastCommitMs > 5000)  /* 1s cost a 12ms fclose hitch per second in [PROF] */
            {
                s_lastCommitMs = nowMs;
                if (s_sdMirror != NULL)
                    fclose(s_sdMirror);
                s_sdMirror = ShLogN64_SdOpen("a");
            }
        }
        g_ProfLogSdTicks += get_ticks() - _t0;
    }
    return len;
}

/* Hardware has no IS-Viewer, and until now no way to hand a log back at all -
 * every hardware report was a phone photo of a screen. The SD card the disc
 * already lives on takes a mirror of the whole stream; the per-second flush
 * below keeps it fresh without a per-line card write. Called from main once
 * the SD mount has settled. */
void ShLogN64_EnableSdMirror(void)
{
    if (s_sdMirror != NULL)
        return;
    s_sdWanted = 1;
    /* Rotate the previous run's log aside before truncating: the N64 has no
     * iterative logging and a crash log is otherwise overwritten by the reboot
     * that follows it. After a crash+reboot, .prev.log holds the crash run.
     * rename() is a no-op on this SD/FAT layer (verified on hardware: no
     * .prev.log ever appeared), so copy the bytes across by hand.
     *
     * ONLY rotate a log worth keeping. With one generation and an
     * unconditional copy, two short boots destroy a long session: that is
     * exactly how a ten-minute hardware run through the hospital was lost on
     * 2026-09-27, with both .log and .prev.log holding nothing but a title
     * screen. A run that never left the menus is a few KB; a run that played
     * is tens to hundreds. Below the threshold the previous run is not worth
     * evicting the one before it, so .prev.log is left alone and the real
     * session survives any number of "does it still boot" restarts. */
    {
        FILE* src = fopen("sd:/silenthill/silenthill.log", "rb");
        if (src != NULL)
        {
            long size = 0;
            if (fseek(src, 0, SEEK_END) == 0)
            {
                size = ftell(src);
                fseek(src, 0, SEEK_SET);
            }
            if (size >= SH_LOG_ROTATE_MIN_BYTES)
            {
                FILE* dst = fopen("sd:/silenthill/silenthill.prev.log", "wb");
                if (dst != NULL)
                {
                    static char cp[4096];
                    size_t n;
                    while ((n = fread(cp, 1, sizeof cp, src)) > 0)
                        fwrite(cp, 1, n, dst);
                    fclose(dst);
                }
                s_rotated = (int)(size / 1024);
            }
            else
            {
                s_rotated = -(int)(size / 1024) - 1;   /* kept, not rotated */
            }
            fclose(src);
        }
    }
    s_sdMirror = ShLogN64_SdOpen("w");
    if (s_sdMirror != NULL)
    {
        if (s_rotated > 0)
            SH_DBG("[LOG] mirroring to sd:/silenthill/silenthill.log (previous run, %d KB, moved to silenthill.prev.log)",
                   s_rotated);
        else if (s_rotated < 0)
            SH_DBG("[LOG] mirroring to sd:/silenthill/silenthill.log (previous run was only %d KB -- too short to be worth keeping, silenthill.prev.log left as it was)",
                   -(s_rotated + 1));
        else
            SH_DBG("[LOG] mirroring to sd:/silenthill/silenthill.log (no previous run to rotate)");
    }
}

/* Called from the SH_PATCHed libdragon __rsp_crash BEFORE it touches the
 * display: the on-screen inspector re-inits video and, on this heap-starved
 * game (sbrk_top framebuffers cannot be reclaimed mid-run), OOMs into its
 * own assert -- the dump pages the user photographs are the wreckage. Commit
 * the crash identity to the SD log FIRST so every RSP crash leaves a line
 * even when the screen shows only the assert cascade. */
void ShN64_RspCrashCommit(unsigned pc, const char* uc, const char* func,
                          const char* file, int line)
{
    void SH_DebugLogFlush(void);   /* defined below */
    SH_DBG("[CRASH] RSP crash: ucode=%s pc=%03x at %s (%s:%d)",
           uc ? uc : "?", pc, func ? func : "?", file ? file : "?", line);
    SH_DebugLogFlush();
    ShLogN64_CrashCommit();
}

/* Second half of the same SH_PATCH, called right after RspCrashCommit with
 * what libdragon's own rspq crash handler would have printed on the console
 * it can never open here: the SP/DP status words, whether the RDP was still
 * making progress, the RSP's queue view (where it was READING commands from,
 * against the two CPU-side queue buffers) and the 16 words at that read
 * pointer. A watchdog "RSP crash" with pc=018 is the ucode parked at
 * RSPQCmd_WaitNewInput -- it read a zero command word and went to sleep --
 * so those 16 words say whether the queue really was empty there (a lost
 * wake-up) or held commands the RSP never saw (a clobbered queue). Bits
 * decoded up front because a phone photo of the red screen is otherwise the
 * only copy of them. */
void ShN64_RspCrashDetail(unsigned spStatus, int sigMore, int bufdoneLo, int bufdoneHi,
                          unsigned dpStatus, int rdpCrashed,
                          unsigned dpStart, unsigned dpEnd, unsigned dpCurrent,
                          unsigned qLow, unsigned qHigh, unsigned qDram, unsigned qGp,
                          unsigned rdpBuf0, unsigned rdpBuf1, unsigned rdpCur,
                          unsigned rdpSentinel, unsigned ovl)
{
    void SH_DebugLogFlush(void);
    unsigned cur = qDram + qGp;

    /* The commit above closed the mirror; reopen in append so these land too. */
    if (s_sdMirror == NULL)
        s_sdMirror = ShLogN64_SdOpen("a");

    SH_DBG("[CRASH] sp=%08x halt=%u broke=%u dmaBusy=%u sigMore=%d bufdone=%d/%d | dp=%08x rdpCrashed=%d "
           "xbus=%u freeze=%u flush=%u pipeBusy=%u cmdBusy=%u dmaBusy=%u | dpStart=%08x dpEnd=%08x dpCur=%08x",
           spStatus, spStatus & 1u, (spStatus >> 1) & 1u, (spStatus >> 2) & 1u,
           sigMore, bufdoneLo, bufdoneHi,
           dpStatus, rdpCrashed,
           dpStatus & 1u, (dpStatus >> 1) & 1u, (dpStatus >> 2) & 1u,
           (dpStatus >> 5) & 1u, (dpStatus >> 6) & 1u, (dpStatus >> 8) & 1u,
           dpStart, dpEnd, dpCurrent);
    SH_DBG("[CRASH] rspq low=%08x high=%08x reading=%08x (dram %08x + gp %x) ovl=%x | rdp buf=%08x/%08x cur=%08x sentinel=%08x",
           qLow, qHigh, cur, qDram, qGp, ovl, rdpBuf0, rdpBuf1, rdpCur, rdpSentinel);
    /* The CPU's side of the hand-off: where rspq_write was putting commands
     * (uncached pointer) and its buffer-switch sentinel. reading vs cpuWrite
     * says whether both sides even agree on which 2KB buffer is live. */
    SH_DBG("[CRASH] cpuWrite=%08x cpuSentinel=%08x",
           (unsigned)(uintptr_t)rspq_cur_pointer, (unsigned)(uintptr_t)rspq_cur_sentinel);
    if ((cur & 0x7FFFFFu) < 0x7FFF00u)
    {
        /* Uncached RDRAM read of the queue around the RSP's read pointer;
         * -8..+7 words, '*' marks the word at the pointer. */
        const volatile unsigned* q = (const volatile unsigned*)(0xA0000000u | (cur & 0x7FFFFCu));
        char line[200];
        int  i, n = 0;
        for (i = -8; i < 8 && n < (int)sizeof(line) - 12; i++)
        {
            if (i < 0 && (cur & 0x7FFFFCu) < (unsigned)(-i * 4))
                continue;
            n += snprintf(line + n, sizeof(line) - n, "%08x%c", q[i], i == 0 ? '*' : ' ');
        }
        SH_DBG("[CRASH] queue@reading: %s", line);
    }
    /* What the RDP choked on. A stuck DP_CURRENT is a different failure from
     * a parked RSP, and the command it stopped at is the only evidence. */
    if ((rdpCur & 0x7FFFFFu) < 0x7FFF00u && rdpCur >= 0x1000u)
    {
        const volatile unsigned* r = (const volatile unsigned*)(0xA0000000u | (rdpCur & 0x7FFFFCu));
        char line[200];
        int  i, n = 0;
        for (i = -8; i < 8 && n < (int)sizeof(line) - 12; i++)
            n += snprintf(line + n, sizeof(line) - n, "%08x%c", r[i], i == 0 ? '*' : ' ');
        SH_DBG("[CRASH] rdp@current: %s", line);
    }
    /* Corruption census: how many aligned zero words sit in the command
     * stream the CPU has already written but the RSP has not reached? One is
     * the known single-word writer; a run would mean something else. */
    {
        unsigned lo  = cur & 0x7FFFFCu;
        unsigned hi  = ((unsigned)(uintptr_t)rspq_cur_pointer) & 0x7FFFFCu;
        unsigned n   = 0, first[3] = {0, 0, 0}, i;
        if (hi > lo && hi - lo < 0x20000u)
        {
            const volatile unsigned* q = (const volatile unsigned*)(0xA0000000u | lo);
            for (i = 0; i < (hi - lo) / 4u; i++)
                if (q[i] == 0u)
                {
                    if (n < 3)
                        first[n] = lo + i * 4u;
                    n++;
                }
            SH_DBG("[CRASH] zero-word census: %u zeros in %u words ahead of the RSP (first %08x %08x %08x)",
                   n, (hi - lo) / 4u, first[0], first[1], first[2]);
        }
    }
    SH_DebugLogFlush();
    ShLogN64_CrashCommit();
}

/* SH_PATCHed rspq self-heal report (LIBDRAGON_PATCHES.md #4). kind 0 = a zero
 * command header was NOOPed and the RSP restarted (words = what the hole and
 * its argument words held), 1 = the RSP was parked on a valid word (a lost
 * wake-up) and was only restarted, 2 = refused (the parked position was not
 * inside the queue, or the hole is followed by an ambiguous word) -- the 2 s
 * watchdog then still fires. Every line is a sample of the writer we have
 * not found; keep them. */
void ShN64_RspqHealed(unsigned addr, const unsigned* words, int n, int kind)
{
    /* kind 3 skipped the rest of the frame's commands, so the RDP's mode
     * state is whatever the last executed command left: make the PSX path
     * re-send all of it. (t3d re-sends its own state every flush pass.) */
    if (kind == 3)
    {
        extern void GpuNv2a_PsxModeInvalidate(void) __attribute__((weak));
        if (GpuNv2a_PsxModeInvalidate)
            GpuNv2a_PsxModeInvalidate();
    }
    void SH_DebugLogFlush(void);
    char buf[96];
    int  i, len = 0;
    buf[0] = 0;
    for (i = 0; i < n && len < (int)sizeof(buf) - 10; i++)
        len += snprintf(buf + len, sizeof(buf) - len, "%08x ", words[i]);
    SH_DBG("[RSPQ-HEAL] kind=%d (%s) at=%08x hole=%s", kind,
           kind == 0 ? "lost command NOOPed" :
           kind == 1 ? "lost wake-up, restarted" :
           kind == 3 ? "skipped to the write cursor -- one frame of drawing lost" :
                       "REFUSED", addr, buf);
    SH_DebugLogFlush();
}

/* Crash-path commit. fflush alone pushes bytes to the FAT layer but leaves
 * the directory entry's SIZE stale, so everything since the last 1-second
 * fclose cycle -- always the [CRASH] lines themselves -- reads back as
 * missing. fclose commits data AND size; no reopen, the next stop is the
 * red screen. */
void ShLogN64_CrashCommit(void)
{
    if (s_sdMirror != NULL)
    {
        FILE* f = s_sdMirror;
        s_sdMirror = NULL;   /* no further mirror writes mid-death */
        fclose(f);
    }
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

    /* LINE buffered, not unbuffered: newlib hands an unbuffered stream to the
     * writer once per printf SEGMENT (~15 calls for one [PROF] line), and each
     * call paid the IS-Viewer/USB write. [PROF] measured the logger at ~1 ms
     * stderr + up to 14 ms SD per frame in the MENU. A line buffer still
     * flushes on every newline, so the power-pull exposure is one partial
     * line, same as before. */
    {
        static char s_mainBuf[512];
        setvbuf(g_ShDebugLog, s_mainBuf, _IOLBF, sizeof(s_mainBuf));
    }
}

void SH_DebugLogFlush(void)
{
    if (g_ShDebugLog != NULL)
        fflush(g_ShDebugLog);
    /* fflush alone is NOT enough on the card: FAT keeps the file's size in
     * the directory entry, and libdragon's driver only commits that on
     * close. A power-off after an hour of fflush-only writes left a 0-byte
     * log (the first hardware log ever handed back). Close and reopen in
     * append mode on every flush so the entry is current within a second. */
    if (s_sdMirror != NULL)
        fclose(s_sdMirror);
    /* Reopen EVERY flush while the mirror is wanted -- including after a
     * failed reopen. One transient FAT failure (the options/card traffic
     * lands exactly at the boot [VIB] read) used to NULL the mirror forever,
     * which is why every hardware log died at ~200 lines at the same spot. */
    if (s_sdWanted)
        s_sdMirror = ShLogN64_SdOpen("a");
}
