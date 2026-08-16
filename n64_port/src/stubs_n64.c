/*
 * stubs_n64.c - the N64 side of subsystems the reused xbox_port / pc_port code
 * calls but that this port has not implemented yet, plus the handful of small
 * platform services that have a real N64 answer.
 *
 * Every entry here is one of two things and the comment says which: a REAL
 * implementation, or an honest "not available" that reports failure rather
 * than pretending to succeed. Nothing here fakes a result.
 */
#include <libdragon.h>

#include <stdio.h>
#include <string.h>

#include "sh_log.h"

/* --------------------------------------------------------------- memory */

/* Real. get_memory_size() reports 4 MB or 8 MB depending on the Expansion Pak;
 * the free figure is what malloc has left of it after the static image. */
unsigned Xbox_MemFreeKB(void)
{
    return (unsigned)(get_memory_size() / 1024) - (unsigned)(((char*)sbrk(0) - (char*)0x80000000) / 1024);
}

void Xbox_MemReport(const char* tag)
{
    SH_DBG("[MEM] %s: rdram=%u KB heap_used=%u KB",
           tag ? tag : "?",
           (unsigned)(get_memory_size() / 1024),
           (unsigned)(((char*)sbrk(0) - (char*)0x80000000) / 1024));
}

/* Not available. There is no dashboard, no XMB and no OS to return to: a
 * cartridge that stops running just stops. Halting loudly beats falling off
 * the end of main into a black screen with no explanation. */
void Xbox_QuitToDashboard(void)
{
    SH_DBG("[SYS] quit requested; halting");
    SH_DebugLogFlush();
    while (1)
        wait_ms(100);
}

/* --------------------------------------------------------------- save dir */

/* Not available yet. Saves belong on the Controller Pak (libdragon's cpakfs)
 * or the SD card, and neither is a path. Returning 0 makes the caller's
 * "could not resolve a save location" branch run, which is the truth. */
int XboxFs_ResolveSaveDir(char* out, int outSize)
{
    (void)out; (void)outSize;
    return 0;
}

/* --------------------------------------------------------------- audio */

/* Not available yet. The N64 has no SPU: mixing is the RSP's job through
 * libdragon's mixer, and the PSX VAG banks have to be transcoded to VADPCM
 * offline before any of it plays. Pumping nothing is honest; the game's own
 * mixer state still advances because SpuInit and the sequencer are shared code
 * that runs regardless. */
void Audio_XboxPump(void) { }

void XaPlayer_Play(int fileIdx, int channel, int loop)
{
    (void)fileIdx; (void)channel; (void)loop;
}
void XaPlayer_Stop(void) { }
void XaPlayer_Update(void) { }
void XaPlayer_SetVolume(int vol) { (void)vol; }
int  Xa_IsVoiceAudioDraining(void) { return 0; }
void Xa_VoiceGapHold(int frames) { (void)frames; }

/* --------------------------------------------------------------- fmv */

/* Not available yet, but decided: libdragon ships an MPEG-1 decoder (mpeg2.h
 * plus RSP YUV blitting in yuv.h), so the STR files get transcoded rather than
 * a codec getting written. Returning without playing lets the game continue to
 * the scene after the movie instead of waiting on a stream that never ends. */
int FMV_Play(const char* path)
{
    SH_DBG("[FMV] skipped (no decoder wired yet): %s", path ? path : "?");
    return 0;
}

/* --------------------------------------------------------------- overlays */

/* Not available. The debug overlay draws through the PC port's text layer,
 * which needs the renderer that milestone 3 brings. */
void DbgOverlay_XboxRender(void) { }

/* --------------------------------------------------------------- retro */

/* Not available, and unlikely to be: RetroAchievements needs a network stack
 * the N64 does not have. */
void Pc_Ra_Update(void) { }
void Pc_Ra_StatusToast(const char* msg) { (void)msg; }
void RaBadge_RenderDirect(void) { }
