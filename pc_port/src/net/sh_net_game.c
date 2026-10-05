/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_game.c - the half of the online client that knows what g_SysWork is.
 *
 * sh_net_client.c is deliberately engine-free; this file is the only place the
 * network layer reads the game. It runs once a frame from MainLoop and does
 * three things: publish where this player is, pull the worker's findings into
 * the game-thread copy, and notice the two events worth telling the server
 * about (a map change and a death).
 *
 * Everything here is a READ of game state except the death edge, which owns a
 * single static of its own. Nothing in this file can change how the game plays
 * — that is the point, and it is what keeps the online branch from drifting
 * away from the single-player one.
 */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/map/map.h"
#include "bodyprog/items.h"        /* INV_ITEM_COUNT_MAX, s_InventoryItem */
#include "bodyprog/item_screens.h" /* Inventory_AddSpecialItem: grant a shared pickup */
#include "bodyprog/collision/collision.h" /* Collision_SurfaceGet: ground-snap a joining guest */
#include "sh_net_steam.h"          /* ShSteam_LobbyOwner: identify the host member */

#include "sh_net.h"
#include "sh_net_memo.h"
#include "sh_net_chat.h"
#include "sh_net_session.h"
#include "sh_net_coop.h"     /* ShNet_SetCoopActive: block pausing in a session */
#include "sh_net_internal.h"
#include "sh_net_platform.h" /* ShNetPlat_Millis, for the roster re-request throttle */
#include "pc_discord.h" /* Pc_MapAreaName, for Steam rich presence */
#include "pc_config.h"
#include "pc_playas.h"
#include "sh_log.h"

/* Above this the player reads as "moving fast" to a watching ghost. Q19.12,
 * and comfortably under Harry's run speed so a jog still counts. */
#define SHNET_RUN_SPEED Q12(0.035f)

static int s_lastMap      = -1;
static int s_deathLatched = 0;
static int s_initDone     = 0;

/* Throttle for the "a ghost is still unnamed" roster re-request below. */
#define SHNET_NAME_REQ_MS 3000u
static unsigned int s_lastNameReqMs = 0;

/* Recompute every frame rather than caching: the player can change character
 * mid-session (PLAYAS), and a ghost wearing the wrong body is worse than the
 * three instructions this costs. */
static int ShNetG_LocalChara(void)
{
    int id = Pc_PlayAs_SkinCharaId();
    if (id < 0 || id > 255)
    {
        id = 0;
    }
    return id;
}

static int ShNetG_BuildFlags(void)
{
    int flags = 0;

    if (g_SysWork.playerWork.player.health > Q12(0.0f))
    {
        flags |= SHNET_PF_ALIVE;
    }
    if (g_SysWork.field_2388.isFlashlightOn_15)
    {
        flags |= SHNET_PF_FLASHLIGHT;
    }
    if (g_SysWork.playerWork.player.moveSpeed > SHNET_RUN_SPEED)
    {
        flags |= SHNET_PF_RUNNING;
    }
    /* The border state is the game's own "a scene is playing" signal, and it
     * is what the ghost renderer keys on: during a cutscene the player is
     * standing somewhere the camera put them, not somewhere they walked. */
    if (g_SysWork.cutsceneBorderState != 0)
    {
        flags |= SHNET_PF_CUTSCENE;
    }
    if (g_SysWork.sysState != SysState_Gameplay)
    {
        flags |= SHNET_PF_MENU;
    }
    return flags;
}

/* Co-op shared items. Each frame in a session we diff the inventory against last
 * frame's per-id totals: a count that went UP is a pickup, which we hand to the
 * other players; what they hand us we add to our own inventory. Receives are
 * folded into the snapshot before the diff, so a granted item is never bounced
 * back. Keyed by item id and summed across slots, so a merged stack and a new
 * slot both read as the same +N. Inert outside a co-op game: the snapshot resets,
 * so single-player never queues a thing. Known gap: the WORLD pickup is not yet
 * removed on the other clients, so two players walking over the same box both
 * collect it; flag-based specials (plates, stone) already de-dup themselves. */
static void ShNetG_SyncItems(void)
{
    extern int            g_PcCoopGame;
    static unsigned short s_snap[256];
    static int            s_snapInit;
    unsigned short        cur[256];
    int                   i, id, cnt;

    if (!g_PcCoopGame || !ShSession_Active())
    {
        s_snapInit = 0;
        return;
    }

    while (ShSession_TakeItem(&id, &cnt))
    {
        if (id > 0 && id < 256 && id != 0xFF && cnt > 0)
        {
            int t;
            Inventory_AddSpecialItem((u8)id, (u8)(cnt > 255 ? 255 : cnt));
            t          = (int)s_snap[id] + cnt;
            s_snap[id] = (unsigned short)(t > 65535 ? 65535 : t);
            SH_DBG("[COOP] got shared item %d x%d", id, cnt);
        }
    }

    memset(cur, 0, sizeof(cur));
    for (i = 0; i < INV_ITEM_COUNT_MAX; i++)
    {
        int sid = g_SavegamePtr->items[i].id_0;
        int sc  = g_SavegamePtr->items[i].count_1;
        if (sid <= 0 || sid >= 0xFF)
        {
            continue;
        }
        {
            int t    = (int)cur[sid] + sc;
            cur[sid] = (unsigned short)(t > 65535 ? 65535 : t);
        }
    }

    if (s_snapInit)
    {
        for (i = 1; i < 0xFF; i++)
        {
            if (cur[i] > s_snap[i])
            {
                ShSession_QueueItem(i, (int)cur[i] - (int)s_snap[i]);
                SH_DBG("[COOP] shared our pickup item %d x%d", i, (int)cur[i] - (int)s_snap[i]);
            }
        }
    }
    memcpy(s_snap, cur, sizeof(s_snap));
    s_snapInit = 1;
}

/* Set by title.c when a guest boots into the host's map (join or join-in-
 * progress); cleared once we have placed him next to the host. */
int g_PcCoopGuestSpawn = 0;

/* Guest join placement. A guest who boots into the host's map should appear next
 * to the host, not at the map's New Game spawn (which, for a mid-game joiner,
 * could be a whole map away). We wait until we have actually received the host's
 * pose on this map, then drop the player there with a ground snap so he lands on
 * the floor rather than in the air or under it. The host is standing in a valid
 * spot, so placing the guest at his position cannot be inside a wall. Runs once.
 */
static void ShNetG_CoopGuestSpawn(int mapIdx)
{
    unsigned long long     hostId;
    const ShSessionMember* host = NULL;
    int                    i, n;

    if (!g_PcCoopGuestSpawn || !ShSession_Active() || ShSession_IsHost())
    {
        return;
    }

    hostId = ShSteam_LobbyOwner();
    n      = ShSession_MemberCount();
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (m && m->steamId == hostId)
        {
            host = m;
            break;
        }
    }

    /* Need the host's live pose, and he must be on our map. Until then wait: the
     * player sits at the New Game spawn for the few frames it takes to hear him. */
    if (!host || host->poseMs == 0 || host->mapIdx != mapIdx)
    {
        return;
    }

    {
        s_SubCharacter*    hp = &g_SysWork.playerWork.player;
        s_CollisionSurface coll;
        Collision_SurfaceGet(&coll, host->x, host->z);
        hp->position.vx = host->x;
        hp->position.vz = host->z;
        if (coll.groundHeight != Q12(8.0f)) /* Q12(8.0f) is the "no floor here" sentinel */
        {
            hp->position.vy                    = coll.groundHeight;
            hp->properties.player.groundHeight = coll.groundHeight;
        }
        else
        {
            hp->position.vy = host->y;
        }
        hp->rotation.vy = (s16)host->rotY;
    }

    g_PcCoopGuestSpawn = 0;
    SH_DBG("[COOP] guest placed next to host on map %d", mapIdx);
}

void ShNet_OnMapChanged(int mapIdx)
{
    if (mapIdx == s_lastMap)
    {
        return;
    }
    s_lastMap      = mapIdx;
    s_deathLatched = 0;
    ShNet_RequestMemos();
    ShNet_RequestRoster();
    /* What friends see on the Steam friends list. The area name is the same
     * one the Discord presence line and the player list use. */
    ShSession_PublishPresence(Pc_MapAreaName(mapIdx), mapIdx);

    /* Co-op host: tell the session members to follow us into this map so a guest
     * boots (or re-boots) into the same area. Only the host dictates the world. */
    {
        extern int g_PcCoopGame;
        if (g_PcCoopGame && ShSession_IsHost())
        {
            ShSession_RequestWorld(mapIdx);
        }
    }
}

void ShNet_GameTick(void)
{
    int   inWorld;
    int   mapIdx;
    int   flags;
    short health;

    if (!s_initDone)
    {
        /* Deferred to the first frame rather than done in main_pc.c's init:
         * the config is fully parsed by then, and starting a thread before
         * SDL is up is the kind of ordering bug that only shows on someone
         * else's machine. */
        s_initDone = 1;
        ShNet_Init();
    }

    if (!ShNet_Enabled())
    {
        return;
    }

    ShNetMemo_Tick();
    ShNetChat_Tick();

    /* A multiplayer game is never pausable -- pausing a world meant to be shared
     * is the one thing co-op must not do. Drive the coop seam from g_PcCoopGame
     * (set when the game was launched via the Multiplayer menu) so
     * ShNet_LiveWorld() / ShNet_PauseBlocked() report it; a single-player game
     * leaves it 0 and pauses normally. */
    {
        extern int g_PcCoopGame;
        if (g_PcCoopGame != ShNet_CoopActive())
        {
            ShNet_SetCoopActive(g_PcCoopGame, g_PcCoopGame ? "multiplayer game" : "single-player");
        }
    }

    inWorld = (g_GameWork.gameState == GameState_InGame) &&
              (g_SavegamePtr != NULL) &&
              !(g_SysWork.sysFlags & SysFlag_DemoActive);

    if (!inWorld)
    {
        ShNet_PublishLocal(0, s_lastMap < 0 ? 0 : s_lastMap, ShNetG_LocalChara(), 0,
                           0, 0, 0, 0, 0, 0, 0);
        ShSession_PublishLocalPos(-1, 0, 0, 0, 0, 0, 0, 0, 0); /* no pose outside a map */
        ShSession_PublishPresence("In the menus", -1);
        ShNet_PumpToGameThread();
        return;
    }

    mapIdx = (int)g_SavegamePtr->mapIdx;
    if (mapIdx != s_lastMap)
    {
        ShNet_OnMapChanged(mapIdx);
    }

    flags = ShNetG_BuildFlags();

    {
        q19_12 hp = g_SysWork.playerWork.player.health;
        int    pct;
        if (hp < 0)
        {
            hp = 0;
        }
        pct = (int)(hp >> 12);
        if (pct > 999)
        {
            pct = 999;
        }
        health = (short)pct;
    }

    ShNet_PublishLocal(1, mapIdx, ShNetG_LocalChara(), flags,
                       (int)g_SysWork.playerWork.player.position.vx,
                       (int)g_SysWork.playerWork.player.position.vy,
                       (int)g_SysWork.playerWork.player.position.vz,
                       (short)g_SysWork.playerWork.player.rotation.vy,
                       health,
                       (unsigned short)g_SysWork.playerWork.player.model.anim.status,
                       (unsigned short)g_SysWork.playerWork.player.model.anim.keyframeIdx);

    /* Co-op presence: the same pose, to the Steam session members. */
    ShSession_PublishLocalPos(mapIdx, ShNetG_LocalChara(), flags,
                       (int)g_SysWork.playerWork.player.position.vx,
                       (int)g_SysWork.playerWork.player.position.vy,
                       (int)g_SysWork.playerWork.player.position.vz,
                       (short)g_SysWork.playerWork.player.rotation.vy,
                       (unsigned short)g_SysWork.playerWork.player.model.anim.status,
                       (unsigned short)g_SysWork.playerWork.player.model.anim.keyframeIdx);

    /* Co-op shared items: hand out what we picked up, take in what others did. */
    ShNetG_SyncItems();

    /* Co-op: place a joining guest next to the host once his pose is known. */
    ShNetG_CoopGuestSpawn(mapIdx);

    /* Death marker, once per death. Latched rather than edge-detected on the
     * health value alone: health sits at zero for the whole death animation
     * and the game-over screen, which would otherwise place a marker every
     * frame for several seconds. Cleared when the player is alive again,
     * which covers both continue and load. */
    if (g_PcConfig.onlineDeaths)
    {
        const int dead = (g_SysWork.playerWork.player.health <= Q12(0.0f)) ||
                         (g_SysWork.sysState == SysState_GameOver);
        if (dead && !s_deathLatched)
        {
            s_deathLatched = 1;
            ShNet_ReportDeath((int)g_SysWork.playerWork.player.position.vx,
                              (int)g_SysWork.playerWork.player.position.vy,
                              (int)g_SysWork.playerWork.player.position.vz,
                              (short)g_SysWork.playerWork.player.rotation.vy);
            SH_DBG("[NET] death marker placed on map %d", mapIdx);
        }
        else if (!dead)
        {
            s_deathLatched = 0;
        }
    }

    ShNet_PumpToGameThread();

    /* Ghost names come from the roster, which is fetched once on connect and on
     * each map change. A player who reaches this map after that shows as "?"
     * (the placeholder set when a ghost is first seen), which also leaves the
     * nameplate blank. Ask for the roster again -- throttled -- whenever a ghost
     * is still unnamed, so late arrivals get named without polling the roster
     * every frame. */
    {
        int n = ShNet_GhostCount();
        int i;
        int unnamed = 0;
        for (i = 0; i < n; i++)
        {
            const ShNetGhost* g = ShNet_Ghost(i);
            if (g && (g->name[0] == '\0' || g->name[0] == '?'))
            {
                unnamed = 1;
                break;
            }
        }
        if (unnamed)
        {
            unsigned int now = ShNetPlat_Millis();
            if (now - s_lastNameReqMs >= SHNET_NAME_REQ_MS)
            {
                s_lastNameReqMs = now;
                ShNet_RequestRoster();
            }
        }
    }
}
