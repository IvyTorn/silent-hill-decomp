/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Android second screen: the inventory's item models, read off the disc.
 *
 * The second screen (SecondScreen.java) cannot borrow the game's renderer --
 * the game owns its one SDL/GLES window -- so it draws the item models itself.
 * This file turns what the game would load into something it can draw: plain
 * triangles with their own small RGBA texture.
 *
 * Nothing is shipped and nothing the game uses is touched. The files are read
 * straight out of the player's disc image with Pc_LangReadDiscFile, which opens
 * its own handle, on a worker thread. The choices of WHAT to read are the
 * game's own, reproduced here:
 *
 *   - the model pack, ITEM/IT_00x.TMD, per map (GameFs_MapItemsTextureLoad);
 *   - the texture TIMs: TIM07 always, TIM00 for the common items, and one of
 *     TIM01..06 per map (GameFs_MapItemsModelLoad, GameFs_Tim00TIMLoad);
 *   - where each TIM lands in VRAM, from the same s_FsImageDesc the game loads
 *     it with (Fs_QueueStartReadTim) -- including the PAL palette homes that
 *     Font_ApplyRegionPatches installs, which the PAL TMDs point at;
 *   - which object of the pack an item is: its index in the current map's
 *     loadableItems list (Gfx_Items_Display / GsGetTMDObject).
 *
 * The TMD packet shapes are the ones the port's own validator accepts for the
 * disc's item packs (pc_big_tmd.c): gouraud tris 0x30/0x34/0x36 and quads
 * 0x38/0x3C, each with interleaved (normal, vertex) index pairs.
 *
 * Step 1 of the 3D second screen: this builds the data and can dump it for
 * checking; nothing draws it yet.
 */
#include "game.h"

#include "bodyprog/bodyprog.h"
#include "bodyprog/items.h"
#include "bodyprog/map/map.h"
#include "bodyprog/savegame.h"
#include "main/fileinfo.h"
#include "main/fsqueue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* Pure decoding: no game state, no threads. Host-testable.               */
/* ---------------------------------------------------------------------- */

#define SM_VRAM_W 1024
#define SM_VRAM_H 512

#define SM_TMD_HDR   12 /* id, flags, nobj */
#define SM_TMD_OBJ   28 /* vertop, vern, nortop, norn, primtop, primn, scale */

#define SM_VERT_BYTES 16
#define SM_ITEM_HDR   20

#define SM_ATLAS_W    256
#define SM_REGION_MAX 64

/* Vertex flag bits in the blob. */
#define SM_VF_TEXTURED 0x01
#define SM_VF_SEMI     0x02 /* ABE; the blend mode is in bits 2-3 */

static unsigned Sm_U16(const unsigned char* p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long Sm_U32(const unsigned char* p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
           ((unsigned long)p[3] << 24);
}

static void Sm_Put16(unsigned char* p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

/* Place one TIM in vram[] the way Fs_QueueStartReadTim does: the pixel rect at
 * the descriptor's page + (u, v) unless u is 0xFF, the CLUT at (clutX, clutY)
 * unless clutX is -1 -- the TIM's own header coordinates otherwise.
 * Returns 0 on a malformed file. */
int Sm_PlaceTim(unsigned short* vram, const unsigned char* tim, long size, const s_FsImageDesc* desc)
{
    unsigned long flags;
    long          p = 8;

    if (size < 8 || Sm_U32(tim) != 0x10)
        return 0;

    flags = Sm_U32(tim + 4);

    if (flags & 0x8)
    {
        unsigned long bnum;
        int           x, y, w, h, row;

        if (p + 12 > size)
            return 0;
        bnum = Sm_U32(tim + p);
        x    = (int)Sm_U16(tim + p + 4);
        y    = (int)Sm_U16(tim + p + 6);
        w    = (int)Sm_U16(tim + p + 8);
        h    = (int)Sm_U16(tim + p + 10);
        if (desc != NULL && desc->clutX != NO_VALUE)
        {
            x = desc->clutX;
            y = desc->clutY;
        }
        if (bnum < 12 || p + (long)bnum > size || 12 + (long)w * h * 2 > (long)bnum)
            return 0;
        for (row = 0; row < h; row++)
        {
            int col;
            for (col = 0; col < w; col++)
            {
                int vx = x + col, vy = y + row;
                if (vx >= 0 && vx < SM_VRAM_W && vy >= 0 && vy < SM_VRAM_H)
                    vram[vy * SM_VRAM_W + vx] = (unsigned short)Sm_U16(tim + p + 12 + (row * w + col) * 2);
            }
        }
        p += (long)bnum;
    }

    {
        unsigned long bnum;
        int           x, y, w, h, row;

        if (p + 12 > size)
            return 0;
        bnum = Sm_U32(tim + p);
        x    = (int)Sm_U16(tim + p + 4);
        y    = (int)Sm_U16(tim + p + 6);
        w    = (int)Sm_U16(tim + p + 8);
        h    = (int)Sm_U16(tim + p + 10);
        if (desc != NULL && desc->u != 0xFF)
        {
            x = desc->u + ((desc->tPage[1] & 0xF) << 6);
            y = desc->v + ((desc->tPage[1] << 4) & 0x100);
        }
        if (bnum < 12 || p + (long)bnum > size || 12 + (long)w * h * 2 > (long)bnum)
            return 0;
        for (row = 0; row < h; row++)
        {
            int col;
            for (col = 0; col < w; col++)
            {
                int vx = x + col, vy = y + row;
                if (vx >= 0 && vx < SM_VRAM_W && vy >= 0 && vy < SM_VRAM_H)
                    vram[vy * SM_VRAM_W + vx] = (unsigned short)Sm_U16(tim + p + 12 + (row * w + col) * 2);
            }
        }
    }
    return 1;
}

/* One texel the way the GPU would fetch it for a prim with this tpage and
 * clut word, as RGBA. Colour word 0 is the PSX's "do not draw". */
static unsigned long Sm_Texel(const unsigned short* vram, unsigned tpage, unsigned clut, int u, int v)
{
    int            px    = (int)(tpage & 0xF) * 64;
    int            py    = (tpage & 0x10) ? 256 : 0;
    int            depth = (int)((tpage >> 7) & 3);
    int            cx    = (int)(clut & 0x3F) * 16;
    int            cy    = (int)((clut >> 6) & 0x1FF);
    unsigned short c;
    unsigned       r, g, b;

    u &= 0xFF;
    v &= 0xFF;
    if (py + v >= SM_VRAM_H)
        return 0;

    if (depth == 0)
    {
        unsigned short w   = vram[(py + v) * SM_VRAM_W + ((px + (u >> 2)) & (SM_VRAM_W - 1))];
        int            idx = (w >> ((u & 3) * 4)) & 0xF;
        c                  = vram[cy * SM_VRAM_W + ((cx + idx) & (SM_VRAM_W - 1))];
    }
    else if (depth == 1)
    {
        unsigned short w   = vram[(py + v) * SM_VRAM_W + ((px + (u >> 1)) & (SM_VRAM_W - 1))];
        int            idx = (w >> ((u & 1) * 8)) & 0xFF;
        c                  = vram[cy * SM_VRAM_W + ((cx + idx) & (SM_VRAM_W - 1))];
    }
    else
    {
        c = vram[(py + v) * SM_VRAM_W + ((px + u) & (SM_VRAM_W - 1))];
    }

    if (c == 0)
        return 0;

    r = c & 31;
    g = (c >> 5) & 31;
    b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    /* Alpha 0xFE marks the STP bit: drawn, but blended when the prim is ABE. */
    return (unsigned long)r | ((unsigned long)g << 8) | ((unsigned long)b << 16) |
           ((unsigned long)((c & 0x8000) ? 0xFE : 0xFF) << 24);
}

typedef struct
{
    unsigned tpage, clut;
    int      u0, v0, u1, v1; /* inclusive texel bounds */
    int      ax, ay;         /* where the region (inside its 1-texel border) lands */
} s_SmRegion;

typedef struct
{
    int      vi[4];
    int      quad;
    int      textured;
    unsigned tpage, clut;
    int      u[4], v[4];
    unsigned char rgb[3];
    unsigned char flags;
} s_SmPrim;

/* Shape of one shipped packet, or 0 if this is not one. */
static int Sm_PrimShape(unsigned mode, unsigned ilen, int* quad, int* textured)
{
    switch (mode)
    {
        case 0x30: *quad = 0; *textured = 0; return ilen == 4;
        case 0x34:
        case 0x36: *quad = 0; *textured = 1; return ilen == 6;
        case 0x38: *quad = 1; *textured = 0; return ilen == 5;
        case 0x3C:
        case 0x3E: *quad = 1; *textured = 1; return ilen == 8;
        default:   return 0;
    }
}

static int Sm_RegionFor(s_SmRegion* regions, int* count, const s_SmPrim* pr)
{
    int i, k, n = pr->quad ? 4 : 3;

    for (i = 0; i < *count; i++)
    {
        if (regions[i].tpage == pr->tpage && regions[i].clut == pr->clut)
            break;
    }
    if (i == *count)
    {
        if (*count >= SM_REGION_MAX)
            return -1;
        regions[i].tpage = pr->tpage;
        regions[i].clut  = pr->clut;
        regions[i].u0 = regions[i].v0 = 255;
        regions[i].u1 = regions[i].v1 = 0;
        (*count)++;
    }
    for (k = 0; k < n; k++)
    {
        if (pr->u[k] < regions[i].u0) regions[i].u0 = pr->u[k];
        if (pr->v[k] < regions[i].v0) regions[i].v0 = pr->v[k];
        if (pr->u[k] > regions[i].u1) regions[i].u1 = pr->u[k];
        if (pr->v[k] > regions[i].v1) regions[i].v1 = pr->v[k];
    }
    return i;
}

/* Build one item's record for the blob from object `obj` of the TMD pack.
 *
 * Record: itemId u8, obj u8, triangle count u16, texture w u16, h u16,
 * bounding box 6 x s16 (min xyz, max xyz); then 3 vertices per triangle of
 * SM_VERT_BYTES (x, y, z s16; flags u8; pad u8; u, v u16 texel coordinates in
 * the record's texture; r, g, b, a u8, where 255 = the texel unchanged); then
 * w*h RGBA texels. A one-texel white cell at (1,1) serves the untextured
 * triangles. Returns the bytes written, or 0 if the object is unusable or
 * `cap` is too small. */
long Sm_BuildItem(unsigned char* out, long cap, int itemId, const unsigned char* tmd, long tmdSize, int obj,
                  const unsigned short* vram)
{
    static s_SmPrim   prims[2048];
    static s_SmRegion regions[SM_REGION_MAX];

    const unsigned char* ot;
    unsigned long        vertop, vern, primtop, primn;
    long                 p;
    int                  nprim = 0, nreg = 0, ntri = 0;
    int                  i, k;
    int                  texW = SM_ATLAS_W, texH;
    int                  shelfX, shelfY, shelfH;
    int                  bb[6] = { 32767, 32767, 32767, -32768, -32768, -32768 };
    unsigned char*       w;
    long                 need;

    if (tmdSize < SM_TMD_HDR || Sm_U32(tmd) != 0x41)
        return 0;
    if (obj < 0 || (unsigned long)obj >= Sm_U32(tmd + 8) || SM_TMD_HDR + (long)(obj + 1) * SM_TMD_OBJ > tmdSize)
        return 0;

    ot      = tmd + SM_TMD_HDR + obj * SM_TMD_OBJ;
    vertop  = Sm_U32(ot + 0);
    vern    = Sm_U32(ot + 4);
    primtop = Sm_U32(ot + 16);
    primn   = Sm_U32(ot + 20);

    /* Offsets are from the object table, the file + 12 (pc_big_tmd.c). */
    if (SM_TMD_HDR + (long)vertop + (long)vern * 8 > tmdSize)
        return 0;

    p = SM_TMD_HDR + (long)primtop;
    for (i = 0; i < (int)primn; i++)
    {
        const unsigned char* pk;
        unsigned             ilen, flag, mode;
        s_SmPrim*            pr;
        int                  quad, textured, n, base;

        if (p + 4 > tmdSize)
            return 0;
        pk   = tmd + p;
        ilen = pk[1];
        flag = pk[2];
        mode = pk[3];
        if (ilen == 0 || p + 4 + (long)ilen * 4 > tmdSize)
            return 0;
        p += 4 + (long)ilen * 4;

        if (!Sm_PrimShape(mode, ilen, &quad, &textured) || nprim >= (int)(sizeof(prims) / sizeof(prims[0])))
            continue;

        pr           = &prims[nprim];
        pr->quad     = quad;
        pr->textured = textured;
        pr->flags    = (unsigned char)((textured ? SM_VF_TEXTURED : 0) | ((mode & 2) ? SM_VF_SEMI : 0));
        n            = quad ? 4 : 3;
        (void)flag;

        if (textured)
        {
            for (k = 0; k < n; k++)
            {
                pr->u[k] = pk[4 + k * 4];
                pr->v[k] = pk[5 + k * 4];
            }
            pr->clut  = Sm_U16(pk + 6);
            pr->tpage = Sm_U16(pk + 10);
            pr->flags |= (unsigned char)(((pr->tpage >> 5) & 3) << 2);
            pr->rgb[0] = pr->rgb[1] = pr->rgb[2] = 0x80;
            base       = 4 + n * 4;
        }
        else
        {
            pr->rgb[0] = pk[4];
            pr->rgb[1] = pk[5];
            pr->rgb[2] = pk[6];
            pr->tpage = pr->clut = 0;
            for (k = 0; k < 4; k++)
                pr->u[k] = pr->v[k] = 0;
            base = 8;
        }

        /* (normal, vertex) pairs; the normal is not used. */
        for (k = 0; k < n; k++)
        {
            pr->vi[k] = (int)Sm_U16(pk + base + k * 4 + 2);
            if ((unsigned long)pr->vi[k] >= vern)
                break;
        }
        if (k < n)
            continue;

        if (textured && Sm_RegionFor(regions, &nreg, pr) < 0)
            continue;

        ntri += quad ? 2 : 1;
        nprim++;
    }

    if (ntri == 0)
        return 0;

    /* Shelf-pack the texture regions, each with a one-texel border, after a
     * 3x3 white cell for the untextured triangles. */
    shelfX = 3;
    shelfY = 0;
    shelfH = 3;
    for (i = 0; i < nreg; i++)
    {
        int rw = regions[i].u1 - regions[i].u0 + 1 + 2;
        int rh = regions[i].v1 - regions[i].v0 + 1 + 2;

        if (rw > texW)
            texW = rw;
        if (shelfX + rw > texW)
        {
            shelfY += shelfH;
            shelfX = 0;
            shelfH = 0;
        }
        regions[i].ax = shelfX + 1;
        regions[i].ay = shelfY + 1;
        shelfX += rw;
        if (rh > shelfH)
            shelfH = rh;
    }
    texH = (shelfY + shelfH + 3) & ~3;

    need = SM_ITEM_HDR + (long)ntri * 3 * SM_VERT_BYTES + (long)texW * texH * 4;
    if (need > cap)
        return 0;

    memset(out, 0, (size_t)need);
    out[0] = (unsigned char)itemId;
    out[1] = (unsigned char)obj;
    Sm_Put16(out + 2, (unsigned)ntri);
    Sm_Put16(out + 4, (unsigned)texW);
    Sm_Put16(out + 6, (unsigned)texH);

    w = out + SM_ITEM_HDR;
    for (i = 0; i < nprim; i++)
    {
        static const int TRI[2][3] = { { 0, 1, 2 }, { 1, 3, 2 } };
        const s_SmPrim*  pr        = &prims[i];
        int              reg       = pr->textured ? Sm_RegionFor(regions, &nreg, pr) : -1;
        int              t;

        for (t = 0; t < (pr->quad ? 2 : 1); t++)
        {
            for (k = 0; k < 3; k++)
            {
                int                  c  = TRI[t][k];
                const unsigned char* vp = tmd + SM_TMD_HDR + vertop + (long)pr->vi[c] * 8;
                int                  x  = (short)Sm_U16(vp);
                int                  y  = (short)Sm_U16(vp + 2);
                int                  z  = (short)Sm_U16(vp + 4);
                int                  tu, tv;

                Sm_Put16(w + 0, (unsigned)x & 0xFFFF);
                Sm_Put16(w + 2, (unsigned)y & 0xFFFF);
                Sm_Put16(w + 4, (unsigned)z & 0xFFFF);
                w[6] = pr->flags;
                if (reg >= 0)
                {
                    tu = regions[reg].ax + (pr->u[c] - regions[reg].u0);
                    tv = regions[reg].ay + (pr->v[c] - regions[reg].v0);
                }
                else
                {
                    tu = 1;
                    tv = 1;
                }
                Sm_Put16(w + 8, (unsigned)tu);
                Sm_Put16(w + 10, (unsigned)tv);
                /* PSX modulation: 0x80 leaves a texel as it is, so a textured
                 * prim's neutral colour becomes 255 here; an untextured one
                 * draws its colour straight. */
                if (pr->textured)
                {
                    w[12] = (unsigned char)(pr->rgb[0] >= 0x80 ? 255 : pr->rgb[0] * 2);
                    w[13] = (unsigned char)(pr->rgb[1] >= 0x80 ? 255 : pr->rgb[1] * 2);
                    w[14] = (unsigned char)(pr->rgb[2] >= 0x80 ? 255 : pr->rgb[2] * 2);
                }
                else
                {
                    w[12] = pr->rgb[0];
                    w[13] = pr->rgb[1];
                    w[14] = pr->rgb[2];
                }
                w[15] = 255;
                w += SM_VERT_BYTES;

                if (x < bb[0]) bb[0] = x;
                if (y < bb[1]) bb[1] = y;
                if (z < bb[2]) bb[2] = z;
                if (x > bb[3]) bb[3] = x;
                if (y > bb[4]) bb[4] = y;
                if (z > bb[5]) bb[5] = z;
            }
        }
    }
    for (k = 0; k < 6; k++)
        Sm_Put16(out + 8 + k * 2, (unsigned)bb[k] & 0xFFFF);

    /* Texture: white cell, then each region with its border texels repeated
     * so a filtered or rounded fetch at an edge stays inside its own image. */
    {
        unsigned char* tex = w;
        int            y, x;

        for (y = 0; y < 3; y++)
            for (x = 0; x < 3; x++)
                memset(tex + ((long)y * texW + x) * 4, 0xFF, 4);

        for (i = 0; i < nreg; i++)
        {
            const s_SmRegion* r  = &regions[i];
            int               rw = r->u1 - r->u0 + 1;
            int               rh = r->v1 - r->v0 + 1;

            for (y = -1; y <= rh; y++)
            {
                for (x = -1; x <= rw; x++)
                {
                    int           su = r->u0 + (x < 0 ? 0 : (x >= rw ? rw - 1 : x));
                    int           sv = r->v0 + (y < 0 ? 0 : (y >= rh ? rh - 1 : y));
                    unsigned long c  = Sm_Texel(vram, r->tpage, r->clut, su, sv);
                    unsigned char* d = tex + ((long)(r->ay + y) * texW + (r->ax + x)) * 4;

                    d[0] = (unsigned char)(c & 0xFF);
                    d[1] = (unsigned char)((c >> 8) & 0xFF);
                    d[2] = (unsigned char)((c >> 16) & 0xFF);
                    d[3] = (unsigned char)((c >> 24) & 0xFF);
                }
            }
        }
    }

    return need;
}

/* The pack a map shows in its inventory: GameFs_MapItemsTextureLoad. */
int Sm_PackForMap(int map)
{
    switch (map)
    {
        case MapIdx_MAP0_S00:
            return 0;

        case MapIdx_MAP0_S01: case MapIdx_MAP0_S02: case MapIdx_MAP1_S06:
        case MapIdx_MAP2_S00: case MapIdx_MAP2_S01: case MapIdx_MAP2_S02: case MapIdx_MAP2_S04:
        case MapIdx_MAP4_S00: case MapIdx_MAP4_S01: case MapIdx_MAP4_S02: case MapIdx_MAP4_S03:
        case MapIdx_MAP4_S05:
            return 1;

        case MapIdx_MAP1_S00: case MapIdx_MAP1_S01: case MapIdx_MAP1_S02: case MapIdx_MAP1_S03:
        case MapIdx_MAP1_S05:
            return 2;

        case MapIdx_MAP3_S00: case MapIdx_MAP3_S01: case MapIdx_MAP3_S02: case MapIdx_MAP3_S03:
        case MapIdx_MAP3_S04: case MapIdx_MAP3_S05: case MapIdx_MAP3_S06: case MapIdx_MAP4_S04:
            return 3;

        case MapIdx_MAP5_S00: case MapIdx_MAP5_S01: case MapIdx_MAP5_S02: case MapIdx_MAP5_S03:
        case MapIdx_MAP6_S00: case MapIdx_MAP6_S01: case MapIdx_MAP6_S02: case MapIdx_MAP6_S03:
        case MapIdx_MAP6_S04:
            return 4;

        case MapIdx_MAP7_S00: case MapIdx_MAP7_S01:
            return 5;

        case MapIdx_MAP7_S02: case MapIdx_MAP7_S03:
            return 6;

        default:
            return -1;
    }
}

/* The per-map key-item texture, TIM01..06: GameFs_MapItemsModelLoad. Note
 * the groups are not the pack's -- MAP1 is pack 2 but texture 1. */
int Sm_KeyTimForMap(int map)
{
    switch (map)
    {
        case MapIdx_MAP1_S00: case MapIdx_MAP1_S01: case MapIdx_MAP1_S02: case MapIdx_MAP1_S03:
        case MapIdx_MAP1_S05:
            return 1;

        case MapIdx_MAP0_S01: case MapIdx_MAP0_S02: case MapIdx_MAP1_S06:
        case MapIdx_MAP2_S00: case MapIdx_MAP2_S01: case MapIdx_MAP2_S02: case MapIdx_MAP2_S04:
        case MapIdx_MAP4_S00: case MapIdx_MAP4_S01: case MapIdx_MAP4_S02: case MapIdx_MAP4_S03:
        case MapIdx_MAP4_S05:
            return 2;

        case MapIdx_MAP3_S00: case MapIdx_MAP3_S01: case MapIdx_MAP3_S02: case MapIdx_MAP3_S03:
        case MapIdx_MAP3_S04: case MapIdx_MAP3_S05: case MapIdx_MAP3_S06: case MapIdx_MAP4_S04:
            return 3;

        case MapIdx_MAP5_S00: case MapIdx_MAP5_S01: case MapIdx_MAP5_S02: case MapIdx_MAP5_S03:
        case MapIdx_MAP6_S00: case MapIdx_MAP6_S01: case MapIdx_MAP6_S02: case MapIdx_MAP6_S03:
        case MapIdx_MAP6_S04:
            return 4;

        case MapIdx_MAP7_S00: case MapIdx_MAP7_S01:
            return 5;

        case MapIdx_MAP7_S02: case MapIdx_MAP7_S03:
            return 6;

        default:
            return 0;
    }
}

/* ---------------------------------------------------------------------- */
/* Android: the worker, the dump and JNI.                                 */
/* ---------------------------------------------------------------------- */

#if defined(__ANDROID__)

#include <jni.h>
#include <pthread.h>
#include <sys/stat.h>
#include <android/log.h>

#include "lang_text.h" /* Pc_LangReadDiscFile */

#define SM_TAG       "SH2Screen"
#define SM_LIST_MAX  64
#define SM_BLOB_HDR  16

typedef struct
{
    int           map;
    int           pack;
    int           keyTim;
    int           count;
    unsigned char items[SM_LIST_MAX];
    s_FsImageDesc descKey, descCommon, descAlways;
    unsigned long sector[4], size[4]; /* pack, TIM07, TIM00, key TIM */
    char          dumpDir[512];
} s_SmJob;

static pthread_mutex_t s_smLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char*  s_smBlob;
static long            s_smBlobLen;
static int             s_smSerial;
static int             s_smBusy;
static char            s_smDumpDir[512];
static int             s_smForce;

static int             s_smLastMap = -2;
static const void*     s_smLastList;

static void Sm_WriteFile(const char* dir, const char* name, const void* data, long len)
{
    char  path[700];
    FILE* f;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (f == NULL)
    {
        __android_log_print(ANDROID_LOG_WARN, SM_TAG, "models: cannot write %s", path);
        return;
    }
    fwrite(data, 1, (size_t)len, f);
    fclose(f);
}

static void Sm_DumpItem(const char* dir, const unsigned char* rec, long len)
{
    extern void* tdefl_write_image_to_png_file_in_memory(const void*, int, int, int, size_t*);
    extern void  mz_free(void*);

    int                  item = rec[0];
    unsigned             ntri = Sm_U16(rec + 2);
    int                  tw   = (int)Sm_U16(rec + 4);
    int                  th   = (int)Sm_U16(rec + 6);
    const unsigned char* v    = rec + SM_ITEM_HDR;
    const unsigned char* tex  = v + (long)ntri * 3 * SM_VERT_BYTES;
    char                 name[64];
    char                 path[700];
    size_t               pngLen = 0;
    void*                png;
    FILE*                f;
    unsigned             i;

    (void)len;

    png = tdefl_write_image_to_png_file_in_memory(tex, tw, th, 4, &pngLen);
    if (png != NULL)
    {
        snprintf(name, sizeof(name), "item_%03d.png", item);
        Sm_WriteFile(dir, name, png, (long)pngLen);
        mz_free(png);
    }

    /* Wavefront OBJ: opens in most 3D viewers, and is what gets checked
     * against the original on a PC. PSX y points down, hence the flip. */
    snprintf(path, sizeof(path), "%s/item_%03d.obj", dir, item);
    f = fopen(path, "w");
    if (f == NULL)
        return;
    fprintf(f, "mtllib item_%03d.mtl\nusemtl m\n", item);
    for (i = 0; i < ntri * 3; i++)
    {
        const unsigned char* p = v + (long)i * SM_VERT_BYTES;
        fprintf(f, "v %d %d %d %.3f %.3f %.3f\n", (short)Sm_U16(p), -(short)Sm_U16(p + 2), -(short)Sm_U16(p + 4),
                p[12] / 255.0, p[13] / 255.0, p[14] / 255.0);
    }
    for (i = 0; i < ntri * 3; i++)
    {
        const unsigned char* p = v + (long)i * SM_VERT_BYTES;
        fprintf(f, "vt %.5f %.5f\n", (Sm_U16(p + 8) + 0.5) / tw, 1.0 - (Sm_U16(p + 10) + 0.5) / th);
    }
    for (i = 0; i < ntri; i++)
        fprintf(f, "f %u/%u %u/%u %u/%u\n", i * 3 + 1, i * 3 + 1, i * 3 + 2, i * 3 + 2, i * 3 + 3, i * 3 + 3);
    fclose(f);

    snprintf(path, sizeof(path), "%s/item_%03d.mtl", dir, item);
    f = fopen(path, "w");
    if (f != NULL)
    {
        fprintf(f, "newmtl m\nKd 1 1 1\nmap_Kd item_%03d.png\n", item);
        fclose(f);
    }
}

static void* Sm_Worker(void* arg)
{
    static const char* const TIM_NAME[4] = { "pack", "TIM07.TIM", "TIM00.TIM", "key.TIM" };

    s_SmJob*        job  = (s_SmJob*)arg;
    unsigned char*  file[4] = { NULL, NULL, NULL, NULL };
    unsigned short* vram = NULL;
    unsigned char*  blob = NULL;
    long            cap  = 4L * 1024 * 1024;
    long            len  = SM_BLOB_HDR;
    int             i, built = 0;
    const char*     dump = NULL;
    char            dumpMap[600];

    /* One folder per map, so walking through a few areas collects them all. */
    if (job->dumpDir[0])
    {
        snprintf(dumpMap, sizeof(dumpMap), "%s/map_%02d", job->dumpDir, job->map);
        mkdir(dumpMap, 0775);
        dump = dumpMap;
    }

    for (i = 0; i < 4; i++)
    {
        if (job->size[i] > 0)
            file[i] = Pc_LangReadDiscFile((unsigned)job->sector[i], (unsigned)job->size[i]);
    }

    if (file[0] == NULL)
    {
        __android_log_print(ANDROID_LOG_WARN, SM_TAG, "models: could not read pack IT_%03d from the disc", job->pack);
        goto done;
    }

    vram = (unsigned short*)calloc((size_t)SM_VRAM_W * SM_VRAM_H, sizeof(unsigned short));
    blob = (unsigned char*)malloc((size_t)cap);
    if (vram == NULL || blob == NULL)
        goto done;

    if (file[1] && !Sm_PlaceTim(vram, file[1], (long)job->size[1], &job->descAlways))
        __android_log_print(ANDROID_LOG_WARN, SM_TAG, "models: TIM07 not a TIM");
    if (file[2] && !Sm_PlaceTim(vram, file[2], (long)job->size[2], &job->descCommon))
        __android_log_print(ANDROID_LOG_WARN, SM_TAG, "models: TIM00 not a TIM");
    if (file[3] && !Sm_PlaceTim(vram, file[3], (long)job->size[3], &job->descKey))
        __android_log_print(ANDROID_LOG_WARN, SM_TAG, "models: TIM%02d not a TIM", job->keyTim);

    memset(blob, 0, SM_BLOB_HDR);
    blob[0] = 'S';
    blob[1] = 'H';
    blob[2] = 'M';
    blob[3] = '1';
    blob[4] = 1;
    blob[5] = (unsigned char)job->map;
    blob[6] = (unsigned char)job->pack;

    if (dump != NULL)
    {
        char name[32];

        snprintf(name, sizeof(name), "IT_%03d.TMD", job->pack);
        Sm_WriteFile(dump, name, file[0], (long)job->size[0]);
        for (i = 1; i < 4; i++)
        {
            if (file[i] == NULL)
                continue;
            if (i == 3)
                snprintf(name, sizeof(name), "TIM%02d.TIM", job->keyTim);
            else
                snprintf(name, sizeof(name), "%s", TIM_NAME[i]);
            Sm_WriteFile(dump, name, file[i], (long)job->size[i]);
        }
        Sm_WriteFile(dump, "vram.bin", vram, (long)SM_VRAM_W * SM_VRAM_H * 2);
    }

    for (i = 0; i < job->count; i++)
    {
        long n = Sm_BuildItem(blob + len, cap - len, job->items[i], file[0], (long)job->size[0], i, vram);

        if (n <= 0)
        {
            __android_log_print(ANDROID_LOG_INFO, SM_TAG, "models: item %d (object %d) has no usable model",
                                job->items[i], i);
            continue;
        }
        __android_log_print(ANDROID_LOG_INFO, SM_TAG, "models: item %d = object %d, %u triangles, texture %ux%u",
                            job->items[i], i, Sm_U16(blob + len + 2), Sm_U16(blob + len + 4), Sm_U16(blob + len + 6));
        if (dump != NULL)
            Sm_DumpItem(dump, blob + len, n);
        len += n;
        built++;
    }
    Sm_Put16(blob + 8, (unsigned)built);

    if (dump != NULL)
    {
        char  path[700];
        FILE* f;

        snprintf(path, sizeof(path), "%s/items.txt", dump);
        f = fopen(path, "w");
        if (f != NULL)
        {
            extern const char* Pc_Inventory_ItemName(u8 id);

            fprintf(f, "map %d  pack IT_%03d.TMD  key texture TIM%02d.TIM  region %d\n", job->map, job->pack,
                    job->keyTim, (int)g_GameRegion);
            fprintf(f, "TIM07 desc tpage %d u %d v %d clut %d,%d\n", job->descAlways.tPage[1], job->descAlways.u,
                    job->descAlways.v, job->descAlways.clutX, job->descAlways.clutY);
            fprintf(f, "TIM00 desc tpage %d u %d v %d clut %d,%d\n", job->descCommon.tPage[1], job->descCommon.u,
                    job->descCommon.v, job->descCommon.clutX, job->descCommon.clutY);
            fprintf(f, "key   desc tpage %d u %d v %d clut %d,%d\n", job->descKey.tPage[1], job->descKey.u,
                    job->descKey.v, job->descKey.clutX, job->descKey.clutY);
            for (i = 0; i < job->count; i++)
                fprintf(f, "object %2d  item %3d  %s\n", i, job->items[i], Pc_Inventory_ItemName(job->items[i]));
            fclose(f);
        }
        Sm_WriteFile(dump, "models.bin", blob, len);
        __android_log_print(ANDROID_LOG_INFO, SM_TAG, "models: dumped to %s", dump);
    }

    pthread_mutex_lock(&s_smLock);
    free(s_smBlob);
    s_smBlob    = blob;
    s_smBlobLen = len;
    s_smSerial  = (s_smSerial % 255) + 1;
    pthread_mutex_unlock(&s_smLock);
    blob = NULL;

    __android_log_print(ANDROID_LOG_INFO, SM_TAG, "models: map %d, pack IT_%03d, %d of %d items built, %ld bytes",
                        job->map, job->pack, built, job->count, len);

done:
    for (i = 0; i < 4; i++)
        free(file[i]);
    free(vram);
    free(blob);
    free(job);

    pthread_mutex_lock(&s_smLock);
    s_smBusy = 0;
    pthread_mutex_unlock(&s_smLock);
    return NULL;
}

static void Sm_FileOf(s_SmJob* job, int slot, int fileIdx)
{
    job->sector[slot] = g_FileTable[fileIdx].startSector;
    job->size[slot]   = (unsigned long)g_FileTable[fileIdx].blockCount * 256;
}

/* Game thread, every frame (pc_second_screen.c). Starts a rebuild when the
 * map -- and with it the item list -- has changed. */
void Pc_SecondScreenModels_Tick(void)
{
    static const int PACK_FILE[7] = {
        FILE_ITEM_IT_000_TMD, FILE_ITEM_IT_001_TMD, FILE_ITEM_IT_002_TMD, FILE_ITEM_IT_003_TMD,
        FILE_ITEM_IT_004_TMD, FILE_ITEM_IT_005_TMD, FILE_ITEM_IT_006_TMD
    };
    static const int KEY_FILE[7] = {
        0, FILE_ITEM_TIM01_TIM, FILE_ITEM_TIM02_TIM, FILE_ITEM_TIM03_TIM,
        FILE_ITEM_TIM04_TIM, FILE_ITEM_TIM05_TIM, FILE_ITEM_TIM06_TIM
    };

    s_SmJob*        job;
    const u8*       list;
    int             map, pack, i, busy;
    pthread_t       th;
    pthread_attr_t  attr;

    if (g_GameWork.gameState != GameState_InGame || g_pMapOverlayHeader == NULL || g_SavegamePtr == NULL)
        return;

    map  = g_SavegamePtr->mapIdx;
    list = g_pMapOverlayHeader->loadableItems;

    pthread_mutex_lock(&s_smLock);
    busy = s_smBusy;
    if (!busy && s_smForce)
    {
        s_smForce   = 0;
        s_smLastMap = -2;
    }
    pthread_mutex_unlock(&s_smLock);

    if (busy || (map == s_smLastMap && (const void*)list == s_smLastList))
        return;

    s_smLastMap  = map;
    s_smLastList = list;

    pack = Sm_PackForMap(map);
    if (pack < 0 || list == NULL)
        return;

    job = (s_SmJob*)calloc(1, sizeof(*job));
    if (job == NULL)
        return;

    job->map    = map;
    job->pack   = pack;
    job->keyTim = Sm_KeyTimForMap(map);
    for (i = 0; i < SM_LIST_MAX && list[i] != 0; i++)
        job->items[i] = list[i];
    job->count = i;

    /* Copies, taken here on the game thread: the descriptors carry the PAL
     * palette homes Font_ApplyRegionPatches installed. */
    job->descKey    = g_InventoryKeyItemTextureImg;
    job->descCommon = g_FirstAidKitItemTextureImg;
    job->descAlways = D_800A9074;

    Sm_FileOf(job, 0, PACK_FILE[pack]);
    Sm_FileOf(job, 1, FILE_ITEM_TIM07_TIM);
    Sm_FileOf(job, 2, FILE_ITEM_TIM00_TIM);
    if (job->keyTim > 0)
        Sm_FileOf(job, 3, KEY_FILE[job->keyTim]);

    pthread_mutex_lock(&s_smLock);
    snprintf(job->dumpDir, sizeof(job->dumpDir), "%s", s_smDumpDir);
    s_smBusy = 1;
    pthread_mutex_unlock(&s_smLock);

    if (job->count == 0)
    {
        __android_log_print(ANDROID_LOG_INFO, SM_TAG, "models: map %d lists no inventory models", map);
    }

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, Sm_Worker, job) != 0)
    {
        free(job);
        pthread_mutex_lock(&s_smLock);
        s_smBusy = 0;
        pthread_mutex_unlock(&s_smLock);
    }
    pthread_attr_destroy(&attr);
}

int Pc_SecondScreenModels_Serial(void)
{
    int s;

    pthread_mutex_lock(&s_smLock);
    s = s_smSerial;
    pthread_mutex_unlock(&s_smLock);
    return s;
}

/* UI thread: where the dump goes, or null for no dump. Takes effect at the
 * next map change; a set directory also forces a rebuild of the current map. */
JNIEXPORT void JNICALL
Java_com_silenthill_port_SecondScreen_nativeModelsConfigure(JNIEnv* env, jclass cls, jstring jDir)
{
    const char* dir = (jDir != NULL) ? (*env)->GetStringUTFChars(env, jDir, NULL) : NULL;

    (void)cls;

    pthread_mutex_lock(&s_smLock);
    snprintf(s_smDumpDir, sizeof(s_smDumpDir), "%s", dir != NULL ? dir : "");
    if (dir != NULL)
        s_smForce = 1;
    pthread_mutex_unlock(&s_smLock);

    if (dir != NULL)
        (*env)->ReleaseStringUTFChars(env, jDir, dir);
}

/* UI thread: the current model blob, or null. */
JNIEXPORT jbyteArray JNICALL
Java_com_silenthill_port_SecondScreen_nativeModels(JNIEnv* env, jclass cls)
{
    jbyteArray arr = NULL;

    (void)cls;

    pthread_mutex_lock(&s_smLock);
    if (s_smBlob != NULL && s_smBlobLen > 0)
    {
        arr = (*env)->NewByteArray(env, (jsize)s_smBlobLen);
        if (arr != NULL)
            (*env)->SetByteArrayRegion(env, arr, 0, (jsize)s_smBlobLen, (const jbyte*)s_smBlob);
    }
    pthread_mutex_unlock(&s_smLock);
    return arr;
}

#else

void Pc_SecondScreenModels_Tick(void)
{
}

int Pc_SecondScreenModels_Serial(void)
{
    return 0;
}

#endif
