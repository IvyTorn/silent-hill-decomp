/*
 * audio_n64.c - audio output HAL: pumps the software SPU (audio_xbox.c) into
 * libdragon's audio interface (AI DMA out of RDRAM).
 *
 * Same division of labour as the Xbox: audio_xbox.c owns all SPU state and
 * produces PCM via Audio_RenderInto(); this file owns the hardware ring and is
 * pumped from VSync on the main thread, so the mixer's state needs no locking.
 *
 * 22050 Hz, not 48000: the mix is CPU-side C on a 93 MHz VR4300, and the rate
 * is the single biggest term in its cost. audio_xbox.c derives every pitch and
 * envelope step from OUT_HZ, so the lower rate stays time-correct.
 *
 * EXPERIMENT status (known caveats, both die with the planned RSP mixer):
 *  - VOICE_PCM_CAP is 0x800 on N64, so any sample longer than ~46 ms is
 *    truncated at key-on. Short SFX are fine; long voices/ambience cut off.
 *  - XA streams (BGM, cutscene voice) have no player yet - this is SPU only.
 */
#include <libdragon.h>

#include "sh_log.h"

#define N64_AUDIO_HZ   22050
#define N64_AUDIO_BUFS 4

void Audio_RenderInto(short* out, int frames); /* audio_xbox.c */

static int s_audioUp;

void Audio_XboxPump(void)
{
    if (!s_audioUp)
    {
        /* First VSync. Deliberately lazy: audio_init takes its ring from the
         * heap, and by now the boot allocations that matter are placed. */
        audio_init(N64_AUDIO_HZ, N64_AUDIO_BUFS);
        s_audioUp = 1;
        SH_DBG("[SND] AI up: %d Hz, %d buffers x %d samples",
               audio_get_frequency(), N64_AUDIO_BUFS, audio_get_buffer_length());
    }

    /* Fill every free ring slot. After a slow frame this mixes more than one
     * buffer in a burst; that is the correct trade - the alternative is an
     * audible underrun gap on exactly the frames that are already struggling. */
    while (audio_can_write())
    {
        short* buf = audio_write_begin();
        Audio_RenderInto(buf, audio_get_buffer_length());
        audio_write_end();
    }
}
