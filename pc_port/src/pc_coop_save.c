/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See pc_coop_save.h. Game thread only. */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/savegame.h"

#include "pc_coop_save.h"
#include "sh_log.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef _WIN32
int _mkdir(const char* dirname);
#endif

#define COOP_SAVE_DIR   "gamedata/coopsaves"
#define COOP_SAVE_EXT   ".sav"
#define COOP_SAVE_MAGIC 0x53504F43u /* "COPS" */
#define COOP_SAVE_VER   1u

typedef struct
{
    unsigned int magic;
    unsigned int version;
    char         name[COOP_SAVE_NAME_MAX];
    int          mapIdx;
    int          saveCount;
    unsigned int gameplayTimer;
    unsigned int reserved[4];
} CoopSaveHeader;

static void Coop_EnsureDir(void)
{
#ifdef _WIN32
    _mkdir("gamedata");
    _mkdir(COOP_SAVE_DIR);
#else
    mkdir("gamedata", 0755);
    mkdir(COOP_SAVE_DIR, 0755);
#endif
}

/* Lowercased, spaces to '_', only [a-z0-9_-] kept, so a custom name is always a
 * safe filename. */
static void Coop_SafeName(const char* in, char* out, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    if (in)
    {
        for (; *in && i < cap - 1; in++)
        {
            char c = *in;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')
                out[i++] = c;
            else if (c == ' ' || c == '_')
                out[i++] = '_';
        }
    }
    if (i == 0)
    {
        /* Nothing usable: a stable default so a save is never nameless. */
        const char* d = "coop";
        while (*d && i < cap - 1) out[i++] = *d++;
    }
    out[i] = '\0';
}

static void Coop_Path(const char* name, char* out, int cap)
{
    char safe[COOP_SAVE_NAME_MAX];
    Coop_SafeName(name, safe, sizeof(safe));
    snprintf(out, cap, "%s/%s%s", COOP_SAVE_DIR, safe, COOP_SAVE_EXT);
}

int Pc_CoopSave_Write(const char* name)
{
    char           path[256];
    CoopSaveHeader h;
    FILE*          f;

    Coop_EnsureDir();
    Coop_Path(name, path, sizeof(path));

    /* Pull the live player state into the savegame blob first, exactly as a
     * save point does. */
    SysWork_SavegameUpdatePlayer();

    f = fopen(path, "wb");
    if (!f)
    {
        SH_DBG("[COOPSAVE] cannot write %s", path);
        return 0;
    }

    memset(&h, 0, sizeof(h));
    h.magic         = COOP_SAVE_MAGIC;
    h.version       = COOP_SAVE_VER;
    Coop_SafeName(name, h.name, sizeof(h.name));
    h.mapIdx        = (int)g_SavegamePtr->mapIdx;
    h.saveCount     = (int)g_SavegamePtr->savegameCount;
    h.gameplayTimer = (unsigned int)g_SavegamePtr->gameplayTimer;

    if (fwrite(&h, sizeof(h), 1, f) != 1 ||
        fwrite(g_SavegamePtr, sizeof(s_Savegame), 1, f) != 1)
    {
        SH_DBG("[COOPSAVE] short write %s", path);
        fclose(f);
        return 0;
    }
    fclose(f);
    SH_DBG("[COOPSAVE] saved %s (map %d)", path, h.mapIdx);
    return 1;
}

int Pc_CoopSave_Load(const char* name)
{
    char           path[256];
    CoopSaveHeader h;
    FILE*          f;

    Coop_Path(name, path, sizeof(path));
    f = fopen(path, "rb");
    if (!f)
    {
        SH_DBG("[COOPSAVE] cannot open %s", path);
        return 0;
    }
    if (fread(&h, sizeof(h), 1, f) != 1 ||
        h.magic != COOP_SAVE_MAGIC || h.version != COOP_SAVE_VER)
    {
        SH_DBG("[COOPSAVE] bad header %s", path);
        fclose(f);
        return 0;
    }
    if (fread(g_SavegamePtr, sizeof(s_Savegame), 1, f) != 1)
    {
        SH_DBG("[COOPSAVE] short read %s", path);
        fclose(f);
        return 0;
    }
    fclose(f);
    SH_DBG("[COOPSAVE] loaded %s (map %d)", path, (int)g_SavegamePtr->mapIdx);
    return 1;
}

int Pc_CoopSave_List(CoopSaveEntry* out, int max)
{
    DIR*           d;
    struct dirent* e;
    int            n = 0;

    if (!out || max <= 0)
    {
        return 0;
    }
    d = opendir(COOP_SAVE_DIR);
    if (!d)
    {
        return 0;
    }
    while ((e = readdir(d)) != NULL && n < max)
    {
        char           path[256];
        CoopSaveHeader h;
        FILE*          f;
        size_t         len = strlen(e->d_name);
        const size_t   ext = sizeof(COOP_SAVE_EXT) - 1;

        if (len <= ext || strcmp(e->d_name + (len - ext), COOP_SAVE_EXT) != 0)
        {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", COOP_SAVE_DIR, e->d_name);
        f = fopen(path, "rb");
        if (!f)
        {
            continue;
        }
        if (fread(&h, sizeof(h), 1, f) == 1 &&
            h.magic == COOP_SAVE_MAGIC && h.version == COOP_SAVE_VER)
        {
            h.name[COOP_SAVE_NAME_MAX - 1] = '\0';
            snprintf(out[n].name, COOP_SAVE_NAME_MAX, "%s", h.name[0] ? h.name : e->d_name);
            snprintf(out[n].file, sizeof(out[n].file), "%s", e->d_name);
            out[n].mapIdx    = h.mapIdx;
            out[n].saveCount = h.saveCount;
            n++;
        }
        fclose(f);
    }
    closedir(d);
    return n;
}
