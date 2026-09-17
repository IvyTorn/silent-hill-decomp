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
#include "psx_memory.h"

/* Map modules and libdragon's main-executable symbol table live in a dead
 * stretch of the emulated PSX RAM, not on the heap (LIBDRAGON_PATCHES.md #7).
 * [0x24B60, 0xC9578) is where the PSX loaded BODYPROG.BIN; on N64 that code
 * is native, g_OvlBodyprog is set but never written, and the window is .bss
 * we already pay for. The heap runs ~100 KB free in-game and fragmented, so
 * the next map's 200+ KB contiguous memalign failed at the station's front
 * door. The window ends where the PSX map BIN still loads (g_OvlDynamic). */
#define DSO_WIN_BEGIN 0x00024B60u
#define DSO_WIN_END   0x000C9578u
/* Fixed layout, independent of which user asks first (map0_s00 is linked
 * in, so the world pool can come before any dlopen):
 *   [begin, +MODULE_CAP)            the one resident map module
 *   [.., +SYMTAB_CAP)               libdragon's main-exe symbol table
 *   [.., end)                       tail -> native world chunk pool
 * Largest module today is map7_s03, 265,340 B + ~450 B LZ4 margin; the
 * symbol table is 114 KB. Either one outgrowing its cap falls back to heap
 * and says so. */
#define DSO_MODULE_CAP (272u * 1024u)
#define DSO_SYMTAB_CAP (128u * 1024u)

static int      s_winChecked, s_winClean;
static int      s_moduleBusy;
static uint32_t s_symtabUsed;

static uint8_t* DsoWinBase(void)
{
    return (uint8_t*)g_PsxRam + DSO_WIN_BEGIN;
}

/* One-time proof that nothing on this port writes the window: it is zeroed
 * by PsxMemory_Init at boot and must still be zero at the first map load. */
static int DsoWinUsable(void)
{
    if (!s_winChecked)
    {
        const uint8_t* p = DsoWinBase();
        uint32_t n = DSO_WIN_END - DSO_WIN_BEGIN, i;
        s_winChecked = 1;
        s_winClean   = 1;
        for (i = 0; i < n; i++)
            if (p[i] != 0)
            {
                s_winClean = 0;
                SH_DBG("[DSO-WIN] PSX window dirty at +%05x (%02x) -- modules stay on the heap", i, p[i]);
                break;
            }
        if (s_winClean)
            SH_DBG("[DSO-WIN] PSX window %uKB clean: modules + symtab go there", (unsigned)(n / 1024));
    }
    return s_winClean;
}

void* sh_dl_module_alloc(int size)
{
    uint8_t* base;
    if (size <= 0 || s_moduleBusy || !DsoWinUsable())
        return NULL;
    if ((uint32_t)size > DSO_MODULE_CAP)
    {
        SH_DBG("[DSO-WIN] module needs %d B > %u B slot -- heap fallback", size, DSO_MODULE_CAP);
        return NULL;
    }
    base = (uint8_t*)(((uintptr_t)DsoWinBase() + 31u) & ~(uintptr_t)31u);
    s_moduleBusy = 1;
    SH_DBG("[DSO-WIN] module %d B -> %p", size, (void*)base);
    return base;
}

int sh_dl_module_free(void* ptr)
{
    uint8_t* base = (uint8_t*)(((uintptr_t)DsoWinBase() + 31u) & ~(uintptr_t)31u);
    if (ptr != base)
        return 0;
    s_moduleBusy = 0;
    return 1;
}

static uintptr_t DsoSymtabBase(void)
{
    return (((uintptr_t)DsoWinBase() + 31u) & ~(uintptr_t)31u) + DSO_MODULE_CAP;
}

void* sh_dl_symtab_alloc(int size)
{
    uintptr_t p = DsoSymtabBase();
    if (size <= 0 || s_symtabUsed || !DsoWinUsable())
        return NULL;
    if ((uint32_t)size > DSO_SYMTAB_CAP)
    {
        SH_DBG("[DSO-WIN] symtab %d B > %u B reserve -- heap", size, DSO_SYMTAB_CAP);
        return NULL;
    }
    s_symtabUsed = (uint32_t)size;
    SH_DBG("[DSO-WIN] symtab %d B -> %p", size, (void*)p);
    return (void*)p;
}

int ShN64_PsxWindowTail(uint8_t** base, uint32_t* bytes)
{
    uintptr_t lo = (DsoSymtabBase() + DSO_SYMTAB_CAP + 15u) & ~(uintptr_t)15u;
    uintptr_t hi = (uintptr_t)g_PsxRam + DSO_WIN_END;
    if (!DsoWinUsable() || lo >= hi)
        return 0;
    *base  = (uint8_t*)lo;
    *bytes = (uint32_t)(hi - lo);
    return 1;
}

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

    /* The paletted texture cache holds ~272KB of heap and the larger overlays
     * need 70-118KB against a two-digit free figure; release it before asking
     * dlopen for that much. It re-fills lazily on the next textured draw.
     * Unconditional, NOT inside the close-previous block: the very first DSO
     * load (leaving the statically-linked map0_s00) has no previous handle and
     * is exactly the one that was failing. */
    {
        extern void PsxVram_N64ReleaseCache(void);
        PsxVram_N64ReleaseCache();
    }

    /* Same reasoning for the native world renderer: a map switch invalidates
     * every resident chunk block and the area tile store, and their heap is
     * needed for the incoming overlay. */
    {
        extern void ShT3d_WorldReset(void);
        ShT3d_WorldReset();
    }

    {
        extern void Xbox_MemReport(const char* tag);
        Xbox_MemReport("before dlopen");
    }
    /* Evidence for the next reclaim: PSX kernel + SLUS executable RAM below
     * the BODYPROG window. Nothing on N64 is known to address it; a console
     * session of transitions that keeps reporting it clean makes it pool
     * material. (Without an SD card -- the emulator -- savefs_n64.c keeps its
     * RAM memory card at +4000..+24000, so non-zero there is expected.) */
    {
        const uint8_t* p = (const uint8_t*)g_PsxRam;
        uint32_t i, first = DSO_WIN_BEGIN, nz = 0;
        for (i = 0; i < DSO_WIN_BEGIN; i++)
            if (p[i] != 0)
            {
                if (first == DSO_WIN_BEGIN)
                    first = i;
                nz++;
            }
        SH_DBG("[DSO-WIN] low PSX RAM [0,%05x): %u non-zero bytes, first at +%05x",
               DSO_WIN_BEGIN, (unsigned)nz, (unsigned)first);
    }
    for (i = 0; i < sizeof(s_dirs) / sizeof(s_dirs[0]); i++)
    {
        snprintf(path, sizeof(path), "%s/%s.dso", s_dirs[i], mapName);
        s_handle = dlopen(path, RTLD_LOCAL);
        if (s_handle != NULL)
            break;
    }
    {
        extern void Xbox_MemReport(const char* tag);
        Xbox_MemReport("after dlopen");
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

/* Emulator-only loader exercise (main_n64.c runs it when no SD card is
 * mounted, i.e. never on a console): every map module is loaded into the PSX
 * window, bound against the main executable, resolved and unloaded. A
 * hardware round-trip per loader or missing-export bug is the expensive way
 * to find one. */
void MapDso_SelfTest(void)
{
    extern void Xbox_MemReport(const char* tag);
    uint8_t* win = (uint8_t*)(((uintptr_t)DsoWinBase() + 31u) & ~(uintptr_t)31u);
    int area, sub, ok = 0, bad = 0;

    Xbox_MemReport("before DSO self-test");
    for (area = 0; area <= 7; area++)
        for (sub = 0; sub <= 6; sub++)
        {
            char  name[16];
            char  path[64];
            char  sym[48];
            void* h;
            void* hdr = NULL;
            FILE* probe;
            snprintf(name, sizeof(name), "map%d_s%02d", area, sub);
            snprintf(path, sizeof(path), "rom:/maps/%s.dso", name);
            probe = fopen(path, "rb");
            if (probe == NULL)
                continue;
            fclose(probe);
            h = dlopen(path, RTLD_LOCAL);
            if (h != NULL)
            {
                snprintf(sym, sizeof(sym), "%s%s", s_prefix, name);
                hdr = dlsym(h, sym);
            }
            if (h != NULL && hdr != NULL && (uint8_t*)h == win)
                ok++;
            else
                bad++;
            SH_DBG("[DSO-TEST] %s handle=%p header=%p %s", name, h, hdr,
                   (uint8_t*)h == win ? "IN WINDOW" : "on heap");
            if (h != NULL)
                dlclose(h);
        }
    SH_DBG("[DSO-TEST] %d modules OK, %d bad", ok, bad);
    Xbox_MemReport("after DSO self-test");
}
