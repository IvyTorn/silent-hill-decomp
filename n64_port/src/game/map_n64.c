/*
 * map_n64.c - the map registry on N64. Mirrors xbox_port/src/map_xbox.c, which
 * mirrors pc_port/src/map_registry.c.
 *
 * On the GAME side of the include firewall (n64_port/src/game/): it needs
 * s_MapOverlayHdr and e_MapIdx, so it must not see libdragon.
 *
 * The difference from every other port is the table. PC, Xbox and PSP link all
 * 42 map overlays statically because they have the RAM; that is what makes the
 * PSP image 17.7 MB. The N64 does not have it and goes back to what the PSX
 * actually did -- one overlay resident at a time -- through libdragon's DSO
 * loader (dso_n64.c, across the include firewall).
 *
 * A transition to an overlay that will not load is still REFUSED rather than
 * followed into a NULL header: a refused transition leaves the player standing
 * still, which is obvious, while a followed one dereferences NULL function
 * pointers somewhere far from here.
 */
#include <string.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/map/map.h"
#include "sh_log.h"
#include "map_dso_n64.h"

extern s_MapOverlayHdr g_MapOverlayHeader_map0_s00;

/* RSP-park evidence, called across the include firewall from gpu_rdp.c's
 * FrameBegin the moment the RSP is seen parked (~1 s in, long before the 5 s
 * watchdog's [CRASH] hole dump): what the GAME was doing when a stray 32-bit
 * zero landed in the command queue. Both parks logged so far had Harry
 * standing still for seconds beforehand -- the idle-animation window -- so
 * the field set is chosen to confirm or kill that: motion/pose state,
 * animation, weapon and held item, map. Lives here (game side) because it
 * needs g_SysWork. */
void ShN64_ParkSnapshot(unsigned frame, unsigned ms)
{
    s_SubCharacter* p = &g_SysWork.playerWork.player;
    SH_DBG("[RSPQ-PARK] f=%u +%ums map=%d pos=(%d,%d,%d) rotY=%d state=%d upper=%d anim=%d "
           "weaponAtk=%d held=%d",
           frame, ms, (int)g_SavegamePtr->mapIdx,
           (int)p->position.vx, (int)p->position.vy, (int)p->position.vz,
           (int)p->rotation.vy,
           (int)g_SysWork.playerWork.extra.state,
           (int)g_SysWork.playerWork.extra.upperBodyState,
           (int)p->model.anim.status,
           (int)g_SysWork.playerCombat.weaponAttack,
           (int)g_WorldGfxWork.heldItem.itemId);
}

s_MapOverlayHdr* g_pMapOverlayHeader = &g_MapOverlayHeader_map0_s00;

/* Same order as e_MapIdx / PC map_registry.c. */
#define MAP_N64_COUNT 44
static const char* const MAP_N64_NAMES[MAP_N64_COUNT] = {
    "map0_s00", "map0_s01", "map0_s02",
    "map1_s00", "map1_s01", "map1_s02", "map1_s03", "map1_s04", "map1_s05", "map1_s06",
    "map2_s00", "map2_s01", "map2_s02", "map2_s03", "map2_s04",
    "map3_s00", "map3_s01", "map3_s02", "map3_s03", "map3_s04", "map3_s05", "map3_s06",
    "map4_s00", "map4_s01", "map4_s02", "map4_s03", "map4_s04", "map4_s05", "map4_s06",
    "map5_s00", "map5_s01", "map5_s02", "map5_s03",
    "map6_s00", "map6_s01", "map6_s02", "map6_s03", "map6_s04", "map6_s05",
    "map7_s00", "map7_s01", "map7_s02", "map7_s03",
    "mapx_s00",
};

/* Index 0 is the statically linked map0_s00. The rest are filled in by the DSO
 * loader at runtime, which is why this is not const. */
static s_MapOverlayHdr* s_mapHeaders[MAP_N64_COUNT] = {
    &g_MapOverlayHeader_map0_s00,
    /* remainder NULL */
};

static int s_currentMapIdx = 0; /* MapIdx_MAP0_S00 */

/* Bring an overlay in. Only ONE non-static overlay is resident at a time --
 * MapDso_Open closes the previous one -- so every other entry has to be
 * forgotten here, or the registry would keep handing out a header whose code
 * and data have just been unmapped. That is the PSX's own arrangement; the
 * ports that keep all 42 resident are the ones doing something unusual. */
static void MapN64_Open(int id)
{
    void* hdr;
    int   i;

    if (id <= 0 || id >= MAP_N64_COUNT)
        return;                                  /* 0 is static, never loaded */

    hdr = MapDso_Open(MAP_N64_NAMES[id]);
    if (hdr == NULL)
        return;

    for (i = 1; i < MAP_N64_COUNT; i++)
        s_mapHeaders[i] = NULL;

    s_mapHeaders[id] = (s_MapOverlayHdr*)hdr;
}

int MapXbox_OverlayIsLinked(int mapIdx)
{
    return mapIdx >= 0 && mapIdx < MAP_N64_COUNT && s_mapHeaders[mapIdx] != NULL;
}

/* Rate-limited: the blocking event is typically TriggerType_None and re-fires
 * EVERY frame, so log occurrence #1 and then every 300th (~10 s at 30 fps). */
void MapXbox_LogUnlinkedOverlay(int mapIdx, const char* where)
{
    static int      s_lastIdx = -1;
    static unsigned s_count;

    if (mapIdx != s_lastIdx)
    {
        s_lastIdx = mapIdx;
        s_count   = 0;
    }

    if ((s_count++ % 300) == 0)
    {
        SH_DBG("[MAP-GUARD] overlay %d (%s) NOT RESIDENT - transition refused at %s (x%u)",
               mapIdx,
               (mapIdx >= 0 && mapIdx < MAP_N64_COUNT) ? MAP_N64_NAMES[mapIdx] : "??",
               where, s_count);
        SH_DebugLogFlush();
    }
}

int MapRegistry_Count(void) { return MAP_N64_COUNT; }

int MapRegistry_FindByName(const char* name)
{
    int i;

    if (name == NULL)
        return -1;

    for (i = 0; i < MAP_N64_COUNT; i++)
    {
        if (strcmp(name, MAP_N64_NAMES[i]) == 0)
            return i;
    }

    return -1;
}

const char* MapRegistry_GetName(int id)
{
    if (id >= 0 && id < MAP_N64_COUNT)
        return MAP_N64_NAMES[id];

    return "unknown";
}

const char* MapRegistry_GetDescription(int id)
{
    (void)id;
    return "";
}

void MapRegistry_Load(int id)
{
    s_MapOverlayHdr* header;

    if (id < 0 || id >= MAP_N64_COUNT)
    {
        SH_DBG("[MAP-LOAD] MapRegistry_Load(%d) out of range", id);
        return;
    }

    if (s_mapHeaders[id] == NULL)
        MapN64_Open(id);

    if (s_mapHeaders[id] == NULL)
    {
        SH_DBG("[MAP-LOAD] MapRegistry_Load(%d)=%s NOT RESIDENT - REFUSED (header stays %s)",
               id, MapRegistry_GetName(id), MapRegistry_GetName(s_currentMapIdx));
        return;
    }

    header = s_mapHeaders[id];
    g_pMapOverlayHeader = header;
    s_currentMapIdx     = id;

    SH_DBG("[MAP-LOAD] MapRegistry_Load(%d)=%s header=%08x",
           id, MapRegistry_GetName(id), (unsigned)header);
}
