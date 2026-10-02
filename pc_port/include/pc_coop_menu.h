/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_coop_menu.h - the simple co-op front end (host / join).
 *
 * An overlay opened from the main menu's Multiplayer row. Like the achievement
 * browser it does NOT change g_GameWork.gameState: it draws on top of the title
 * screen and owns the pad while it is up. Host / Join drive the Steam session
 * layer (sh_net_session.h); nothing here touches the master-server living world.
 */
#ifndef PC_COOP_MENU_H
#define PC_COOP_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

void Pc_CoopMenu_Open(void);        /* main-menu popup (Host / Join) */
void Pc_CoopMenu_OpenInGame(void);  /* in-game M menu (Resume / players / leave) */
void Pc_CoopMenu_Close(void);
int  Pc_CoopMenu_IsOpen(void);
int  Pc_CoopMenu_InGame(void);      /* 1 while the in-game M menu is up */

/* One frame of input, edges already derived by the caller. */
void Pc_CoopMenu_Update(int cancel, int up, int down, int confirm);

/* Accessors for the renderer (sh_net_ui.c's Nu_DrawCoopMenu). The menu is drawn
 * with the online UI's clean panel so it matches the quick menu / achievements
 * popup, on the title screen and in game alike. */
int         Pc_CoopMenu_RowCount(void);
int         Pc_CoopMenu_Selected(void);
const char* Pc_CoopMenu_Title(void);
void        Pc_CoopMenu_RowText(int i, char* out, int cap);
void        Pc_CoopMenu_StatusText(char* out, int cap);

/* Mouse: the renderer maps the pointer to a row and drives selection/confirm. */
void        Pc_CoopMenu_SetSelected(int i);
void        Pc_CoopMenu_Confirm(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_COOP_MENU_H */
