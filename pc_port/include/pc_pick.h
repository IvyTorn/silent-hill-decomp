/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_PICK_H
#define PC_PICK_H

#ifdef SH_PC_PORT

/* Console object picking: with the console open, click a character in the scene
 * to select it and right-click to clear, then run commands (SCALE) against the
 * selection. Bethesda-console style. */

typedef enum
{
    PcPick_None = 0,
    PcPick_Player,
    PcPick_Npc,
    PcPick_Prop
} e_PcPickKind;

struct _SubCharacter;
struct GsCOORDINATE2;

/** Arm a pick at a window pixel. Resolved on the next drawn frame, when the
 * view matrices the hit test needs are the ones the player is looking at. */
void Pc_Pick_RequestAt(int windowX, int windowY);

/** Drop the selection. Prints what was dropped when `announce` is set. */
void Pc_Pick_Clear(int announce);

/** The Q12 scale a character is drawn at, 4096 (1.0) when it is not scaled.
 * Collision reads its shape through this so a resized character is hittable
 * and blocked at the size you can see. Read-site only: it never writes the
 * collision fields, so it cannot compound or corrupt them. */
int Pc_Pick_CollScale(const void* chara);

/** As above, but for the MAP-collision cylinder only, which always reports 1.0
 * for the player: a scaled cylinder holds Harry that much further off every
 * wall, so he could not reach a door or fit down a corridor. His combat
 * volumes do scale -- only how he fits through the world stays vanilla. */
int Pc_Pick_MoveScale(const void* chara);

/** Scale a span measured from a character origin: origin + (v - origin)*s. */
int Pc_Pick_ScaleAbout(int origin, int v, int scaleQ12);

/** Select an NPC slot directly. Returns 0 if the slot holds no live
 * character. Used by "select nearest". */
int Pc_Pick_SelectNpc(int slot);

/** Nearest live NPC to the player, or -1. */
int Pc_Pick_NearestNpc(void);

/** Select the player without clicking, for the cameras that hide him. */
void Pc_Pick_SelectPlayer(void);

int Pc_Pick_Kind(void);
int Pc_Pick_Slot(void); /* NPC slot, or -1 for the player */

/** "npc[3] GROANER" / "player". Always NUL-terminated. */
void Pc_Pick_Describe(char* out, int outSize);

/** SCALE: q19_12 multiplier on the selection's root bone. Returns 0 when
 * nothing is selected. */
int    Pc_Pick_SetScale(int scaleQ12);
int    Pc_Pick_GetScale(void);

/** Once per frame, before a character's skeleton is drawn: applies that
 * character's console scale, and resolves a pending click against it.
 * `slot` is the NPC index, or -1 for the player. */
void Pc_Pick_CharaPreDraw(struct _SubCharacter* chara, int slot, void* boneCoords);

/** Once per frame, before a world object (prop) is drawn: applies its console
 * scale to the coord being built, and resolves a pending click against it. */
void Pc_Pick_WorldObjectPreDraw(const void* worldObject, void* coord);

/** Once per frame from the console update: prints the result of a pick that the
 * frame just resolved. */
void Pc_Pick_FrameEnd(void);

/** Forget every scale and the selection (map change / New Game). */
void Pc_Pick_Reset(void);

#endif /* SH_PC_PORT */
#endif /* PC_PICK_H */
