/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_coop_save.h - multiplayer save files.
 *
 * Co-op has its own save namespace that mirrors single-player but with its own
 * files and custom names, so a co-op run never touches the normal memory-card
 * saves. A save is the ordinary s_Savegame blob written to
 * gamedata/coopsaves/<name>.sav behind a small header (the custom name + the
 * map, for the host's load list). Every player runs the same build, so the raw
 * blob is portable between them.
 */
#ifndef PC_COOP_SAVE_H
#define PC_COOP_SAVE_H

#ifdef __cplusplus
extern "C" {
#endif

#define COOP_SAVE_NAME_MAX 32

typedef struct
{
    char name[COOP_SAVE_NAME_MAX]; /* the custom name shown in the host list */
    char file[64];                 /* basename on disk, for load */
    int  mapIdx;
    int  saveCount;
} CoopSaveEntry;

/* Refresh the savegame from live player state and write it to <name>.sav.
 * Returns 1 on success. Refuses nothing itself -- the boss-fight gate is the
 * caller's (Pc_Save_BossActive). */
int Pc_CoopSave_Write(const char* name);

/* Read <name>.sav into g_GameWork.savegame (through g_SavegamePtr). The caller
 * then boots the map (GameBoot_PlayerInit + GameBoot_MapLoad). Returns 1 on a
 * valid file. */
int Pc_CoopSave_Load(const char* name);

/* Fill `out` with up to `max` saves found in the co-op save dir. Returns the
 * count. */
int Pc_CoopSave_List(CoopSaveEntry* out, int max);

/* 1 while a boss is alive on the map: saving is refused there, same as the
 * quick save and the original game's lack of a save point in a boss chamber.
 * Defined in pc_quicksave.c. */
int Pc_Save_BossActive(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_COOP_SAVE_H */
