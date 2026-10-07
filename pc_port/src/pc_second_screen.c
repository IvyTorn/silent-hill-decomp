/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Android: live inventory snapshot for a secondary display (AYN Thor).
 *
 * Read-only. The game thread copies the few fields the second screen shows
 * into a flat little-endian blob once per frame; the Java side
 * (SecondScreen.java) polls it from the UI thread and draws it with ordinary
 * Android views. Nothing here writes game state, and the renderer is not
 * involved -- the second display is a plain Android window, not a GL surface.
 *
 * The two threads never touch each other's data: the blob is rebuilt into a
 * local buffer, compared against the published one, and swapped under a mutex
 * only when it differs. A serial number lets the poll return nothing at all
 * when the picture has not changed, which is nearly every poll.
 */
#include "game.h"

#include "bodyprog/bodyprog.h"
#include "bodyprog/items.h"
#include "bodyprog/savegame.h"

#include <string.h>

#if defined(__ANDROID__)

#include <jni.h>
#include <pthread.h>
#include <android/log.h>

#define SS_TAG       "SH2Screen"
#define SS_VERSION   1
#define SS_NAME_MAX  48
#define SS_HEADER    20
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
        p[2] = (unsigned char)i; /* inventory slot, for phase 2 */
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

#else

void Pc_SecondScreen_Update(void)
{
}

#endif
