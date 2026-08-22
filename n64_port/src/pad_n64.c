/*
 * pad_n64.c - PSX controller (libpad) on N64, backed by libdragon's joypad.
 *
 * The game calls PadInitDirect(&g_GameWork.rawController, ...) once, then reads
 * that raw PSX pad buffer every frame (Joy_ReadP1 -> Joy_ControllerDataUpdate,
 * which does heldBtnFlags = ~digitalButtons). PSX buttons are ACTIVE-LOW
 * (0 = pressed). We capture that buffer pointer and refresh it each frame from
 * Pad_Poll, called out of VSync.
 *
 * PSX s_AnalogController layout (include/bodyprog/sys/joy.h):
 *   [0] status        : 0x00 connected, 0xFF disconnected
 *   [1] recv:4|term:4 : 0x41 = 16-button digital pad
 *   [2..3] digitalButtons (u16, active-low)
 *   [4..7] rightX, rightY, leftX, leftY (0x80 = neutral)
 *
 * The N64 pad is the awkward one of every target so far: 14 buttons against the
 * PSX's 16, one analog stick, and a C-pad that is four digital buttons rather
 * than a second stick. The mapping below is a starting point, not a settled
 * scheme -- see the note at Pad_Poll.
 */
#include <libdragon.h>

#include <string.h>

#include "sh_log.h"

/* PSX digital-button bit positions, active-low. */
#define PSXB_SELECT   0
#define PSXB_L3       1
#define PSXB_R3       2
#define PSXB_START    3
#define PSXB_UP       4
#define PSXB_RIGHT    5
#define PSXB_DOWN     6
#define PSXB_LEFT     7
#define PSXB_L2       8
#define PSXB_R2       9
#define PSXB_L1      10
#define PSXB_R1      11
#define PSXB_TRIANGLE 12
#define PSXB_CIRCLE  13
#define PSXB_CROSS   14
#define PSXB_SQUARE  15

static unsigned char* s_padBuf;

/* The N64 stick reads roughly +/-80 at full throw, not +/-128, and a worn
 * stick rests off-centre often enough that a naive threshold walks the player
 * into walls on a fresh boot. 20/80 swallows that and stays well inside the
 * throw. */
#define STICK_DEADZONE 20

static void Pad_FillIdle(unsigned char* b)
{
    b[0] = 0x00;                          /* connected */
    b[1] = 0x41;                          /* digital pad */
    b[2] = 0xFF; b[3] = 0xFF;             /* all buttons released (active-low) */
    b[4] = 0x80; b[5] = 0x80;             /* right stick neutral */
    b[6] = 0x80; b[7] = 0x80;             /* left stick neutral */
}

void Pad_N64Init(void)
{
    joypad_init();
    SH_DBG("[PAD] joypad ready");
}

void PadInitDirect(void* pad1, void* pad2)
{
    (void)pad2;                           /* single-controller machine */
    s_padBuf = (unsigned char*)pad1;
    if (s_padBuf)
        Pad_FillIdle(s_padBuf);
    Pad_N64Init();
}

void PadStartCom(void) { }

void Pad_Poll(void)
{
    joypad_buttons_t   b;
    joypad_inputs_t    in;
    unsigned short     psx = 0xFFFF;
    int                sx, sy;

    if (!s_padBuf)
        return;

/* 1 = the emulator drives a New Game itself (no human input path in ares).
 * MUST be 0 for hardware: the injector mashes START/CROSS forever, which
 * in-game means constant pausing and dialogue skipping. */
/* 2 = press START only (reach the title menu and LINGER there - the menu
 * screenshot mode); 1 = full New Game drive; 0 = hardware (humans only). */
#define SH_N64_AUTOSTART 2
#if SH_N64_AUTOSTART
    /* TEMP diagnostic: drive a real New Game with no human. The emulator runs
     * ~7 game-frames a second, so from ~100 s in, mash START then CROSS on a
     * cycle -- that walks PUSH-START -> menu (cursor sits on START with no
     * saves) -> confirm -> difficulty confirm. Removes the hardware round-trip
     * from reproducing the map-load hang. */
    {
        static unsigned s_poll = 0;
        unsigned        phase;
        s_poll++;
        /* Watchdog: name the last MainLoop site every ~9s of polls. A hang
         * that spin-waits on VSync keeps this printing its own tag; a hang
         * that does not freezes the log entirely, which is its own answer. */
        if ((s_poll & 63) == 0)
        {
            extern const char* g_MlTraceTag;
            extern void ShLogN64_PushWrapped(const char* line);
            char ln[56];
            int  di = 0, k;
            const char* pre = "[MLWD] ";
            while (*pre) ln[di++] = *pre++;
            for (k = 0; g_MlTraceTag[k] && di < 54; k++) ln[di++] = g_MlTraceTag[k];
            ln[di] = 0;
            ShLogN64_PushWrapped(ln);
        }
        if (s_poll > 700)
        {
            phase = s_poll % 20;
            /* Byte split per the main path below: buf[2] = bits 8..15,
             * buf[3] = bits 0..7. START is bit 3 (LOW byte), CROSS bit 14
             * (HIGH byte) -- the first version had them swapped, plus a
             * negative shift. */
            if (phase < 2)
            {
                s_padBuf[0] = 0x00; s_padBuf[1] = 0x41;
                s_padBuf[2] = 0xFF;
                s_padBuf[3] = (unsigned char)~(1u << PSXB_START);        /* 0xF7 */
                s_padBuf[4] = s_padBuf[5] = s_padBuf[6] = s_padBuf[7] = 0x80;
                return;
            }
            if (SH_N64_AUTOSTART == 2)
            {
                /* Menu-linger mode: no CROSS ever, so the cursor never
                 * confirms and the title menu stays up for the capture. */
            }
            else if (phase >= 10 && phase < 12)
            {
                s_padBuf[0] = 0x00; s_padBuf[1] = 0x41;
                s_padBuf[2] = (unsigned char)~(1u << (PSXB_CROSS - 8));  /* 0xBF */
                s_padBuf[3] = 0xFF;
                s_padBuf[4] = s_padBuf[5] = s_padBuf[6] = s_padBuf[7] = 0x80;
                return;
            }
        }
    }
#endif

    joypad_poll();

    if (!joypad_is_connected(JOYPAD_PORT_1))
    {
        Pad_FillIdle(s_padBuf);
        s_padBuf[0] = 0xFF;               /* disconnected */
        return;
    }

    in = joypad_get_inputs(JOYPAD_PORT_1);
    b  = in.btn;

    #define PRESS(bit) (psx &= (unsigned short)~(1u << (bit)))

    if (b.d_up)    PRESS(PSXB_UP);
    if (b.d_down)  PRESS(PSXB_DOWN);
    if (b.d_left)  PRESS(PSXB_LEFT);
    if (b.d_right) PRESS(PSXB_RIGHT);
    if (b.start)   PRESS(PSXB_START);

    /* Face buttons. A/B are the confirm/cancel pair, so they take Cross/Circle.
     * The N64 has no third and fourth face button, so Square (run/attack) and
     * Triangle (menu) go to the C-pad, which is where the OoT/RE2 conventions
     * put secondary actions and is the closest thing to muscle memory a player
     * arrives with. */
    if (b.a)       PRESS(PSXB_CROSS);
    if (b.b)       PRESS(PSXB_CIRCLE);
    if (b.c_down)  PRESS(PSXB_SQUARE);
    if (b.c_up)    PRESS(PSXB_TRIANGLE);

    /* Four PSX shoulders, two N64 ones, plus Z. Aim (R2) is the one the game
     * cannot be finished without, so it takes Z -- the trigger the hand is
     * already on. L keeps L1 (sidestep). R1 and L2 land on the remaining C
     * buttons rather than going unmapped; Select takes C-right because the map
     * screen is reached often enough to deserve a button.
     *
     * This is a first pass. The port's per-scheme binding layer is where this
     * should end up configurable, and the alt-camera schemes will want the
     * C-pad back for free aim. */
    if (b.l)       PRESS(PSXB_L1);
    if (b.z)       PRESS(PSXB_R2);
    if (b.r)       PRESS(PSXB_R1);
    if (b.c_left)  PRESS(PSXB_L2);
    if (b.c_right) PRESS(PSXB_SELECT);

    /* Analog stick -> d-pad directions, ORed with the real d-pad exactly as the
     * Xbox and PSP ports do, so the tank scheme works from either. */
    sx = in.stick_x;
    sy = in.stick_y;
    if (sx < -STICK_DEADZONE) PRESS(PSXB_LEFT);
    if (sx >  STICK_DEADZONE) PRESS(PSXB_RIGHT);
    if (sy >  STICK_DEADZONE) PRESS(PSXB_UP);     /* N64 Y is up-positive */
    if (sy < -STICK_DEADZONE) PRESS(PSXB_DOWN);

    #undef PRESS

    s_padBuf[0] = 0x00;
    s_padBuf[1] = 0x41;
    s_padBuf[2] = (unsigned char)(psx >> 8);
    s_padBuf[3] = (unsigned char)(psx & 0xFF);
    s_padBuf[4] = 0x80;
    s_padBuf[5] = 0x80;
    /* Reported on the LEFT stick pair as well as through the d-pad bits above:
     * the alt-camera schemes read the raw axes for free aim. Rescaled from the
     * N64's ~+/-80 throw to the PSX's 0x00..0xFF, and Y is inverted because the
     * PSX reports down-positive. */
    s_padBuf[6] = (unsigned char)(128 + ((sx * 127) / 80 > 127 ? 127 :
                                        ((sx * 127) / 80 < -128 ? -128 : (sx * 127) / 80)));
    s_padBuf[7] = (unsigned char)(128 - ((sy * 127) / 80 > 127 ? 127 :
                                        ((sy * 127) / 80 < -128 ? -128 : (sy * 127) / 80)));
}

int  PadGetState(int port)                          { (void)port; return 6; }   /* 6 = stable */
int  PadInfoMode(int socket, int term, int offs)    { (void)socket; (void)term; (void)offs; return 7; }
int  PadSetMainMode(int socket, int offs, int lock) { (void)socket; (void)offs; (void)lock; return 0; }
int  PadSetActAlign(int socket, unsigned char* tbl) { (void)socket; (void)tbl; return 1; }
int  PadChkVsync(void)                              { return 1; }

/* Rumble Pak. Honest "not available" rather than a silent no-op: the N64's
 * rumble is a pak in the memory slot, and the slot is where the Controller Pak
 * that holds saves also goes. Wiring rumble means choosing which one the player
 * gets, and that is a decision to make with the save system, not before it. */
void PadSetAct(int socket, unsigned char* table, int len)
{
    (void)socket; (void)table; (void)len;
}

/* Xbox's black/white buttons. No N64 equivalent and nothing to fake. */
int Pad_XboxBlackWhite(int* black, int* white)
{
    if (black) *black = 0;
    if (white) *white = 0;
    return 0;
}
