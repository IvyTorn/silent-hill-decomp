/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_coop_menu.c - the simple co-op front end (state + logic only).
 *
 * Rendering lives in sh_net_ui.c (Nu_DrawCoopMenu), so this menu shares the
 * clean panel look of the rest of the online UI and draws from the same
 * post-capture hook on the title screen and in game alike. This file only holds
 * the page/selection state, services input, and exposes accessors the renderer
 * reads. Host / Join drive the Steam session layer (sh_net_session.h).
 */

#include "game.h"
#include "bodyprog/bodyprog.h"

#include "pc_coop_menu.h"
#include "pc_coop_save.h"
#include "pc_config.h"
#include "sh_net_session.h"
#include "sh_net_steam.h"
#include "sh_log.h"

#include <stdio.h>
#include <string.h>

typedef enum
{
    COOP_PAGE_ROOT = 0,     /* main-menu: Host / Join / Back */
    COOP_PAGE_HOST_SETUP,
    COOP_PAGE_HOST,
    COOP_PAGE_JOIN,
    COOP_PAGE_INGAME,       /* in-game M menu: Resume / prefs / players / leave */
    COOP_PAGE_PLAYERS       /* member list (in-game) */
} CoopPage;

static int      s_open;
static int      s_inGame;   /* 1 = opened in-game (M menu), 0 = main-menu popup */
static CoopPage s_page;
static int      s_sel;
static char     s_msg[64];  /* transient feedback shown on the in-game status line */

/* 1 while the CURRENT game was launched as multiplayer (host or join via the
 * Multiplayer menu). This -- not a config flag -- is what turns on the in-game
 * co-op features (the M menu, no pausing). A normal New Game / Continue leaves
 * it 0, so single-player is identical to the stock port. title.c sets it at the
 * boot and clears it at the menu. */
int g_PcCoopGame;

/* Pending host settings, seeded from config when the setup page is entered and
 * written back to config just before the lobby is opened (the worker reads them
 * there -- see ShSession_Tick's wantHost handler). */
static int s_setMaxPlayers = 4;
static int s_setPublic     = 0;
static int s_setFps        = 60;

/* Host-setup save picker: the co-op saves on disk and which one to start from
 * (-1 = a fresh New Game). */
static CoopSaveEntry s_saves[8];
static int           s_saveCount;
static int           s_loadIdx = -1;

/* Start-game request handed to title.c (GameState_MainMenu_Update), which does
 * the actual boot. 0 = none, 1 = new game, 2 = load s_startSave. */
static int  s_startReq;
static char s_startSave[COOP_SAVE_NAME_MAX];

void Pc_CoopMenu_Open(void)
{
    s_open   = 1;
    s_inGame = 0;
    s_page   = COOP_PAGE_ROOT;
    s_sel    = 0;
    SH_DBG("[COOP] menu opened");
}

void Pc_CoopMenu_OpenInGame(void)
{
    s_open   = 1;
    s_inGame = 1;
    s_page   = COOP_PAGE_INGAME;
    s_sel    = 0;
    s_msg[0] = '\0';
    SH_DBG("[COOP] in-game M menu opened");
}

int Pc_CoopMenu_InGame(void)
{
    return s_open && s_inGame;
}

void Pc_CoopMenu_Close(void)
{
    s_open = 0;
}

int Pc_CoopMenu_IsOpen(void)
{
    return s_open;
}

static void Coop_EnterHostSetup(void)
{
    s_setMaxPlayers = g_PcConfig.onlineSteamMaxPlayers;
    if (s_setMaxPlayers < 2) s_setMaxPlayers = 2;
    if (s_setMaxPlayers > 4) s_setMaxPlayers = 4;
    s_setPublic   = g_PcConfig.onlineSteamPublic ? 1 : 0;
    s_setFps      = (g_PcConfig.fpsCap >= 60) ? 60 : 30;
    s_saveCount   = Pc_CoopSave_List(s_saves, (int)(sizeof(s_saves) / sizeof(s_saves[0])));
    s_loadIdx     = -1; /* default: start a fresh game */
    s_page        = COOP_PAGE_HOST_SETUP;
    s_sel         = 0;
}

/* Queue the game boot for title.c. Reads the host-setup load choice. */
static void Coop_RequestStart(void)
{
    if (s_loadIdx >= 0 && s_loadIdx < s_saveCount)
    {
        s_startReq = 2;
        snprintf(s_startSave, sizeof(s_startSave), "%s", s_saves[s_loadIdx].name);
    }
    else
    {
        s_startReq      = 1;
        s_startSave[0]  = '\0';
    }
    Pc_CoopMenu_Close();
}

int Pc_CoopMenu_TakeStartRequest(char* outName, int cap)
{
    int req = s_startReq;
    s_startReq = 0;
    if (outName && cap > 0)
    {
        snprintf(outName, cap, "%s", s_startSave);
    }
    return req;
}

static void Coop_StartHosting(void)
{
    /* The worker reads these out of config when it creates the lobby. */
    g_PcConfig.onlineSteamMaxPlayers = s_setMaxPlayers;
    g_PcConfig.onlineSteamPublic     = s_setPublic;
    g_PcConfig.fpsCap                = s_setFps;
    SH_DBG("[COOP] host: %d players, %s, fps %d",
           s_setMaxPlayers, s_setPublic ? "public" : "private", s_setFps);
    ShSession_RequestHost();
    s_page = COOP_PAGE_HOST;
    s_sel  = 0;
}

/* Rows on the current page; the last row is always Back. */
int Pc_CoopMenu_RowCount(void)
{
    switch (s_page)
    {
    case COOP_PAGE_ROOT:       return 3; /* Host, Join, Back */
    case COOP_PAGE_HOST_SETUP: return 6; /* Players, Visibility, FPS, Load, Open Lobby, Back */
    case COOP_PAGE_HOST:       return 4; /* Invite, Start Game, Leave, Back */
    case COOP_PAGE_JOIN:       return 1; /* Back */
    case COOP_PAGE_INGAME:     return 6; /* Resume, Save, Save&Exit, Nameplates, Players, Leave */
    case COOP_PAGE_PLAYERS:    return ShSession_MemberCount() + 1; /* members + Back */
    default:                   return 1;
    }
}

int Pc_CoopMenu_Selected(void)
{
    return s_sel;
}

const char* Pc_CoopMenu_Title(void)
{
    switch (s_page)
    {
    case COOP_PAGE_HOST_SETUP: return "HOST GAME";
    case COOP_PAGE_HOST:       return "LOBBY";
    case COOP_PAGE_JOIN:       return "JOIN GAME";
    case COOP_PAGE_INGAME:     return "MULTIPLAYER";
    case COOP_PAGE_PLAYERS:    return "PLAYERS";
    default:                   return "MULTIPLAYER";
    }
}

void Pc_CoopMenu_RowText(int i, char* out, int cap)
{
    if (!out || cap <= 0)
    {
        return;
    }
    out[0] = '\0';
    switch (s_page)
    {
    case COOP_PAGE_ROOT:
        if (i == 0) snprintf(out, cap, "Host Game");
        else if (i == 1) snprintf(out, cap, "Join Game");
        else snprintf(out, cap, "Back");
        break;

    case COOP_PAGE_HOST_SETUP:
        if (i == 0) snprintf(out, cap, "Max Players:  %d", s_setMaxPlayers);
        else if (i == 1) snprintf(out, cap, "Visibility:  %s", s_setPublic ? "Public" : "Private");
        else if (i == 2) snprintf(out, cap, "FPS Lock:  %d", s_setFps);
        else if (i == 3)
        {
            if (s_loadIdx >= 0 && s_loadIdx < s_saveCount)
                snprintf(out, cap, "Load:  %s", s_saves[s_loadIdx].name);
            else
                snprintf(out, cap, "Load:  New Game");
        }
        else if (i == 4) snprintf(out, cap, "Open Lobby");
        else snprintf(out, cap, "Back");
        break;

    case COOP_PAGE_HOST:
        if (i == 0) snprintf(out, cap, "Invite Friend");
        else if (i == 1) snprintf(out, cap, "Start Game");
        else if (i == 2) snprintf(out, cap, "Close Lobby");
        else snprintf(out, cap, "Back");
        break;

    case COOP_PAGE_INGAME:
        if (i == 0) snprintf(out, cap, "Resume");
        else if (i == 1) snprintf(out, cap, "Save");
        else if (i == 2) snprintf(out, cap, "Save & Exit");
        else if (i == 3) snprintf(out, cap, "Nameplates:  %s",
                                  g_PcConfig.onlineNameplates ? "On" : "Off");
        else if (i == 4) snprintf(out, cap, "Players");
        else snprintf(out, cap, "Leave to Title");
        break;

    case COOP_PAGE_PLAYERS:
    {
        int mc = ShSession_MemberCount();
        if (i < mc)
        {
            const ShSessionMember* m = ShSession_Member(i);
            if (m && m->pingMs >= 0)
                snprintf(out, cap, "%s  (%dms)", m->name, m->pingMs);
            else if (m)
                snprintf(out, cap, "%s", m->name);
        }
        else
        {
            snprintf(out, cap, "Back");
        }
        break;
    }

    case COOP_PAGE_JOIN:
    default:
        snprintf(out, cap, "Back");
        break;
    }
}

void Pc_CoopMenu_StatusText(char* out, int cap)
{
    if (!out || cap <= 0)
    {
        return;
    }
    out[0] = '\0';
    switch (s_page)
    {
    case COOP_PAGE_ROOT:
        if (!ShSteam_Available())
        {
            snprintf(out, cap, "Steam not running - start Steam and relaunch.");
        }
        break;

    case COOP_PAGE_HOST:
        ShSession_StatusLine(out, cap);
        break;

    case COOP_PAGE_JOIN:
        snprintf(out, cap, "Accept a Steam invite from a friend to join.");
        break;

    case COOP_PAGE_INGAME:
        if (s_msg[0]) { snprintf(out, cap, "%s", s_msg); }
        else          { ShSession_StatusLine(out, cap); }
        break;

    case COOP_PAGE_HOST_SETUP:
    case COOP_PAGE_PLAYERS:
    default:
        break;
    }
}

/* Write the co-op save (named after the player), refused during a boss fight.
 * andExit also disconnects and returns to the title. Feedback goes to s_msg. */
static void Coop_DoSave(int andExit)
{
    const char* nm = (g_PcConfig.onlineName[0]) ? g_PcConfig.onlineName : "coop";

    if (Pc_Save_BossActive())
    {
        snprintf(s_msg, sizeof(s_msg), "Can't save during a boss fight");
        return;
    }
    if (!Pc_CoopSave_Write(nm))
    {
        snprintf(s_msg, sizeof(s_msg), "Save failed");
        return;
    }
    snprintf(s_msg, sizeof(s_msg), "Saved as \"%s\"", nm);
    if (andExit)
    {
        ShSession_RequestLeave();
        g_SysWork.sysFlags |= SysFlag_DoWarmReset;
        Pc_CoopMenu_Close();
    }
}

static void Coop_Confirm(void)
{
    switch (s_page)
    {
    case COOP_PAGE_ROOT:
        if (s_sel == 0)      { Coop_EnterHostSetup(); }
        else if (s_sel == 1) { s_page = COOP_PAGE_JOIN; s_sel = 0; }
        else                 { Pc_CoopMenu_Close(); }
        break;

    case COOP_PAGE_HOST_SETUP:
        /* Setting rows cycle on confirm; no left/right needed. */
        if (s_sel == 0)      { s_setMaxPlayers = (s_setMaxPlayers >= 4) ? 2 : s_setMaxPlayers + 1; }
        else if (s_sel == 1) { s_setPublic = !s_setPublic; }
        else if (s_sel == 2) { s_setFps = (s_setFps == 60) ? 30 : 60; }
        else if (s_sel == 3) { s_loadIdx = (s_loadIdx + 1 >= s_saveCount) ? -1 : s_loadIdx + 1; }
        else if (s_sel == 4) { Coop_StartHosting(); } /* Open Lobby */
        else                 { s_page = COOP_PAGE_ROOT; s_sel = 0; }
        break;

    case COOP_PAGE_HOST:
        if (s_sel == 0)      { ShSession_RequestInvite(); }
        else if (s_sel == 1) { Coop_RequestStart(); }                       /* Start Game */
        else if (s_sel == 2) { ShSession_RequestLeave(); s_page = COOP_PAGE_ROOT; s_sel = 0; }
        else                 { s_page = COOP_PAGE_ROOT; s_sel = 0; }
        break;

    case COOP_PAGE_INGAME:
        if (s_sel == 0)      { Pc_CoopMenu_Close(); }                       /* Resume */
        else if (s_sel == 1) { Coop_DoSave(0); }                            /* Save */
        else if (s_sel == 2) { Coop_DoSave(1); }                            /* Save & Exit */
        else if (s_sel == 3) { g_PcConfig.onlineNameplates = !g_PcConfig.onlineNameplates; }
        else if (s_sel == 4) { s_page = COOP_PAGE_PLAYERS; s_sel = 0; }     /* Players */
        else
        {
            /* Leave to Title without saving. */
            ShSession_RequestLeave();
            g_SysWork.sysFlags |= SysFlag_DoWarmReset;
            Pc_CoopMenu_Close();
        }
        break;

    case COOP_PAGE_PLAYERS:
        /* Rows above Back are members; host-kick lands with the session kick
         * API. Back returns to the in-game root (the Players row). */
        s_page = COOP_PAGE_INGAME;
        s_sel  = 4;
        break;

    case COOP_PAGE_JOIN:
    default:
        s_page = COOP_PAGE_ROOT;
        s_sel  = 0;
        break;
    }
}

/* Mouse hover drives the selection; a click selects then confirms. The renderer
 * (sh_net_ui.c) owns the row geometry, so it maps the pointer to a row and calls
 * these. */
void Pc_CoopMenu_SetSelected(int i)
{
    if (s_open && i >= 0 && i < Pc_CoopMenu_RowCount())
    {
        s_sel = i;
    }
}

void Pc_CoopMenu_Confirm(void)
{
    if (s_open)
    {
        Coop_Confirm();
    }
}

void Pc_CoopMenu_Update(int cancel, int up, int down, int confirm)
{
    int rows;
    if (!s_open)
    {
        return;
    }
    rows = Pc_CoopMenu_RowCount();
    if (up)
    {
        s_sel = (s_sel + rows - 1) % rows;
    }
    if (down)
    {
        s_sel = (s_sel + 1) % rows;
    }
    if (confirm)
    {
        Coop_Confirm();
        return;
    }
    if (cancel)
    {
        if (s_page == COOP_PAGE_PLAYERS)
        {
            s_page = COOP_PAGE_INGAME; /* back out of the member list */
            s_sel  = 4;
        }
        else if (s_page == COOP_PAGE_ROOT || s_page == COOP_PAGE_INGAME)
        {
            Pc_CoopMenu_Close(); /* top level: close / resume */
        }
        else
        {
            s_page = s_inGame ? COOP_PAGE_INGAME : COOP_PAGE_ROOT;
            s_sel  = 0;
        }
    }
}
