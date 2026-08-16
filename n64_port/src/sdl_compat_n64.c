/*
 * sdl_compat_n64.c - N64 implementations of the thin SDL2 slice the decomp's
 * SH_PC_PORT code still calls.
 *
 * The SDL_* headers here are xbox_port's declaration-only shims, not real SDL.
 * Keyboard and mouse report "no input" so the shared input code falls through
 * to the pad path (pad_n64.c).
 */
#include <libdragon.h>

#include "SDL_timer.h"
#include "SDL_mouse.h"
#include "SDL_scancode.h"

/* TICKS_READ is COP0 Count: 32-bit, ticking at half the VR4300's 93.75 MHz
 * core clock, wrapping every ~91 s. get_ticks() is libdragon's 64-bit
 * extension of it, which is what these callers need -- SDL_GetTicks is
 * documented as monotonic and a raw wrap would read as a 40000-second jump. */
Uint32 SDL_GetTicks(void)
{
    return (Uint32)(get_ticks_ms() & 0xFFFFFFFFu);
}

Uint64 SDL_GetPerformanceCounter(void)   { return (Uint64)get_ticks(); }
Uint64 SDL_GetPerformanceFrequency(void) { return (Uint64)TICKS_PER_SECOND; }

void SDL_Delay(Uint32 ms)
{
    wait_ms((unsigned long)ms);
}

/* No keyboard: a permanently-zeroed state array means every
 * g_sdlKeyboardState[SDL_SCANCODE_*] read is "not pressed". */
static const unsigned char s_zeroKeys[SDL_NUM_SCANCODES] = { 0 };
const unsigned char* g_sdlKeyboardState = s_zeroKeys;

Uint32 SDL_GetMouseState(int* x, int* y)          { if (x) *x = 0; if (y) *y = 0; return 0; }
Uint32 SDL_GetRelativeMouseState(int* x, int* y)  { if (x) *x = 0; if (y) *y = 0; return 0; }
int    SDL_SetRelativeMouseMode(SDL_bool enabled) { (void)enabled; return 0; }
