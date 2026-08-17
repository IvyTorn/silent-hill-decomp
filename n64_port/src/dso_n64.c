/*
 * dso_n64.c - map overlay loading on libdragon's DSO loader.
 *
 * PC, Xbox, 360 and PSP link all 42 map overlays into the executable. This one
 * cannot: the shared AI/particle sources are #included into every map, so those
 * ports rename every defined symbol to <map>_<sym> and link 42 private copies,
 * which is what makes the PSP image 17.7 MB. An N64 has 8 MB.
 *
 * So this port does what the PSX did: one overlay resident at a time, loaded on
 * room transition and dropped when the next one arrives. Each map is its own
 * link unit, which dissolves the duplicate-symbol problem on the way past --
 * two maps may both define Chara_Update without ever meeting.
 *
 * Overlay imports (the hundreds of shared game symbols a map calls) are bound
 * at dlopen time against the .msym table built from the main ELF. Two things
 * have to be right for that, and both live in build_n64.sh: n64dso-extern must
 * pin those symbols so --gc-sections cannot strip them, and n64dso-msym must
 * put the table in the ROM.
 */
#include <libdragon.h>

#include <stdio.h>
#include <string.h>

#include "sh_log.h"
#include "map_dso_n64.h"

static void*      s_handle;
static char       s_openName[16] = "";
static const char s_prefix[] = "g_MapOverlayHeader_";

/* rom: before sd: here, the opposite of cd_n64.c's disc probe -- and for the
 * opposite reason. The disc is user data that a card should be able to
 * override; overlays are part of the BUILD, and an overlay from a different
 * build than the executable would bind against the wrong msym table and jump
 * into nothing. Taking the ROM's own copy first makes that mismatch impossible.
 * sd: remains as a fallback for a cartridge too small to carry them. */
static const char* const s_dirs[] = {
    "rom:/maps",
    "sd:/silenthill/maps",
};

void* MapDso_Open(const char* mapName)
{
    char  path[80];
    char  sym[48];
    void* hdr = NULL;
    unsigned i;

    if (mapName == NULL || mapName[0] == '\0')
        return NULL;

    /* Already open: hand back the same header rather than reloading. A room
     * transition that returns to the map it came from is common. */
    if (s_handle != NULL && strcmp(s_openName, mapName) == 0)
    {
        snprintf(sym, sizeof(sym), "%s%s", s_prefix, mapName);
        return dlsym(s_handle, sym);
    }

    if (s_handle != NULL)
    {
        SH_DBG("[DSO] closing %s", s_openName);
        dlclose(s_handle);
        s_handle      = NULL;
        s_openName[0] = '\0';
    }

    for (i = 0; i < sizeof(s_dirs) / sizeof(s_dirs[0]); i++)
    {
        snprintf(path, sizeof(path), "%s/%s.dso", s_dirs[i], mapName);
        s_handle = dlopen(path, RTLD_LOCAL);
        if (s_handle != NULL)
            break;
    }

    if (s_handle == NULL)
    {
        SH_DBG("[DSO] %s.dso not found in rom:/maps or sd:/silenthill/maps", mapName);
        return NULL;
    }

    snprintf(sym, sizeof(sym), "%s%s", s_prefix, mapName);
    hdr = dlsym(s_handle, sym);
    if (hdr == NULL)
    {
        /* The overlay loaded but does not carry the symbol the registry wants.
         * That is a build mismatch, not a missing file, and saying so is the
         * difference between a five-minute fix and an afternoon. */
        SH_DBG("[DSO] %s.dso loaded but has no %s", mapName, sym);
        dlclose(s_handle);
        s_handle = NULL;
        return NULL;
    }

    snprintf(s_openName, sizeof(s_openName), "%s", mapName);
    SH_DBG("[DSO] %s resident, header=%08x", mapName, (unsigned)(unsigned long)hdr);
    return hdr;
}

const char* MapDso_Current(void)
{
    return s_openName;
}
