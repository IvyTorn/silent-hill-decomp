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
#include "pc_config.h"
#include "sh_net_session.h"
#include "sh_net_steam.h"
#include "sh_log.h"

#include <stdio.h>
#include <string.h>

typedef enum
{
    COOP_PAGE_ROOT = 0,
    COOP_PAGE_HOST_SETUP,
    COOP_PAGE_HOST,
    COOP_PAGE_JOIN
} CoopPage;

static int      s_open;
static CoopPage s_page;
static int      s_sel;

/* Pending host settings, seeded from config when the setup page is entered and
 * written back to config just before the lobby is opened (the worker reads them
 * there -- see ShSession_Tick's wantHost handler). */
static int s_setMaxPlayers = 4;
static int s_setPublic     = 0;
static int s_setFps        = 60;

void Pc_CoopMenu_Open(void)
{
    s_open = 1;
    s_page = COOP_PAGE_ROOT;
    s_sel  = 0;
    SH_DBG("[COOP] menu opened");
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
    s_setPublic = g_PcConfig.onlineSteamPublic ? 1 : 0;
    s_setFps    = (g_PcConfig.fpsCap >= 60) ? 60 : 30;
    s_page      = COOP_PAGE_HOST_SETUP;
    s_sel       = 0;
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
    case COOP_PAGE_HOST_SETUP: return 5; /* Players, Visibility, FPS, Start, Back */
    case COOP_PAGE_HOST:       return 3; /* Invite, Leave, Back */
    case COOP_PAGE_JOIN:       return 1; /* Back */
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
        else if (i == 3) snprintf(out, cap, "Start Hosting");
        else snprintf(out, cap, "Back");
        break;

    case COOP_PAGE_HOST:
        if (i == 0) snprintf(out, cap, "Invite Friend");
        else if (i == 1) snprintf(out, cap, "Close Lobby");
        else snprintf(out, cap, "Back");
        break;

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

    case COOP_PAGE_HOST_SETUP:
    default:
        break;
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
        /* The three setting rows cycle on confirm; no left/right needed. */
        if (s_sel == 0)      { s_setMaxPlayers = (s_setMaxPlayers >= 4) ? 2 : s_setMaxPlayers + 1; }
        else if (s_sel == 1) { s_setPublic = !s_setPublic; }
        else if (s_sel == 2) { s_setFps = (s_setFps == 60) ? 30 : 60; }
        else if (s_sel == 3) { Coop_StartHosting(); }
        else                 { s_page = COOP_PAGE_ROOT; s_sel = 0; }
        break;

    case COOP_PAGE_HOST:
        if (s_sel == 0)      { ShSession_RequestInvite(); }
        else if (s_sel == 1) { ShSession_RequestLeave(); s_page = COOP_PAGE_ROOT; s_sel = 0; }
        else                 { s_page = COOP_PAGE_ROOT; s_sel = 0; }
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
        if (s_page == COOP_PAGE_ROOT)
        {
            Pc_CoopMenu_Close();
        }
        else
        {
            s_page = COOP_PAGE_ROOT;
            s_sel  = 0;
        }
    }
}
