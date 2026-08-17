/*
 * savefs_n64.c - a writable filesystem for the memory card, mounted at "sav:/".
 *
 * WHY THIS EXISTS, precisely: GameState_KcetLogo_MemCardCheck returns "rerun me
 * next frame" until func_80033548() reports the cards ready, and it never gives
 * up. With no writable save location mcard_xbox.c's _card_info delivers
 * EvSpTIMOUT forever, so the boot wedges on the KCET logo and the game can
 * never reach its title screen. That file's own comment predicted it -- an
 * earlier version reported empty slots the real-PSX way and "the memcard
 * element layer wedged polling channel 18 forever at the KCET-logo check".
 *
 * The N64's honest answers are a Controller Pak (32 KB, a quarter of a PSX
 * card) or a flashcart's SD. SD is preferred and used when present. This is the
 * fallback for when neither is there, which on an emulator is always: a plain
 * RAM disk holding the card image.
 *
 * It is NOT a pretend save. Saving and loading work for the session, and the
 * data is genuinely gone at power-off -- which is stated at mount time in the
 * log rather than discovered later. Persistence is the SD path, and a
 * Controller Pak backend would be the cartridge-native one.
 */
#include <libdragon.h>
/* libdragon.h does NOT pull in system.h, which is where filesystem_t and
 * attach_filesystem live. */
#include <system.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sh_log.h"
#include "savefs_n64.h"

/* Two channels is the whole requirement: mcard_xbox.c masks every channel with
 * (chan & 1) onto 0.MCD and 1.MCD. A PSX card image is 128 KB, and on a machine
 * with ~350 KB of heap that is not a size to round up. */
#define SAVEFS_MAX_FILES 2
#define SAVEFS_MAX_SIZE  (128 * 1024)

typedef struct
{
    char           name[16];
    unsigned char* data;
    int            size;
    int            used;
} SaveFile;

typedef struct
{
    SaveFile* file;
    int       pos;
    int       writable;
} SaveHandle;

static SaveFile   s_files[SAVEFS_MAX_FILES];
static SaveHandle s_handles[4];
static int        s_mounted;

static SaveFile* SaveFs_Find(const char* name, int create)
{
    int i, freeIdx = -1;

    while (*name == '/')
        name++;

    for (i = 0; i < SAVEFS_MAX_FILES; i++)
    {
        if (s_files[i].used && strcmp(s_files[i].name, name) == 0)
            return &s_files[i];
        if (!s_files[i].used && freeIdx < 0)
            freeIdx = i;
    }

    if (!create || freeIdx < 0)
        return NULL;

    s_files[freeIdx].data = (unsigned char*)malloc(SAVEFS_MAX_SIZE);
    if (s_files[freeIdx].data == NULL)
    {
        SH_DBG("[SAVEFS] out of heap allocating %s (%d KB)", name, SAVEFS_MAX_SIZE / 1024);
        return NULL;
    }

    memset(s_files[freeIdx].data, 0, SAVEFS_MAX_SIZE);
    snprintf(s_files[freeIdx].name, sizeof(s_files[freeIdx].name), "%s", name);
    s_files[freeIdx].size = 0;
    s_files[freeIdx].used = 1;
    SH_DBG("[SAVEFS] created %s", name);
    return &s_files[freeIdx];
}

static void* SaveFs_Open(char* name, int flags)
{
    SaveFile* f;
    int       i;
    int       wantWrite = (flags & (O_WRONLY | O_RDWR)) != 0;

    f = SaveFs_Find(name, wantWrite);
    if (f == NULL)
    {
        errno = ENOENT;
        return NULL;
    }

    /* O_TRUNC is what fopen("wb") asks for. Length goes to zero; the buffer
     * stays allocated, because the only writer immediately fills it again. */
    if ((flags & O_TRUNC) != 0)
        f->size = 0;

    for (i = 0; i < (int)(sizeof(s_handles) / sizeof(s_handles[0])); i++)
    {
        if (s_handles[i].file == NULL)
        {
            s_handles[i].file     = f;
            s_handles[i].pos      = 0;
            s_handles[i].writable = wantWrite;
            return &s_handles[i];
        }
    }

    errno = EMFILE;
    return NULL;
}

static int SaveFs_Close(void* file)
{
    SaveHandle* h = (SaveHandle*)file;
    if (h == NULL)
        return -1;
    h->file = NULL;
    return 0;
}

static int SaveFs_Read(void* file, uint8_t* ptr, int len)
{
    SaveHandle* h = (SaveHandle*)file;
    int         avail;

    if (h == NULL || h->file == NULL)
        return -1;

    avail = h->file->size - h->pos;
    if (avail <= 0)
        return 0;
    if (len > avail)
        len = avail;

    memcpy(ptr, h->file->data + h->pos, (size_t)len);
    h->pos += len;
    return len;
}

static int SaveFs_Write(void* file, uint8_t* ptr, int len)
{
    SaveHandle* h = (SaveHandle*)file;
    int         room;

    if (h == NULL || h->file == NULL)
        return -1;
    if (!h->writable)
    {
        errno = EBADF;
        return -1;
    }

    room = SAVEFS_MAX_SIZE - h->pos;
    if (len > room)
        len = room;
    if (len <= 0)
    {
        errno = ENOSPC;
        return -1;
    }

    memcpy(h->file->data + h->pos, ptr, (size_t)len);
    h->pos += len;
    /* Only ever grows: a seek-and-overwrite in the middle of the card must not
     * truncate the blocks after it, which is exactly what the save path does. */
    if (h->pos > h->file->size)
        h->file->size = h->pos;
    return len;
}

static int SaveFs_LSeek(void* file, int ptr, int dir)
{
    SaveHandle* h = (SaveHandle*)file;
    int         pos;

    if (h == NULL || h->file == NULL)
        return -1;

    switch (dir)
    {
        case SEEK_SET: pos = ptr; break;
        case SEEK_CUR: pos = h->pos + ptr; break;
        case SEEK_END: pos = h->file->size + ptr; break;
        default: errno = EINVAL; return -1;
    }

    if (pos < 0 || pos > SAVEFS_MAX_SIZE)
    {
        errno = EINVAL;
        return -1;
    }

    h->pos = pos;
    return pos;
}

static int SaveFs_FStat(void* file, struct stat* st)
{
    SaveHandle* h = (SaveHandle*)file;

    if (h == NULL || h->file == NULL || st == NULL)
        return -1;

    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0666;
    st->st_size = h->file->size;
    return 0;
}

static int SaveFs_Stat(char* name, struct stat* st)
{
    SaveFile* f = SaveFs_Find(name, 0);

    if (f == NULL || st == NULL)
    {
        errno = ENOENT;
        return -1;
    }

    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0666;
    st->st_size = f->size;
    return 0;
}

static int SaveFs_Unlink(char* name)
{
    SaveFile* f = SaveFs_Find(name, 0);

    if (f == NULL)
    {
        errno = ENOENT;
        return -1;
    }
    free(f->data);
    memset(f, 0, sizeof(*f));
    return 0;
}

static filesystem_t s_saveFs = {
    .thread_safe = false,
    .open        = SaveFs_Open,
    .fstat       = SaveFs_FStat,
    .stat        = SaveFs_Stat,
    .lseek       = SaveFs_LSeek,
    .read        = SaveFs_Read,
    .write       = SaveFs_Write,
    .close       = SaveFs_Close,
    .unlink      = SaveFs_Unlink,
};

int SaveFs_N64Init(void)
{
    if (s_mounted)
        return 1;

    if (attach_filesystem("sav:/", &s_saveFs) != 0)
    {
        SH_DBG("[SAVEFS] attach_filesystem failed");
        return 0;
    }

    s_mounted = 1;
    SH_DBG("[SAVEFS] RAM save device mounted at sav:/ - saves are LOST at power-off");
    return 1;
}
