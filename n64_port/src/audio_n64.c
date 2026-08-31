/*
 * audio_n64.c - audio output HAL: pumps the software SPU (audio_xbox.c) into
 * libdragon's audio interface (AI DMA out of RDRAM).
 *
 * Same division of labour as the Xbox: audio_xbox.c owns all SPU state and
 * produces PCM via Audio_RenderInto(); this file owns the hardware ring and is
 * pumped from VSync on the main thread, so the mixer's state needs no locking.
 *
 * 11025 Hz, not 48000: the mix is CPU-side C on a 93 MHz VR4300, and the rate
 * is the single biggest term in its cost. audio_xbox.c derives every pitch and
 * envelope step from OUT_HZ, so the lower rate stays time-correct.
 *
 * EXPERIMENT status (known caveats, both die with the planned RSP mixer):
 *  - VOICE_PCM_CAP is 0x800 on N64, so any sample longer than ~46 ms is
 *    truncated at key-on. Short SFX are fine; long voices/ambience cut off.
 *  - XA streams (BGM, cutscene voice) have no player yet - this is SPU only.
 */
#include <libdragon.h>

#include "pc_config.h"    /* g_PcConfig.n64AudioPumpBudgetMs */
#include "sh_log.h"

#define N64_AUDIO_HZ   11025
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
    {
        int wrote = 0, peak = 0;
        unsigned long long t0       = get_ticks();
        unsigned           budgetUs = (unsigned)g_PcConfig.n64AudioPumpBudgetMs * 1000u;

        while (audio_can_write())
        {
            short* buf = audio_write_begin();
            int    n   = audio_get_buffer_length();
            Audio_RenderInto(buf, n);
            {
                int i;
                for (i = 0; i < n * 2; i += 64)
                {
                    int v = buf[i] < 0 ? -buf[i] : buf[i];
                    if (v > peak) peak = v;
                }
            }
            audio_write_end();
            wrote++;

            /* Bounded catch-up. A slow frame drains the whole ring, so the next
             * pump refills ALL of it -- the mixer's cost per frame grows with
             * how slow the frame already was, which is a spiral, not a
             * recovery. Past the budget the rest of the ring waits for the next
             * pump: the audio gaps (it already does at this frame rate) instead
             * of the frame rate paying for audio that arrives late anyway. One
             * buffer always goes out. audio_pump_budget_ms=0 restores the
             * unbounded fill. */
            if (budgetUs != 0 &&
                (unsigned)TICKS_TO_US((unsigned)(get_ticks() - t0)) >= budgetUs)
                break;
        }

        /* TEMP diagnostic: where does silence come from - no voices keyed
         * (game/SPU side), voices keyed but zero samples (mixer side), or
         * samples present (output side / hardware AI)? */
        {
            extern int Audio_N64DiagVoices(void); /* audio_xbox.c */
            extern int g_N64KeyOnCount;
            extern int g_N64SilentKeyOns;
            static int s_sndTick = 0;
            if ((s_sndTick++ & 511) == 0)
                SH_DBG("[SNDD] wrote=%d peak=%d live=%d keyons=%d silent=%d",
                       wrote, peak, Audio_N64DiagVoices(), g_N64KeyOnCount,
                       g_N64SilentKeyOns);
        }
    }
}
