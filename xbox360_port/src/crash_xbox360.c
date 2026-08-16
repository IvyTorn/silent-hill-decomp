/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * crash_xbox360.c - get libXenon's exception report onto the USB stick.
 *
 * THE PROBLEM. When this port faults, the log simply stops. There is no report,
 * no faulting address, nothing -- so a crash costs several hardware runs of
 * inference instead of one look at an address. That is not because the console
 * has no exception handling: libXenon installs the full set of PowerPC vectors
 * (except.o: machine check, DSI, ISI, program, alignment, ...) and c_except.o's
 * crashdump() already prints exception type, PIR, DAR, SRR0/SRR1/LR, all 32
 * GPRs and a stack dump. It prints them to the CONSOLE. Once Xe owns the
 * framebuffer that output is invisible, and it never reaches the stick either
 * way, so all that information was being produced and thrown away.
 *
 * WHY A TEE AND NOT AN OVERRIDE. crashdump is a global symbol, so it can be
 * replaced by link order, but then its formatting would have to be
 * reimplemented -- and that means reverse-engineering the exception context
 * struct from the disassembly (the GPR array is walked backwards from the
 * pointer it is handed, with PIR/DAR read as split 32-bit halves at +0x130 and
 * +0x138). Getting that subtly wrong produces a confident, wrong register dump,
 * which is worse than none. Wrapping console_putch instead keeps libXenon's own
 * decoding and only moves the bytes: whatever it decides to print, we capture
 * verbatim. It also survives libXenon changing that struct.
 *
 * console_putch cannot simply be redefined here -- console.o also defines
 * console_init/clrscr/set_colors, which are needed, so a duplicate definition
 * would clash. `ld --wrap=console_putch` (in build_xbox360.sh) has neither
 * problem: every existing reference is redirected to __wrap_console_putch and
 * the original stays reachable as __real_console_putch.
 *
 * THE OTHER HALF, AND THE BIGGER ONE. g_ShDebugLog is a 256 KB fully-buffered
 * stream, and SH_DebugLogFlush was only called at boot, on quit, and inside the
 * [MAP-GUARD] "not linked" path -- which stopped firing the moment all 43
 * overlays were linked. So at a crash, everything since the last flush was
 * still sitting in the stdio buffer and was lost with the console. The end of a
 * log was therefore a BUFFER BOUNDARY, not the point of failure, and reading it
 * as "it died here" is how log 030 was misread. Flushing what is already
 * buffered BEFORE writing the crash lines is what makes the report land in the
 * right place, with the run's real last moments above it.
 */
#include <stdio.h>
#include <string.h>

#include "sh_log.h"

extern void SH_DebugLogFlush(void);   /* sh_log_xbox360.c, not in sh_log.h */

/* Supplied by the linker under --wrap: the real console_putch. */
extern void __real_console_putch(const char c);

static char s_line[512];
static int  s_len;
static int  s_crash;      /* latched: a crash report is in progress */
static int  s_busy;       /* re-entry guard, see below */

/* libXenon's crashdump emits these before anything else worth having. Matching
 * on its own strings rather than on a flag we set means the tee also catches a
 * fault raised inside libXenon itself, before any of our code would know. */
static int LineStartsReport(const char* s)
{
    return strstr(s, "Exception vector!") != NULL ||
           strstr(s, "Segmentation fault!") != NULL;
}

static void EmitLine(const char* line)
{
    if (!g_ShDebugLog)
        return;

    if (!s_crash && LineStartsReport(line))
    {
        s_crash = 1;
        /* Order matters: push the run's buffered tail out FIRST, so the report
         * lands after the last thing the game actually did instead of after
         * whatever the last flush happened to be. */
        SH_DebugLogFlush();
        fprintf(g_ShDebugLog,
                "\n======== CRASH ========\n"
                "[CRASH] libXenon exception handler entered. Register dump follows.\n"
                "[CRASH] Addresses are absolute; the image is linked at 0x80000000, so\n"
                "[CRASH] resolve sr0 directly against xenon.debug.elf (xenon-nm / addr2line).\n");
        SH_DebugLogFlush();
    }

    fprintf(g_ShDebugLog, "%s %s\n", s_crash ? "[CRASH]" : "[CON]", line);

    /* During a report every line is worth an fsync: the next instruction may be
     * the one that takes the console down, and a report only half on disk is
     * exactly the situation this file exists to prevent. Outside a report the
     * console is low volume, so it rides along in the normal buffer. */
    if (s_crash)
        SH_DebugLogFlush();
}

void __wrap_console_putch(const char c)
{
    /* Console first. If anything below faults, the screen still showed the line
     * -- the tee must never be able to make the crash reporting worse. */
    __real_console_putch(c);

    /* fprintf on a libfat stream can reach printf on an error path, which would
     * come straight back here. One line of recursion is enough to lose the
     * report, so drop rather than recurse. */
    if (s_busy)
        return;
    s_busy = 1;

    if (c == '\n' || s_len >= (int)sizeof(s_line) - 1)
    {
        s_line[s_len] = '\0';
        if (s_len > 0)
            EmitLine(s_line);
        s_len = 0;
    }
    else if (c != '\r')
    {
        s_line[s_len++] = c;
    }

    s_busy = 0;
}

/* Called once from main so a log can be checked for "is the tee even active?"
 * without a crash. A missing line here means --wrap was dropped from the link,
 * which is silent otherwise. */
void Sh360Crash_Init(void)
{
    SH_DBG("[CRASH] console tee armed (libXenon exception reports will be logged)");
    SH_DebugLogFlush();
}
