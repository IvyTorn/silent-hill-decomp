/*
 * main_n64.c - N64 entry point for the Silent Hill port.
 *
 * Mirror of psp_port/src/main_psp.c, which mirrors xbox_port/src/main_xbox.c,
 * which mirrors pc_port/src/main_pc.c: bring up the HAL, initialise the
 * PSX-RAM-emulated runtime data the way the PC port does, hand control to the
 * shared MainLoop().
 *
 * game.h is deliberately NOT included. This TU pulls libdragon headers, and
 * libdragon's kernel.h collides by name with psyq's; the gate gives HAL TUs an
 * include set with libdragon first and no psyq at all, so game entry points are
 * declared extern here exactly as main_pc.c and main_psp.c do.
 *
 * Every init step announces itself on the framebuffer before it runs, not
 * after. There is no debugger on this target and a hang inside one of these
 * calls is otherwise indistinguishable from a hang in any other: what is on
 * screen when it stops names the call that did not return.
 */
#include <libdragon.h>
#include <stdio.h>
#include <string.h>

#include "sh_log.h"
#include "sh_log_n64.h"
#include "psx_memory.h"   /* PSX_ADDR, PsxMemory_Init -- includes only <stdint.h> */
#include "pc_config.h"    /* g_PcConfig: the loose-files default below */

/* --------------------------------------------------------------- game */

extern void MainLoop(void);
extern void Fs_QueueInitialize(void);
extern void ResetGraph(int mode);
extern void SetGraphDebug(int level);
extern void SpuInit(void);
extern void PcPort_InitCharaAnimInfo(void);
extern void PcPort_InitSdBuffers(void);
extern void AsRodata_Reformat(void);
extern void Fs_InitFileTableForRegion(int region);

extern void* g_OvlDynamic;
extern void* g_OvlBodyprog;

/* ---------------------------------------------------------------- hal */

extern void GpuNv2a_Init(void);      /* gpu_rdp.c -- owns display_init */
extern void Cd_N64Init(void);        /* cd_n64.c */
extern void SH_DebugLogInit(void);   /* sh_log_n64.c */
extern void SH_DebugLogFlush(void);
extern void Xbox_MemReport(const char* tag);
extern void Mcard_XboxInit(void);   /* xbox_port/src/mcard_xbox.c */
extern void PcConfig_Load(const char* path);      /* pc_port/src/pc_config.c (SDL-free) */
extern void XboxConfig_ApplyOverrides(void);      /* xbox_port/src/xbox_compat_globals.c */

/* ------------------------------------------------------------- screen */

/* Paints the whole SH_DBG ring, not just this one message. A hang during boot
 * happens BEFORE the frame loop exists, so gpu_rdp.c's per-frame painter never
 * runs and the screen freezes on whatever was last drawn -- one bare line,
 * which says where we stopped but nothing about why. The ring carries every
 * SH_DBG the subsystems emitted on the way in, which is the actual evidence. */
static void Sh_Say(const char* msg)
{
    surface_t* d;
    int        rows, i;

    if (msg != NULL)
        SH_DBG("[BOOT] %s", msg);

    d = display_try_get();
    if (d == NULL)
        return;

    graphics_fill_screen(d, graphics_make_color(0x10, 0x00, 0x14, 0xFF));
    graphics_set_color(graphics_make_color(0xC8, 0xC8, 0xD0, 0xFF), 0);

    rows = ShLogN64_Rows();
    for (i = 0; i < rows; i++)
        graphics_draw_text(d, 4, 4 + (i * 9), ShLogN64_Row(i));

    display_show(d);
}

/* --------------------------------------------------------------- data */

extern int Cd_N64SdPresent(void);

static void Sh_InitGameData(void)
{
    /* PSX memory emulation first -- everything below is g_PsxRam-relative. */
    PsxMemory_Init();

    PcPort_InitCharaAnimInfo();
    PcPort_InitSdBuffers();
    AsRodata_Reformat();

    /* Overlay base pointers into emulated PSX RAM (USA addresses, per main_pc.c). */
    g_OvlDynamic  = PSX_ADDR(0x000C9578);
    g_OvlBodyprog = PSX_ADDR(0x00024B60);
}

/* --------------------------------------------------------------- main */

static void N64_CrashBtFrame(void* arg, backtrace_frame_t* f)
{
    (void)arg;
    SH_DBG("[CRASH]   %08lx %s+0x%lx (%s:%d)%s",
           (unsigned long)f->addr, f->func ? f->func : "?",
           (unsigned long)f->func_offset, f->source_file ? f->source_file : "?",
           f->source_line, f->is_inline ? " inline" : "");
}

static void N64_CrashDump(exception_t* ex)
{
    void* bt[24];
    int   n, i;

    /* Everything addr2line needs in ONE line, first: an earlier dump's second
     * line (the one with ra) never reached the card. epc=00000000 is a call
     * through a NULL function pointer; ra names the caller. */
    SH_DBG("[CRASH] %s code=%d epc=%08lx ra=%08lx sp=%08lx fp=%08lx cr=%08lx sr=%08lx a0=%08lx v0=%08lx s0=%08lx",
           ex->info ? ex->info : "?", (int)ex->code,
           (unsigned long)ex->regs->epc, (unsigned long)(uint32_t)ex->regs->ra,
           (unsigned long)(uint32_t)ex->regs->sp, (unsigned long)(uint32_t)ex->regs->fp,
           (unsigned long)ex->regs->cr, (unsigned long)ex->regs->sr,
           (unsigned long)(uint32_t)ex->regs->a0, (unsigned long)(uint32_t)ex->regs->v0,
           (unsigned long)(uint32_t)ex->regs->s0);
    SH_DebugLogFlush();

    /* Walk the stack from inside the handler: libdragon's walker crosses its
     * own exception frame into the interrupted context (the inspector does
     * exactly this), so the frames name whoever made the bad call. Raw
     * addresses first -- they need no ROM access -- then symbolised, which
     * reads the ROM's symbol table and is best effort if the PI was busy. */
    n = backtrace(bt, 24);
    {
        char line[220];
        int  len = 0;
        line[0] = 0;
        for (i = 0; i < n && len < (int)sizeof(line) - 12; i++)
            len += snprintf(line + len, sizeof(line) - len, "%08lx ", (unsigned long)(uintptr_t)bt[i]);
        SH_DBG("[CRASH] bt: %s", line);
    }
    SH_DebugLogFlush();
    backtrace_symbols_cb(bt, n, 0, N64_CrashBtFrame, NULL);
    SH_DebugLogFlush();
    {
        /* fflush is not enough on FAT (stale directory size); commit hard so
         * the [CRASH] lines above actually survive the power cycle. */
        extern void ShLogN64_CrashCommit(void);
        ShLogN64_CrashCommit();
    }
    exception_default_handler(ex);
}

int main(void)
{
    /* isviewer and usblog both before anything else: if the very first game
     * call faults, the exception handler's backtrace is the only artefact, and
     * it only reaches us over one of these two. */
    debug_init_isviewer();
    debug_init_usblog();
    SH_DebugLogInit();

    /* Any unhandled CPU exception: name the site in the SD log (EPC/cause/
     * ra/sp), commit it, then hand over to libdragon's crash screen so the
     * TV names it too. Turns "frozen for minutes" into an address. */
    register_exception_handler(N64_CrashDump);

    /* gpu_rdp.c owns display_init: the display is the GPU backend's resource
     * and VSync() drives FrameBegin/FrameEnd through it. Two owners would mean
     * two display_get() holders and a frame that never shows. */
    GpuNv2a_Init();

    Xbox_MemReport("after HAL init");
    Sh_Say("boot");
    Sh_Say("psx ram + runtime data");
    Sh_InitGameData();

    Sh_Say("cd");
    Cd_N64Init();

    /* SD is mounted (or declared absent) by now: start mirroring the log to
     * the card so a hardware session hands back a real log instead of a
     * phone photo of the screen. */
    {
        extern void ShLogN64_EnableSdMirror(void);
        ShLogN64_EnableSdMirror();
    }
    /* Right after the mirror is up so every SD log names its ROM: the docker
     * build recompiles this TU every run, so the timestamp is unique per
     * build ("which version did I actually flash?" comes up every session). */
    SH_DBG("[SH] n64 build " __DATE__ " " __TIME__);

    /* Config, THEN the console overrides -- and calling these at all is the
     * fix for two hardware bugs at once. This port ran for weeks on raw PC
     * defaults because nothing here ever loaded a config: globalCharaPool=1
     * put the entire character roster's ILM+ANM on a 1.5MB heap (the New Game
     * black screen: pool exhausts the heap, chunk buffers starve, the load
     * waits forever), preloadChunks=1 made map init try to fund 256 chunk
     * slots, and widescreenMode=1 (Hor+) laid the 2D screens out for a wider
     * virtual display -- on a 4:3 320-wide screen that is text shifted right.
     * The Xbox override set pins all of these to what a console can afford;
     * the cfg file on SD stays the user's tuning hook, applied first so the
     * overrides win only where they must. main_psp.c has always done this;
     * omitting it here was the mistake. */
    Sh_Say("config");
    /* Loose files ON by default here (the PC defaults it off): the asset
     * pipeline's output is a loose tree under sd:/silenthill/gamedata/load/,
     * and a user who has not extracted anything just pays a handful of fopen
     * misses per file load. The cfg (allow_loose_files=0) can turn it off. */
    g_PcConfig.allowLooseFiles = 1;
    PcConfig_Load("sd:/silenthill/silenthill.cfg");
    XboxConfig_ApplyOverrides();
    /* Emulator smoke runs (no SD card = never a console) start New Game on
     * the exterior behind the police-station door: it exercises a DSO load,
     * four active cells in the world chunk pool, and fog, with no human. */
    if (!Cd_N64SdPresent())
        strcpy(g_PcConfig.mapName, "map2_s02");

    /* PSX kernel events + memory card. NOT optional and not obvious: it is what
     * resolves the save location, and without it mcard_xbox.c reports no card,
     * every _card_info delivers EvSpTIMOUT, and GameState_KcetLogo's
     * MemCardCheck loops on "rerun me next frame" forever. The boot wedges on
     * the KCET logo and never reaches the title screen. main_psp.c calls this;
     * omitting it here cost a session. */
    Sh_Say("memory card");
    Mcard_XboxInit();

    Sh_Say("file table (USA)");
    Fs_InitFileTableForRegion(0 /* Region_USA */);

    Sh_Say("spu");
    SpuInit();

    Sh_Say("graph");
    ResetGraph(0);
    SetGraphDebug(0);

    Sh_Say("fs queue");
    Fs_QueueInitialize();

    {
        extern void MapDso_SelfTest(void);
        if (!Cd_N64SdPresent())
            MapDso_SelfTest();
    }

    Xbox_MemReport("before MainLoop");
    Sh_Say("entering MainLoop");
    MainLoop();

    /* MainLoop does not return on any other port. If it does here, say so and
     * stop rather than falling off the end into libdragon's exit path, which
     * on a console is an unhelpful black screen. */
    Sh_Say("MainLoop returned (unexpected)");
    while (1)
        wait_ms(100);
}
