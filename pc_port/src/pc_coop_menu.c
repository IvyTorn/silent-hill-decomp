/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See pc_coop_menu.h. Game thread only; drawn from the main-menu draw path. */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/text/text_draw.h"

#include "pc_coop_menu.h"
#include "pc_config.h"
#include "sh_net_session.h"
#include "sh_net_steam.h"
#include "sh_log.h"

#include <stdio.h>
#include <string.h>

/* 12x16 glyph rows, centred-ish on the 320px PSX framebuffer. */
#define COOP_TITLE_Y   112
#define COOP_ROW0_Y    150
#define COOP_ROW_DY    20
#define COOP_STATUS_Y  280
#define COOP_ROW_X     100

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
static int Coop_RowCount(void)
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
        if (s_sel == 0)
        {
            s_setMaxPlayers = (s_setMaxPlayers >= 4) ? 2 : s_setMaxPlayers + 1;
        }
        else if (s_sel == 1)
        {
            s_setPublic = !s_setPublic;
        }
        else if (s_sel == 2)
        {
            s_setFps = (s_setFps == 60) ? 30 : 60;
        }
        else if (s_sel == 3)
        {
            Coop_StartHosting();
        }
        else
        {
            s_page = COOP_PAGE_ROOT;
            s_sel  = 0;
        }
        break;

    case COOP_PAGE_HOST:
        if (s_sel == 0)
        {
            ShSession_RequestInvite(); /* Steam overlay friend picker */
        }
        else if (s_sel == 1)
        {
            ShSession_RequestLeave();
            s_page = COOP_PAGE_ROOT;
            s_sel  = 0;
        }
        else
        {
            s_page = COOP_PAGE_ROOT;
            s_sel  = 0;
        }
        break;

    case COOP_PAGE_JOIN:
    default:
        s_page = COOP_PAGE_ROOT;
        s_sel  = 0;
        break;
    }
}

void Pc_CoopMenu_Update(int cancel, int up, int down, int confirm)
{
    int rows;
    if (!s_open)
    {
        return;
    }
    rows = Coop_RowCount();
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

static void Coop_DrawText(s32 x, s32 y, s16 color, const char* s)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "%s", s);
    Gfx_StringSetPosition(x, y);
    Gfx_StringSetColor(color);
    Gfx_StringDraw(buf, DEFAULT_MAP_MESSAGE_LENGTH);
}

static void Coop_DrawRow(int idx, const char* label)
{
    char buf[96];
    s32  y = COOP_ROW0_Y + idx * COOP_ROW_DY;
    if (idx == s_sel)
    {
        snprintf(buf, sizeof(buf), "[ %s ]", label);
        Coop_DrawText(COOP_ROW_X - 12, y, StringColorId_White, buf);
    }
    else
    {
        Coop_DrawText(COOP_ROW_X, y, StringColorId_LightGrey, label);
    }
}

void Pc_CoopMenu_Draw(void)
{
    char line[96];
    char status[96];

    if (!s_open)
    {
        return;
    }

    Coop_DrawText(COOP_ROW_X - 8, COOP_TITLE_Y, StringColorId_Gold, "MULTIPLAYER");

    switch (s_page)
    {
    case COOP_PAGE_ROOT:
        Coop_DrawRow(0, "HOST GAME");
        Coop_DrawRow(1, "JOIN GAME");
        Coop_DrawRow(2, "BACK");
        if (!ShSteam_Available())
        {
            Coop_DrawText(COOP_ROW_X - 40, COOP_STATUS_Y, StringColorId_Red,
                          "Steam not available - is Steam running?");
        }
        break;

    case COOP_PAGE_HOST_SETUP:
        snprintf(line, sizeof(line), "MAX PLAYERS: %d", s_setMaxPlayers);
        Coop_DrawRow(0, line);
        snprintf(line, sizeof(line), "VISIBILITY: %s", s_setPublic ? "PUBLIC" : "PRIVATE");
        Coop_DrawRow(1, line);
        snprintf(line, sizeof(line), "FPS LOCK: %d", s_setFps);
        Coop_DrawRow(2, line);
        Coop_DrawRow(3, "START HOSTING");
        Coop_DrawRow(4, "BACK");
        break;

    case COOP_PAGE_HOST:
        Coop_DrawRow(0, "INVITE FRIEND");
        Coop_DrawRow(1, "CLOSE LOBBY");
        Coop_DrawRow(2, "BACK");
        ShSession_StatusLine(status, sizeof(status));
        Coop_DrawText(COOP_ROW_X - 40, COOP_STATUS_Y, StringColorId_LightGrey, status);
        break;

    case COOP_PAGE_JOIN:
    default:
        Coop_DrawRow(0, "BACK");
        Coop_DrawText(COOP_ROW_X - 56, COOP_STATUS_Y - COOP_ROW_DY, StringColorId_LightGrey,
                      "Accept a Steam invite to join a friend.");
        ShSession_StatusLine(status, sizeof(status));
        Coop_DrawText(COOP_ROW_X - 40, COOP_STATUS_Y, StringColorId_LightGrey, status);
        break;
    }
}
