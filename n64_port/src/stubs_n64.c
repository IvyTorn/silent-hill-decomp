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

#include <sys/stat.h>

#include "sh_log.h"
#include "savefs_n64.h"

/* TEMP diagnostic: last MainLoop call site, written by ML_TRACE in
 * game_main.c, printed by the pad watchdog. */
const char* g_MlTraceTag = "pre-loop";

/* TEMP diagnostic: camera-ladder call counts, printed by the [WVS] census.
 * [0]=vbSetWorldScreenMatrix [1]=vwSetViewInfo [2]=vcMoveAndSetCamera
 * [3]=vcInitCamera */
int g_N64CamProbe[4];
int g_N64VbSnap[6];

/* --------------------------------------------------------------- memory */

/* Real, and the numbers that matter most on this port: the heap is whatever is
 * left of RDRAM after a ~7 MB static image, and everything that fails quietly
 * on N64 -- the texture cache, the packet arenas, an overlay -- fails by not
 * getting any. sys_get_heap_stats is libdragon's own accounting rather than an
 * sbrk guess, which is what the first version of this was and it was wrong.
 *
 * Note the [MEMN64] tag: Sh_LogAllow mutes "[MEM", and these lines are the ones
 * worth never losing. */
unsigned Xbox_MemFreeKB(void)
{
    heap_stats_t h;
    sys_get_heap_stats(&h);
    return (unsigned)((h.total - h.used) / 1024);
}

void Xbox_MemReport(const char* tag)
{
    heap_stats_t h;
    sys_get_heap_stats(&h);
    SH_DBG("[MEMN64] %s: rdram=%u KB heap %u/%u KB used, %u KB free",
           tag ? tag : "?",
           (unsigned)(get_memory_size() / 1024),
           (unsigned)(h.used / 1024), (unsigned)(h.total / 1024),
           (unsigned)((h.total - h.used) / 1024));
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

/* THIS RETURNING 0 WEDGES THE BOOT, which is not obvious and cost a session to
 * find. mcard_xbox.c sets s_cardOk from it; with no card every _card_info
 * delivers EvSpTIMOUT, and GameState_KcetLogo_MemCardCheck loops on "rerun me
 * next frame" forever waiting for cards that will never report ready. The game
 * never reaches its title screen.
 *
 * So there is always a location. SD first, because saves there survive a power
 * cycle and can be copied off the card. The RAM device is the fallback, and on
 * an emulator it is the only one -- ares has no flashcart SD. */
int XboxFs_ResolveSaveDir(char* out, int outSize)
{
    if (!out || outSize <= 0)
        return 0;

    /* mkdir failing with EEXIST is success; any other failure means the card is
     * absent or read-only, and the probe below is what actually settles it. */
    mkdir("sd:/silenthill", 0777);
    {
        FILE* probe = fopen("sd:/silenthill/.wtest", "wb");
        if (probe != NULL)
        {
            fclose(probe);
            remove("sd:/silenthill/.wtest");
            snprintf(out, (size_t)outSize, "sd:/silenthill");
            SH_DBG("[MCRD] save location: %s (persistent)", out);
            return 1;
        }
    }

    if (SaveFs_N64Init())
    {
        snprintf(out, (size_t)outSize, "sav:");
        SH_DBG("[MCRD] save location: %s (RAM - saves LOST at power-off)", out);
        return 1;
    }

    SH_DBG("[MCRD] NO save location; the KCET-logo card check will not complete");
    return 0;
}

/* --------------------------------------------------------------- audio */

/* Audio_XboxPump is REAL now - audio_n64.c. What is still missing here is the
 * XA layer: BGM and cutscene voice are XA streams off the disc image, and no
 * decoder is wired, so these honestly do nothing yet. */
void XaPlayer_Play(int fileIdx, int channel, int loop)
{
    (void)fileIdx; (void)channel; (void)loop;
}
void XaPlayer_Stop(void) { }
/* Per-sample mix hook the SPU mixer calls for the XA stream; adds nothing
 * until a decoder exists. Signature: xa_xbox.c:433. */
void Xa_XboxMixInto(int* accL, int* accR, int* accC)
{
    (void)accL; (void)accR; (void)accC;
}
void XaPlayer_Update(void) { }
void XaPlayer_SetVolume(int vol) { (void)vol; }
int  Xa_IsVoiceAudioDraining(void) { return 0; }
void Xa_VoiceGapHold(int frames) { (void)frames; }

/* --------------------------------------------------------------- fmv */

/* Not available yet, but decided: libdragon ships an MPEG-1 decoder (mpeg2.h
 * plus RSP YUV blitting in yuv.h), so the STR files get transcoded rather than
 * a codec getting written. Returning without playing lets the game continue to
 * the scene after the movie instead of waiting on a stream that never ends.
 *
 * The signature is (int file_idx, int max_frames) -- pc_port/src/fmv/fmv_player.h.
 * An earlier version here declared it taking a const char* and printed it with
 * %s, so the FIRST movie the game reached did strlen() on a file index and
 * died. Nothing warned: the gate suppresses implicit declarations for the
 * decomp's sake, so a HAL stub whose prototype disagrees with its caller is
 * only found by running it. */
int FMV_Play(int file_idx, int max_frames)
{
    SH_DBG("[FMV] skipped (no decoder wired yet): file=%d frames=%d",
           file_idx, max_frames);
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
