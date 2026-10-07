/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Android: the inventory on a secondary display (AYN Thor).
 *
 * The game thread copies the few fields the second screen shows into a flat
 * little-endian blob once per frame; the Java side (SecondScreen.java) polls
 * it from the UI thread and draws it with ordinary Android views. The renderer
 * is not involved -- the second display is a plain Android window, not a GL
 * surface.
 *
 * Touch goes the other way, and by the same rule the mouse layer follows
 * (pc_inventory_mouse.c): nothing here uses, equips or reloads anything. A tap
 * opens the game's OWN inventory screen -- which is also what pauses the world
 * -- walks its carousel to the item and presses the buttons the stock code
 * already reads. Every effect, sound, animation and refusal is Inventory_Logic's.
 *
 * The two threads never touch each other's data: the blob is rebuilt into a
 * local buffer, compared against the published one, and swapped under a mutex
 * only when it differs. A serial number lets the poll return nothing at all
 * when the picture has not changed, which is nearly every poll.
 */
#include "game.h"

#include "bodyprog/bodyprog.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/items.h"
#include "bodyprog/savegame.h"
#include "bodyprog/sys/joy.h"

#include <string.h>

#if defined(__ANDROID__)

#include <jni.h>
#include <pthread.h>
#include <android/log.h>
#include <SDL.h>

#define SS_TAG       "SH2Screen"
#define SS_VERSION   2
#define SS_NAME_MAX  48
#define SS_HEADER    24
#define SS_BLOB_MAX  (SS_HEADER + INV_ITEM_COUNT_MAX * (4 + SS_NAME_MAX))

/* Header flag bits. */
#define SS_FLAG_SESSION     (1 << 0) /* a game is loaded; the fields below mean something */
#define SS_FLAG_FLASHLIGHT  (1 << 1)
#define SS_FLAG_RADIO       (1 << 2)

/* Defined next to INVENTORY_ITEM_NAMES (item_screens_3.c). */
extern const char* Pc_Inventory_ItemName(u8 id);

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char   s_pub[SS_BLOB_MAX];
static int             s_pubLen;
static int             s_serial;
static int             s_session;

/* ---- Touch: what the second screen asked for, and where the answer is. ---- */

/* Requests, UI thread -> game thread (nativeInput). */
enum { RQ_NONE = 0, RQ_SELECT, RQ_CHOOSE, RQ_CANCEL, RQ_DISMISS };

/* Driver states, published in the header so the second screen can follow. */
enum
{
    UI_IDLE = 0,
    UI_OPENING,  /* asking gameplay for the inventory */
    UI_SEEK,     /* inventory up, walking the carousel to the item */
    UI_MENU,     /* on the item; waiting for the player to pick a command */
    UI_OPEN_CMD, /* opening the stock command submenu */
    UI_CONFIRM,  /* pressing the chosen row */
    UI_BUSY,     /* the stock code is doing it */
    UI_VIEWING,  /* an examined item is on the main screen */
    UI_SETTLE,   /* done; leave the result up for a moment */
    UI_CLOSING   /* leaving the inventory */
};

/* Why the last request ended without doing anything. */
enum { EV_NONE = 0, EV_UNAVAILABLE, EV_CANT_USE, EV_TOO_DARK, EV_TIMEOUT };

#define UI_OPEN_TIMEOUT_MS  1500
#define UI_STEP_TIMEOUT_MS  10000
#define UI_SETTLE_OK_MS     350
#define UI_SETTLE_MSG_MS    1800

static int    s_rqKind;
static int    s_rqA;
static int    s_rqB;
static Uint32 s_rqAt;

static struct
{
    int    state;
    int    slot;       /* inventory index the request is about */
    int    itemId;     /* what was in that slot when it was tapped */
    int    row;        /* chosen command row */
    int    openedByUs; /* we opened the inventory, so we close it again */
    int    pressed;    /* UI_CONFIRM: the command press has been issued */
    int    eventSeq;   /* bumped on every refusal, so repeats are seen */
    int    eventCode;
    Uint32 since;      /* SDL ticks when the current state began */
} s_ui;

static void Ui_Set(int state)
{
    s_ui.state   = state;
    s_ui.pressed = 0;
    s_ui.since   = SDL_GetTicks();
}

static void Ui_Fail(int code)
{
    s_ui.eventCode = code;
    s_ui.eventSeq  = (s_ui.eventSeq + 1) & 0xFF;
    __android_log_print(ANDROID_LOG_INFO, SS_TAG, "touch: gave up in state %d (reason %d) slot=%d item=%d",
                        s_ui.state, code, s_ui.slot, s_ui.itemId);
}

static Uint32 Ui_Elapsed(void)
{
    return SDL_GetTicks() - s_ui.since;
}

static int Ui_TakeRequest(int* a, int* b)
{
    int kind;

    pthread_mutex_lock(&s_lock);
    kind     = s_rqKind;
    *a       = s_rqA;
    *b       = s_rqB;
    s_rqKind = RQ_NONE;
    pthread_mutex_unlock(&s_lock);
    return kind;
}

static void Ui_Press(u32 bits)
{
    g_Controller0->clickedBtnFlags |= bits;
    g_Controller0->heldBtnFlags    |= bits;
}

/* Does the slot still hold what the player tapped? The list they tapped is up
 * to a poll old, and a pickup or a used-up item can have moved things. */
static int Ui_TargetValid(void)
{
    return s_ui.slot >= 0 && s_ui.slot < g_SavegamePtr->inventorySlotCount &&
           s_ui.slot < INV_ITEM_COUNT_MAX &&
           g_SavegamePtr->items[s_ui.slot].id_0 == s_ui.itemId;
}

static int Ui_CmdCount(s32 cmd)
{
    switch (cmd)
    {
        case InvCmdId_UseHealth:
        case InvCmdId_Use:
        case InvCmdId_Equip:
        case InvCmdId_Unequip:
        case InvCmdId_Reload:
        case InvCmdId_Look:
            return 1;

        case InvCmdId_EquipReload:
        case InvCmdId_UnequipReload:
        case InvCmdId_OnOff:
        case InvCmdId_UseLook:
            return 2;

        default:
            return 0;
    }
}

/* The command set the stock screen would offer for the target, or Unk10 (none)
 * for the two cases Inventory_Logic refuses with the error sound before it
 * ever opens the submenu. */
static int Ui_TargetCmd(void)
{
    const s_InventoryItem* it = &g_SavegamePtr->items[s_ui.slot];

    if (it->id_0 == InvItemId_Flauros ||
        (g_SysWork.field_2388.isFlashlightUnavailable_16 && it->id_0 == InvItemId_Flashlight))
        return InvCmdId_Unk10;

    return it->command_2;
}

/* Put the stock selection on the target item. Returns 1 once it is there.
 *
 * invItemSelectedIdx is never written: func_800539A4 recycles exactly one
 * carousel slot per step, so the index has to move through the stock scroll,
 * one press at a time, exactly as pc_inventory_mouse.c walks a click out. */
static int Ui_SeekTarget(void)
{
    s32 slotCount = g_SavegamePtr->inventorySlotCount;
    s32 cur       = g_SysWork.invItemSelectedIdx;
    s32 fwd;

    if (g_Inventory_SelectionId == InventorySelectionId_ItemCmd ||
        g_Inventory_SelectionId == InventorySelectionId_EquippedItemCmd)
    {
        Ui_Press(g_GameWorkPtr->config.controllerConfig.cancel);
        return 0;
    }

    if (g_Inventory_SelectionId != InventorySelectionId_Item)
    {
        /* Same snap the mouse layer uses to move between regions without the
         * settle delay eating the press that follows. */
        g_Inventory_SelectionId          = InventorySelectionId_Item;
        g_Inventory_PrevSelectionId      = InventorySelectionId_Item;
        g_Inventory_SelectionBordersDraw = 8;
        g_Inventory_CmdSelectedIdx       = 0;
        return 0;
    }

    if (cur == s_ui.slot)
        return 1;

    if (slotCount <= 0)
        return 0;

    fwd = ((s_ui.slot - cur) + slotCount) % slotCount;
    if (fwd * 2 <= slotCount)
        Ui_Press(ControllerFlag_LStickRight | ControllerFlag_LStickRight2);
    else
        Ui_Press(ControllerFlag_LStickLeft | ControllerFlag_LStickLeft2);
    return 0;
}

static void Ui_Finish(void)
{
    Ui_Set(s_ui.openedByUs ? UI_CLOSING : UI_IDLE);
}

/* Runs every frame whatever the game is doing. Neither input hook runs during
 * a cutscene, a dialogue, a door transition or the map and option screens, so
 * without this a tap made then would sit in the request slot and open the
 * inventory by itself whenever gameplay next resumed. A tap is acted on now or
 * not at all. */
static void Ui_Watchdog(void)
{
    s32 gs = g_GameWork.gameState;
    int stale;

    pthread_mutex_lock(&s_lock);
    stale = (s_rqKind != RQ_NONE && (SDL_GetTicks() - s_rqAt) > UI_OPEN_TIMEOUT_MS);
    if (stale)
        s_rqKind = RQ_NONE;
    pthread_mutex_unlock(&s_lock);

    if (stale)
        Ui_Fail(EV_UNAVAILABLE);

    if (s_ui.state == UI_IDLE)
        return;

    if (gs != GameState_InGame && gs != GameState_LoadStatusScreen && gs != GameState_InventoryScreen)
    {
        Ui_Set(UI_IDLE);
    }
    else if (s_ui.state == UI_OPENING && gs == GameState_InGame &&
             g_SysWork.sysState != SysState_StatusMenu && Ui_Elapsed() > UI_OPEN_TIMEOUT_MS + 500)
    {
        /* Only while still in the world: once the inventory is loading, the
         * disc decides how long that takes and the request is already won. */
        Ui_Fail(EV_UNAVAILABLE);
        Ui_Set(UI_IDLE);
    }
}

/* Gameplay half. Called from SysState_Gameplay_Update just before it reads
 * the Item button, so a pending tap becomes that button and every stock rule
 * about when the inventory may open (events, attacks, death) still decides. */
void Pc_SecondScreen_GameplayInput(void)
{
    int a, b;
    int kind;

    if (s_ui.state != UI_IDLE && s_ui.state != UI_OPENING)
    {
        /* Back in the world: whatever was in flight is over. */
        Ui_Set(UI_IDLE);
    }

    kind = Ui_TakeRequest(&a, &b);

    if (kind == RQ_SELECT && s_ui.state == UI_IDLE)
    {
        s_ui.slot       = a;
        s_ui.itemId     = b;
        s_ui.row        = 0;
        s_ui.openedByUs = 1;
        if (!Ui_TargetValid())
        {
            Ui_Fail(EV_UNAVAILABLE);
            return;
        }
        Ui_Set(UI_OPENING);
    }
    else if (kind == RQ_CANCEL && s_ui.state == UI_OPENING)
    {
        Ui_Set(UI_IDLE);
        return;
    }

    if (s_ui.state != UI_OPENING)
        return;

    if (Ui_Elapsed() > UI_OPEN_TIMEOUT_MS)
    {
        Ui_Fail(EV_UNAVAILABLE);
        Ui_Set(UI_IDLE);
        return;
    }

    Ui_Press(g_GameWorkPtr->config.controllerConfig.item);
}

/* Inventory half. Called at the top of GameState_ItemScreens_Update, before
 * anything in the frame reads the pad. */
void Pc_SecondScreen_InventoryInput(void)
{
    int a, b;
    int kind;
    s32 step;

    if (g_GameWork.gameState != GameState_InventoryScreen || g_GameWork.gameStateSteps[1] >= 21)
    {
        (void)Ui_TakeRequest(&a, &b);
        s_ui.state = UI_IDLE;
        return;
    }

    step = g_GameWork.gameStateSteps[1];
    kind = Ui_TakeRequest(&a, &b);

    switch (kind)
    {
        case RQ_SELECT:
            /* Tapped while the inventory is already up -- opened by the pad, or
             * a second item picked from the command sheet. */
            if (s_ui.state == UI_IDLE || s_ui.state == UI_MENU || s_ui.state == UI_SEEK)
            {
                if (s_ui.state == UI_IDLE)
                    s_ui.openedByUs = 0;
                s_ui.slot   = a;
                s_ui.itemId = b;
                s_ui.row    = 0;
                if (Ui_TargetValid())
                    Ui_Set(UI_SEEK);
                else
                    Ui_Fail(EV_UNAVAILABLE);
            }
            break;

        case RQ_CHOOSE:
            if (s_ui.state == UI_MENU)
            {
                s_ui.row = a;
                Ui_Set(UI_OPEN_CMD);
            }
            break;

        case RQ_CANCEL:
            if (s_ui.state == UI_OPENING || s_ui.state == UI_SEEK || s_ui.state == UI_MENU)
                Ui_Finish();
            break;

        case RQ_DISMISS:
            if (s_ui.state == UI_VIEWING && step == 14)
                Ui_Press(g_GameWorkPtr->config.controllerConfig.cancel);
            break;

        default:
            break;
    }

    if (s_ui.state == UI_OPENING)
        Ui_Set(UI_SEEK);

    if (s_ui.state == UI_IDLE)
        return;

    /* Leaving (to the game, the map or the options) is the stock screen's own
     * business from here; stop driving it. */
    if (step >= 17)
    {
        Ui_Set(UI_IDLE);
        return;
    }

    if (s_ui.state != UI_MENU && s_ui.state != UI_VIEWING && Ui_Elapsed() > UI_STEP_TIMEOUT_MS)
    {
        Ui_Fail(EV_TIMEOUT);
        Ui_Set(UI_IDLE);
        return;
    }

    switch (s_ui.state)
    {
        case UI_SEEK:
            if (step != 1)
                break;
            if (!Ui_TargetValid())
            {
                Ui_Fail(EV_UNAVAILABLE);
                Ui_Finish();
                break;
            }
            if (Ui_SeekTarget())
                Ui_Set(UI_MENU);
            break;

        case UI_MENU:
            /* Nothing to press. The pad still works here, so the player may
             * have wandered off the item or used it up; the sheet on the
             * second screen only stays while its item is still there. */
            if (step == 1 && !Ui_TargetValid())
                Ui_Finish();
            break;

        case UI_OPEN_CMD:
            if (step != 1)
            {
                /* Refused outright (nothing to do with it here): the stock
                 * code is already showing why. */
                Ui_Set(UI_BUSY);
                break;
            }
            if (!Ui_TargetValid())
            {
                Ui_Fail(EV_UNAVAILABLE);
                Ui_Finish();
                break;
            }
            if (g_Inventory_SelectionId == InventorySelectionId_ItemCmd &&
                g_SysWork.invItemSelectedIdx == (u32)s_ui.slot)
            {
                Ui_Set(UI_CONFIRM);
                break;
            }
            if (Ui_CmdCount(Ui_TargetCmd()) == 0)
            {
                Ui_Fail(EV_CANT_USE);
                Ui_Set(UI_SETTLE);
                break;
            }
            if (Ui_SeekTarget())
                Ui_Press(g_GameWorkPtr->config.controllerConfig.enter);
            break;

        case UI_CONFIRM:
            /* The press must land exactly once. Inventory_Logic ignores input
             * until its settle counter reaches 8 and drops the counter back to
             * 1 when it accepts a press, so a counter that has fallen after we
             * pressed means the press was taken -- even when nothing else
             * changed, which is what a refused Use looks like: the submenu
             * stays open and pressing again would just repeat the refusal. */
            if (step != 1 || g_Inventory_SelectionId != InventorySelectionId_ItemCmd ||
                g_SysWork.invItemSelectedIdx != (u32)s_ui.slot)
            {
                Ui_Set(UI_BUSY);
            }
            else if (s_ui.pressed && g_Inventory_SelectionBordersDraw < 7)
            {
                Ui_Set(UI_BUSY);
            }
            else if (g_Inventory_SelectionBordersDraw >= 7)
            {
                s32 count = Ui_CmdCount(Ui_TargetCmd());
                s32 row   = s_ui.row;

                if (row < 0 || row >= count)
                    row = 0;
                g_Inventory_CmdSelectedIdx = row;
                Ui_Press(g_GameWorkPtr->config.controllerConfig.enter);
                s_ui.pressed = 1;
            }
            break;

        case UI_BUSY:
            if (step == 13 || step == 14)
            {
                Ui_Set(UI_VIEWING);
            }
            else if (step == 1 && (g_Inventory_SelectionId == InventorySelectionId_Item ||
                                   g_Inventory_SelectionId == InventorySelectionId_ItemCmd))
            {
                /* The stock screen reports "can't use it here" and "too dark
                 * to look" by parking its message step on 3 or 4. */
                if (g_SysWork.sysStateSteps[1] == 3)
                    Ui_Fail(EV_CANT_USE);
                else if (g_SysWork.sysStateSteps[1] == 4)
                    Ui_Fail(EV_TOO_DARK);
                Ui_Set(UI_SETTLE);
            }
            break;

        case UI_VIEWING:
            if (step == 1)
                Ui_Set(UI_SETTLE);
            break;

        case UI_SETTLE:
        {
            int msg = (g_SysWork.sysStateSteps[1] == 3 || g_SysWork.sysStateSteps[1] == 4);

            if (Ui_Elapsed() > (Uint32)(msg ? UI_SETTLE_MSG_MS : UI_SETTLE_OK_MS))
                Ui_Finish();
            break;
        }

        case UI_CLOSING:
            if (step != 1)
                break;
            if (g_Inventory_SelectionId == InventorySelectionId_ItemCmd ||
                g_Inventory_SelectionId == InventorySelectionId_EquippedItemCmd)
                Ui_Press(g_GameWorkPtr->config.controllerConfig.cancel);
            else
                Ui_Press(g_GameWorkPtr->config.controllerConfig.item);
            break;

        default:
            break;
    }
}

static void Ss_PutU32(unsigned char* p, u32 v)
{
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* The savegame block is zeroed/stale on the title screen and during the boot
 * movies, so "is a game loaded" is latched rather than read off the current
 * state: set on the first gameplay frame, dropped back at the main menu. The
 * states in between (inventory, map, options, save, loading) keep it. */
static void Ss_TrackSession(void)
{
    s32 gs = g_GameWork.gameState;

    if (gs == GameState_InGame)
        s_session = 1;
    else if (gs <= GameState_MainMenu)
        s_session = 0;
}

static int Ss_Build(unsigned char* out)
{
    const s_Savegame* save = g_SavegamePtr;
    unsigned char*    p;
    s32               health;
    s32               slotCount;
    s32               weaponIdx;
    s32               equippedId = 0;
    s32               ammoId     = -1;
    u32               flags      = 0;
    s32               i;
    s32               written    = 0;

    memset(out, 0, SS_HEADER);
    out[0] = 'S';
    out[1] = 'H';
    out[2] = '2';
    out[3] = 'S';
    out[4] = SS_VERSION;
    out[6] = (unsigned char)g_GameWork.gameState;
    out[7] = (unsigned char)g_SysWork.sysState;

    if (!s_session || save == NULL)
        return SS_HEADER;

    flags |= SS_FLAG_SESSION;
    if (!(save->itemToggleFlags & ItemToggleFlag_FlashlightOff))
        flags |= SS_FLAG_FLASHLIGHT;
    if (save->itemToggleFlags & ItemToggleFlag_RadioOn)
        flags |= SS_FLAG_RADIO;
    out[5] = (unsigned char)flags;

    health = g_SysWork.playerWork.player.health;
    if (health < 0)
        health = 0;
    Ss_PutU32(out + 8, (u32)health);

    slotCount = save->inventorySlotCount;
    if (slotCount > INV_ITEM_COUNT_MAX)
        slotCount = INV_ITEM_COUNT_MAX;

    /* The equipped gun's live rounds are in playerCombat, not in its inventory
     * entry: firing decrements currentWeaponAmmo and the entry is only brought
     * back in step on a reload or an inventory visit. Same for the reserve. */
    weaponIdx = g_SysWork.playerCombat.weaponInventoryIdx;
    if (weaponIdx >= 0 && weaponIdx < slotCount && save->equippedWeapon != InvItemId_Unequipped &&
        save->items[weaponIdx].id_0 == save->equippedWeapon)
    {
        equippedId = save->equippedWeapon;
        if (INV_ITEM_GROUP(equippedId) == InvItemGroup_GunWeapons)
            ammoId = INV_WEAPON_AMMO_ID(equippedId);
    }
    else
    {
        weaponIdx = -1;
    }

    out[12] = (unsigned char)equippedId;
    out[13] = g_SysWork.playerCombat.currentWeaponAmmo;
    out[14] = g_SysWork.playerCombat.totalWeaponAmmo;
    out[16] = (unsigned char)g_SysWork.invItemSelectedIdx;
    out[17] = (unsigned char)weaponIdx; /* 0xFF = nothing equipped */
    out[18] = (unsigned char)s_ui.state;
    out[19] = (unsigned char)s_ui.slot;
    out[20] = (unsigned char)((s_ui.state == UI_MENU && s_ui.slot >= 0 && s_ui.slot < INV_ITEM_COUNT_MAX)
                                  ? Ui_TargetCmd() : 0xFF);
    out[21] = (unsigned char)s_ui.eventSeq;
    out[22] = (unsigned char)s_ui.eventCode;

    p = out + SS_HEADER;

    for (i = 0; i < slotCount; i++)
    {
        const s_InventoryItem* it = &save->items[i];
        const char*            name;
        s32                    count;
        s32                    len;

        if (it->id_0 == (u8)InvItemId_Empty || it->id_0 == InvItemId_Unequipped)
            continue;

        count = it->count_1;
        if (ammoId >= 0)
        {
            if (i == weaponIdx)
                count = g_SysWork.playerCombat.currentWeaponAmmo;
            else if (it->id_0 == ammoId)
                count = g_SysWork.playerCombat.totalWeaponAmmo;
        }

        name = Pc_Inventory_ItemName(it->id_0);
        len  = (name != NULL) ? (s32)strlen(name) : 0;
        if (len > SS_NAME_MAX)
            len = SS_NAME_MAX;

        p[0] = it->id_0;
        p[1] = (unsigned char)count;
        p[2] = (unsigned char)i; /* inventory slot: what a tap sends back */
        p[3] = (unsigned char)len;
        if (len > 0)
            memcpy(p + 4, name, (size_t)len);
        p += 4 + len;
        written++;
    }

    out[15] = (unsigned char)written;
    return (int)(p - out);
}

void Pc_SecondScreen_Update(void)
{
    unsigned char buf[SS_BLOB_MAX];
    int           len;

    Ss_TrackSession();
    Ui_Watchdog();
    len = Ss_Build(buf);

    pthread_mutex_lock(&s_lock);
    if (len != s_pubLen || memcmp(buf, s_pub, (size_t)len) != 0)
    {
        memcpy(s_pub, buf, (size_t)len);
        s_pubLen = len;
        s_serial++;
        if (s_serial <= 0)
            s_serial = 1;
    }
    pthread_mutex_unlock(&s_lock);
}

/* UI thread. Returns the current blob prefixed with its serial (4 bytes LE),
 * or null when the caller already has that serial. */
JNIEXPORT jbyteArray JNICALL
Java_com_silenthill_port_SecondScreen_nativePoll(JNIEnv* env, jclass cls, jint lastSerial)
{
    unsigned char buf[4 + SS_BLOB_MAX];
    int           len    = 0;
    int           serial = 0;
    jbyteArray    arr;

    (void)cls;

    pthread_mutex_lock(&s_lock);
    serial = s_serial;
    if (serial != 0 && serial != lastSerial)
    {
        len = s_pubLen;
        memcpy(buf + 4, s_pub, (size_t)len);
    }
    pthread_mutex_unlock(&s_lock);

    if (len == 0)
        return NULL;

    Ss_PutU32(buf, (u32)serial);

    arr = (*env)->NewByteArray(env, (jsize)(len + 4));
    if (arr == NULL)
    {
        __android_log_print(ANDROID_LOG_WARN, SS_TAG, "nativePoll: NewByteArray(%d) failed", len + 4);
        return NULL;
    }
    (*env)->SetByteArrayRegion(env, arr, 0, (jsize)(len + 4), (const jbyte*)buf);
    return arr;
}

/* UI thread. One request slot is enough: the second screen only ever has one
 * thing to ask at a time, and a newer tap is the one the player means. */
JNIEXPORT void JNICALL
Java_com_silenthill_port_SecondScreen_nativeInput(JNIEnv* env, jclass cls, jint kind, jint a, jint b)
{
    (void)env;
    (void)cls;

    pthread_mutex_lock(&s_lock);
    s_rqKind = kind;
    s_rqA    = a;
    s_rqB    = b;
    s_rqAt   = SDL_GetTicks();
    pthread_mutex_unlock(&s_lock);
}

#else

void Pc_SecondScreen_Update(void)
{
}

void Pc_SecondScreen_GameplayInput(void)
{
}

void Pc_SecondScreen_InventoryInput(void)
{
}

#endif
