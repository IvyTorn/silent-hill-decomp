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

#include "psx_memory.h"   /* PSX_ADDR, PsxMemory_Init -- includes only <stdint.h> */

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

/* ------------------------------------------------------------- screen */

static int s_line = 0;

static void Sh_Say(const char* msg)
{
    surface_t* d;

    debugf("%s\n", msg);

    d = display_try_get();
    if (d == NULL)
        return;

    graphics_fill_screen(d, graphics_make_color(0x10, 0x00, 0x14, 0xFF));
    graphics_set_color(graphics_make_color(0xC8, 0xC8, 0xD0, 0xFF), 0);
    graphics_draw_text(d, 8, 8, "SILENT HILL / N64");
    graphics_draw_text(d, 8, 24 + (s_line * 10), msg);
    display_show(d);

    if (++s_line > 18)
        s_line = 0;
}

/* --------------------------------------------------------------- data */

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

int main(void)
{
    /* isviewer and usblog both before anything else: if the very first game
     * call faults, the exception handler's backtrace is the only artefact, and
     * it only reaches us over one of these two. */
    debug_init_isviewer();
    debug_init_usblog();
    SH_DebugLogInit();

    /* gpu_rdp.c owns display_init: the display is the GPU backend's resource
     * and VSync() drives FrameBegin/FrameEnd through it. Two owners would mean
     * two display_get() holders and a frame that never shows. */
    GpuNv2a_Init();

    Sh_Say("boot");
    Sh_Say("psx ram + runtime data");
    Sh_InitGameData();

    Sh_Say("cd");
    Cd_N64Init();

    Sh_Say("file table (USA)");
    Fs_InitFileTableForRegion(0 /* Region_USA */);

    Sh_Say("spu");
    SpuInit();

    Sh_Say("graph");
    ResetGraph(0);
    SetGraphDebug(0);

    Sh_Say("fs queue");
    Fs_QueueInitialize();

    Sh_Say("entering MainLoop");
    MainLoop();

    /* MainLoop does not return on any other port. If it does here, say so and
     * stop rather than falling off the end into libdragon's exit path, which
     * on a console is an unhelpful black screen. */
    Sh_Say("MainLoop returned (unexpected)");
    while (1)
        wait_ms(100);
}
