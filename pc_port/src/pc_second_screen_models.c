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
 * Step 2 of the 3D second screen: every pack is built once into a library,
 * so an item has its model wherever the player is, and each model is also
 * drawn into a still icon for the item list. The models themselves are kept
 * for the real-time view that comes next.
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
    int                  texW = SM_ATLAS_W, texH = 0;
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
     * 3x3 white cell for the untextured triangles. The width is whichever of
     * 64, 128 or 256 gives the smallest texture: most items use a handful of
     * small patches, and a fixed 256 left most of every texture empty. */
    {
        static const int WIDTHS[3] = { 64, 128, 256 };
        int              best = -1, bestArea = 0, pass;

        for (pass = 0; pass < 4; pass++)
        {
            int cand = (pass < 3) ? WIDTHS[pass] : WIDTHS[best];
            int ok   = 1;

            shelfX = 3;
            shelfY = 0;
            shelfH = 3;
            for (i = 0; i < nreg; i++)
            {
                int rw = regions[i].u1 - regions[i].u0 + 1 + 2;
                int rh = regions[i].v1 - regions[i].v0 + 1 + 2;

                if (rw > cand)
                {
                    ok = 0;
                    break;
                }
                if (shelfX + rw > cand)
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
            texW = cand;
            texH = (shelfY + shelfH + 3) & ~3;

            if (pass == 3)
                break;
            if (ok && (best < 0 || texW * texH < bestArea))
            {
                best     = pass;
                bestArea = texW * texH;
            }
        }
    }

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

/* One record drawn the way the stock inventory poses it: the game's own
 * per-item tilt (INV_ITEM_ROTATIONS, X then Z) under a turn about the vertical
 * axis, the axis the game spins the item on. Without the tilt, flat items
 * such as the medallions show their back. Drawn with its own texture the way
 * the GPU would -- nearest texel, colour word 0 not drawn, ABE prims blended
 * half and half -- plus a light shade by face angle so the shape reads at
 * small size. RGBA, transparent around the model.
 *
 * A still icon is fitted to what the camera sees at its one angle. A spinning
 * one (steady) is fitted to the circle it sweeps instead, so it keeps its size
 * all the way round. */
#define SM_ICON_SPIN 0.6109f /* 35 degrees of the game's turntable */

#include <math.h>
#include "../../src/bodyprog/items/item_rotations.h"

static void Sm_RenderPose(const unsigned char* rec, int size, unsigned char* rgba, float spin, int steady, float* zb)
{
    unsigned             ntri = Sm_U16(rec + 2);
    int                  tw   = (int)Sm_U16(rec + 4);
    int                  th   = (int)Sm_U16(rec + 6);
    const unsigned char* vtx  = rec + SM_ITEM_HDR;
    const unsigned char* tex  = vtx + (long)ntri * 3 * SM_VERT_BYTES;
    int                  rot  = (int)rec[0] - 32;
    float                tx   = 0.0f, tz = 0.0f, m[3][3];
    float                sx, cxr, sy, cyr, sz, czr;
    float                tm[3][3];
    float                cx, cy, cz, minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f, scale, ox, oy;
    unsigned             i;
    int                  k;

    if (size > 256)
        size = 256;
    memset(rgba, 0, (size_t)size * size * 4);

    if (rot >= 0 && rot < (int)(sizeof(INV_ITEM_ROTATIONS) / sizeof(INV_ITEM_ROTATIONS[0])))
    {
        tx = INV_ITEM_ROTATIONS[rot].vx * (6.2831853f / 4096.0f);
        tz = INV_ITEM_ROTATIONS[rot].vy * (6.2831853f / 4096.0f);
    }
    /* Ry * Rx * Rz, as Math_RotMatrixZxyNeg builds it for the item screen. */
    sx = sinf(tx); cxr = cosf(tx);
    sy = sinf(spin); cyr = cosf(spin);
    sz = sinf(tz); czr = cosf(tz);
    m[0][0] = cyr * czr + sy * sx * sz;  m[0][1] = -cyr * sz + sy * sx * czr; m[0][2] = sy * cxr;
    m[1][0] = cxr * sz;                  m[1][1] = cxr * czr;                 m[1][2] = -sx;
    m[2][0] = -sy * czr + cyr * sx * sz; m[2][1] = sy * sz + cyr * sx * czr;  m[2][2] = cyr * cxr;
    /* The same without the turn: Rx * Rz. */
    tm[0][0] = czr;       tm[0][1] = -sz;       tm[0][2] = 0.0f;
    tm[1][0] = cxr * sz;  tm[1][1] = cxr * czr; tm[1][2] = -sx;
    tm[2][0] = sx * sz;   tm[2][1] = sx * czr;  tm[2][2] = cxr;
    for (i = 0; i < (unsigned)(size * size); i++)
        zb[i] = 1e30f;

    cx = ((short)Sm_U16(rec + 8) + (short)Sm_U16(rec + 14)) * 0.5f;
    cy = ((short)Sm_U16(rec + 10) + (short)Sm_U16(rec + 16)) * 0.5f;
    cz = ((short)Sm_U16(rec + 12) + (short)Sm_U16(rec + 18)) * 0.5f;

#define SM_XFORM(p_, X_, Y_, Z_)                                         \
    do {                                                                 \
        float x_ = (short)Sm_U16(p_) - cx;                               \
        float y_ = (short)Sm_U16((p_) + 2) - cy;                         \
        float z_ = (short)Sm_U16((p_) + 4) - cz;                         \
        X_ = m[0][0] * x_ + m[0][1] * y_ + m[0][2] * z_;                 \
        Y_ = m[1][0] * x_ + m[1][1] * y_ + m[1][2] * z_;                 \
        Z_ = m[2][0] * x_ + m[2][1] * y_ + m[2][2] * z_;                 \
    } while (0)

    if (steady)
    {
        float r2 = 0.0f, ext;

        for (i = 0; i < ntri * 3; i++)
        {
            const unsigned char* q  = vtx + (long)i * SM_VERT_BYTES;
            float                x_ = (short)Sm_U16(q) - cx;
            float                y_ = (short)Sm_U16(q + 2) - cy;
            float                z_ = (short)Sm_U16(q + 4) - cz;
            float                X  = tm[0][0] * x_ + tm[0][1] * y_ + tm[0][2] * z_;
            float                Y  = tm[1][0] * x_ + tm[1][1] * y_ + tm[1][2] * z_;
            float                Z  = tm[2][0] * x_ + tm[2][1] * y_ + tm[2][2] * z_;

            if (X * X + Z * Z > r2) r2 = X * X + Z * Z;
            if (Y < miny) miny = Y;
            if (Y > maxy) maxy = Y;
        }
        if (r2 <= 0.0f || maxy <= miny)
            return;
        ext   = 2.0f * sqrtf(r2);
        scale = (size * 0.88f) / (ext > (maxy - miny) ? ext : (maxy - miny));
        ox    = size * 0.5f;
        oy    = size * 0.5f - (miny + maxy) * 0.5f * scale;
    }
    else
    {
        /* Fit what the camera actually sees, not the model's box: a long thin
         * item seen at an angle would otherwise come out tiny. */
        for (i = 0; i < ntri * 3; i++)
        {
            float X, Y, Z;
            SM_XFORM(vtx + (long)i * SM_VERT_BYTES, X, Y, Z);
            (void)Z;
            if (X < minx) minx = X;
            if (X > maxx) maxx = X;
            if (Y < miny) miny = Y;
            if (Y > maxy) maxy = Y;
        }
        if (maxx <= minx || maxy <= miny)
            return;
        scale = (size * 0.88f) / ((maxx - minx) > (maxy - miny) ? (maxx - minx) : (maxy - miny));
        ox    = size * 0.5f - (minx + maxx) * 0.5f * scale;
        oy    = size * 0.5f - (miny + maxy) * 0.5f * scale;
    }

    for (i = 0; i < ntri; i++)
    {
        const unsigned char* v[3];
        float                X[3], Y[3], Z[3], U[3], V[3], den, shade;
        float                nx, ny, nz, nl;
        int                  x0, x1, y0, y1, xx, yy;
        int                  semi;

        for (k = 0; k < 3; k++)
        {
            v[k] = vtx + ((long)i * 3 + k) * SM_VERT_BYTES;
            SM_XFORM(v[k], X[k], Y[k], Z[k]);
            X[k] = X[k] * scale + ox;
            Y[k] = Y[k] * scale + oy;
            U[k] = (float)Sm_U16(v[k] + 8);
            V[k] = (float)Sm_U16(v[k] + 10);
        }
        semi = (v[0][6] & SM_VF_SEMI) != 0;

        nx = (Y[1] - Y[0]) * (Z[2] - Z[0]) - (Z[1] - Z[0]) * (Y[2] - Y[0]);
        ny = (Z[1] - Z[0]) * (X[2] - X[0]) - (X[1] - X[0]) * (Z[2] - Z[0]);
        nz = (X[1] - X[0]) * (Y[2] - Y[0]) - (Y[1] - Y[0]) * (X[2] - X[0]);
        nl = sqrtf(nx * nx + ny * ny + nz * nz);
        shade = (nl > 0.0f) ? 0.72f + 0.28f * fabsf((nx * -0.35f + ny * -0.55f + nz * -0.76f) / nl) : 1.0f;

        den = (Y[1] - Y[2]) * (X[0] - X[2]) + (X[2] - X[1]) * (Y[0] - Y[2]);
        if (fabsf(den) < 1e-6f)
            continue;

        x0 = (int)floorf(fminf(X[0], fminf(X[1], X[2])));
        x1 = (int)ceilf(fmaxf(X[0], fmaxf(X[1], X[2])));
        y0 = (int)floorf(fminf(Y[0], fminf(Y[1], Y[2])));
        y1 = (int)ceilf(fmaxf(Y[0], fmaxf(Y[1], Y[2])));
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > size - 1) x1 = size - 1;
        if (y1 > size - 1) y1 = size - 1;

        for (yy = y0; yy <= y1; yy++)
        {
            for (xx = x0; xx <= x1; xx++)
            {
                float          px = xx + 0.5f, py = yy + 0.5f;
                float          w0 = ((Y[1] - Y[2]) * (px - X[2]) + (X[2] - X[1]) * (py - Y[2])) / den;
                float          w1 = ((Y[2] - Y[0]) * (px - X[2]) + (X[0] - X[2]) * (py - Y[2])) / den;
                float          w2 = 1.0f - w0 - w1;
                float          z;
                int            tu, tv, c;
                const unsigned char* t;
                unsigned char* d;

                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                    continue;
                z = w0 * Z[0] + w1 * Z[1] + w2 * Z[2];
                if (z >= zb[yy * size + xx])
                    continue;

                tu = (int)(w0 * U[0] + w1 * U[1] + w2 * U[2]);
                tv = (int)(w0 * V[0] + w1 * V[1] + w2 * V[2]);
                if (tu < 0) tu = 0;
                if (tv < 0) tv = 0;
                if (tu > tw - 1) tu = tw - 1;
                if (tv > th - 1) tv = th - 1;
                t = tex + ((long)tv * tw + tu) * 4;
                if (t[3] == 0)
                    continue;

                d = rgba + ((long)yy * size + xx) * 4;
                for (c = 0; c < 3; c++)
                {
                    float col = t[c] * (v[0][12 + c] / 255.0f) * shade;
                    if (col > 255.0f)
                        col = 255.0f;
                    if (semi && t[3] == 0xFE && d[3] != 0)
                        col = (col + d[c]) * 0.5f;
                    d[c] = (unsigned char)col;
                }
                d[3] = 255;
                zb[yy * size + xx] = z;
            }
        }
    }
#undef SM_XFORM
}

void Sm_RenderIcon(const unsigned char* rec, int size, unsigned char* rgba)
{
    static float zb[256 * 256];

    Sm_RenderPose(rec, size, rgba, SM_ICON_SPIN, 0, zb);
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
/* Android: the library, the icons, the dump and JNI.                     */
/* ---------------------------------------------------------------------- */

#if defined(__ANDROID__)

#include <jni.h>
#include <pthread.h>
#include <sys/stat.h>
#include <android/log.h>

#include "lang_text.h"   /* Pc_LangReadDiscFile */
#include "map_registry.h" /* MapRegistry_GetName */
#ifdef SH_STATIC_MAPS
#include "map_static_registry.h"
#endif

#define SM_TAG       "SH2Screen"
#define SM_PACKS     7
#define SM_LIST_MAX  64
#define SM_BLOB_HDR  16
#define SM_ICON      112
#define SM_ICON_HDR  4
#define SM_LIB_CAP   (8L * 1024 * 1024)

/* Everything the worker needs, copied on the game thread. */
typedef struct
{
    int           count[SM_PACKS];
    unsigned char items[SM_PACKS][SM_LIST_MAX];
    int           keyTim[SM_PACKS];
    int           listMap[SM_PACKS]; /* the map whose list was used, for the report */
    unsigned long packSector[SM_PACKS], packSize[SM_PACKS];
    unsigned long keySector[SM_PACKS], keySize[SM_PACKS];
    unsigned long sector07, size07, sector00, size00;
    s_FsImageDesc descKey, descCommon, descAlways;
    char          dumpDir[512];
} s_SmJob;

static pthread_mutex_t s_smLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char*  s_smBlob;   /* "SHM1": the models, for the 3D view */
static long            s_smBlobLen;
static unsigned char*  s_smIcons;  /* "SHI1": a still picture per model */
static long            s_smIconsLen;
static int             s_smSerial;
static int             s_smBusy;
static int             s_smBuilt;
static char            s_smDumpDir[512];
static int             s_smForce;

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

static void Sm_WritePng(const char* dir, const char* name, const unsigned char* rgba, int w, int h)
{
    extern void* tdefl_write_image_to_png_file_in_memory(const void*, int, int, int, size_t*);
    extern void  mz_free(void*);

    size_t len = 0;
    void*  png = tdefl_write_image_to_png_file_in_memory(rgba, w, h, 4, &len);

    if (png != NULL)
    {
        Sm_WriteFile(dir, name, png, (long)len);
        mz_free(png);
    }
}

static void Sm_DumpRecord(const char* dir, const unsigned char* rec, const unsigned char* icon)
{
    int                  item = rec[0];
    int                  mask = rec[1];
    unsigned             ntri = Sm_U16(rec + 2);
    int                  tw   = (int)Sm_U16(rec + 4);
    int                  th   = (int)Sm_U16(rec + 6);
    const unsigned char* v    = rec + SM_ITEM_HDR;
    const unsigned char* tex  = v + (long)ntri * 3 * SM_VERT_BYTES;
    char                 name[64];
    char                 path[700];
    FILE*                f;
    unsigned             i;

    snprintf(name, sizeof(name), "item_%03d_%02x.png", item, mask);
    Sm_WritePng(dir, name, tex, tw, th);
    snprintf(name, sizeof(name), "icon_%03d_%02x.png", item, mask);
    Sm_WritePng(dir, name, icon, SM_ICON, SM_ICON);

    /* Wavefront OBJ: opens in most 3D viewers. PSX y points down. */
    snprintf(path, sizeof(path), "%s/item_%03d_%02x.obj", dir, item, mask);
    f = fopen(path, "w");
    if (f == NULL)
        return;
    fprintf(f, "mtllib item_%03d_%02x.mtl\nusemtl m\n", item, mask);
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

    snprintf(path, sizeof(path), "%s/item_%03d_%02x.mtl", dir, item, mask);
    f = fopen(path, "w");
    if (f != NULL)
    {
        fprintf(f, "newmtl m\nKd 1 1 1\nmap_Kd item_%03d_%02x.png\n", item, mask);
        fclose(f);
    }
}

static long Sm_RecordLen(const unsigned char* rec)
{
    return SM_ITEM_HDR + (long)Sm_U16(rec + 2) * 3 * SM_VERT_BYTES + (long)Sm_U16(rec + 4) * Sm_U16(rec + 6) * 4;
}

/* Builds every item model of every pack once, merging identical copies: the
 * common items appear in all seven packs and are, as a rule, the same model.
 * A record's byte 1 is the set of packs (bit = pack) it came from, so the
 * second screen can prefer the copy of the pack the player is in, which is
 * what the stock inventory would show. */
static void* Sm_Worker(void* arg)
{
    s_SmJob*        job   = (s_SmJob*)arg;
    unsigned char*  f07   = NULL;
    unsigned char*  f00   = NULL;
    unsigned short* base  = NULL;
    unsigned short* vram  = NULL;
    unsigned char*  blob  = NULL;
    unsigned char*  icons = NULL;
    long            len   = SM_BLOB_HDR;
    long            ilen  = SM_BLOB_HDR;
    int             nrec  = 0;
    int             p, i;
    char            dumpPack[600];
    const char*     dump  = job->dumpDir[0] ? job->dumpDir : NULL;
    unsigned char   have[256][SM_PACKS];

    memset(have, 0, sizeof(have));

    f07  = job->size07 ? Pc_LangReadDiscFile((unsigned)job->sector07, (unsigned)job->size07) : NULL;
    f00  = job->size00 ? Pc_LangReadDiscFile((unsigned)job->sector00, (unsigned)job->size00) : NULL;
    base = (unsigned short*)calloc((size_t)SM_VRAM_W * SM_VRAM_H, sizeof(unsigned short));
    vram = (unsigned short*)malloc((size_t)SM_VRAM_W * SM_VRAM_H * sizeof(unsigned short));
    blob = (unsigned char*)malloc((size_t)SM_LIB_CAP);
    icons = (unsigned char*)malloc((size_t)SM_BLOB_HDR + 256L * (SM_ICON_HDR + SM_ICON * SM_ICON * 4));
    if (base == NULL || vram == NULL || blob == NULL || icons == NULL)
        goto done;

    if (f07 == NULL || !Sm_PlaceTim(base, f07, (long)job->size07, &job->descAlways))
        __android_log_print(ANDROID_LOG_WARN, SM_TAG, "models: TIM07 unreadable");
    if (f00 == NULL || !Sm_PlaceTim(base, f00, (long)job->size00, &job->descCommon))
        __android_log_print(ANDROID_LOG_WARN, SM_TAG, "models: TIM00 unreadable");

    if (dump != NULL)
    {
        if (f07) Sm_WriteFile(dump, "TIM07.TIM", f07, (long)job->size07);
        if (f00) Sm_WriteFile(dump, "TIM00.TIM", f00, (long)job->size00);
    }

    memset(blob, 0, SM_BLOB_HDR);
    memcpy(blob, "SHM1", 4);
    blob[4] = 2;
    blob[6] = SM_PACKS;

    for (p = 0; p < SM_PACKS; p++)
    {
        unsigned char* tmd = NULL;
        unsigned char* key = NULL;
        int            built = 0;

        if (job->count[p] == 0 || job->packSize[p] == 0)
            continue;

        tmd = Pc_LangReadDiscFile((unsigned)job->packSector[p], (unsigned)job->packSize[p]);
        if (tmd == NULL)
        {
            __android_log_print(ANDROID_LOG_WARN, SM_TAG, "models: could not read IT_%03d from the disc", p);
            continue;
        }

        /* Each pack sees the textures its own maps load: the shared two, plus
         * its key-item TIM over them. */
        memcpy(vram, base, (size_t)SM_VRAM_W * SM_VRAM_H * sizeof(unsigned short));
        if (job->keySize[p] > 0)
        {
            key = Pc_LangReadDiscFile((unsigned)job->keySector[p], (unsigned)job->keySize[p]);
            if (key == NULL || !Sm_PlaceTim(vram, key, (long)job->keySize[p], &job->descKey))
                __android_log_print(ANDROID_LOG_WARN, SM_TAG, "models: TIM%02d unreadable", job->keyTim[p]);
        }

        if (dump != NULL)
        {
            char name[32];

            snprintf(dumpPack, sizeof(dumpPack), "%s/pack_%d", dump, p);
            mkdir(dumpPack, 0775);
            snprintf(name, sizeof(name), "IT_%03d.TMD", p);
            Sm_WriteFile(dumpPack, name, tmd, (long)job->packSize[p]);
            if (key != NULL)
            {
                snprintf(name, sizeof(name), "TIM%02d.TIM", job->keyTim[p]);
                Sm_WriteFile(dumpPack, name, key, (long)job->keySize[p]);
            }
        }

        for (i = 0; i < job->count[p]; i++)
        {
            int  item = job->items[p][i];
            long n    = Sm_BuildItem(blob + len, SM_LIB_CAP - len, item, tmd, (long)job->packSize[p], i, vram);
            long q;
            int  merged = 0;

            if (n <= 0)
            {
                __android_log_print(ANDROID_LOG_INFO, SM_TAG, "models: IT_%03d object %d (item %d) unusable", p, i, item);
                continue;
            }
            built++;
            have[item][p] = 1;

            /* Same model already seen in another pack? Compare everything
             * after the id/pack bytes. */
            for (q = SM_BLOB_HDR; q < len; q += Sm_RecordLen(blob + q))
            {
                if (blob[q] == item && Sm_RecordLen(blob + q) == n &&
                    memcmp(blob + q + 2, blob + len + 2, (size_t)(n - 2)) == 0)
                {
                    blob[q + 1] |= (unsigned char)(1 << p);
                    merged = 1;
                    break;
                }
            }
            if (merged)
                continue;

            blob[len + 1] = (unsigned char)(1 << p);
            len += n;
            nrec++;
        }

        __android_log_print(ANDROID_LOG_INFO, SM_TAG, "models: IT_%03d (list of map %d, key TIM%02d): %d of %d built",
                            p, job->listMap[p], job->keyTim[p], built, job->count[p]);
        free(tmd);
        free(key);
    }
    Sm_Put16(blob + 8, (unsigned)nrec);

    /* Icons, one per distinct model. */
    memset(icons, 0, SM_BLOB_HDR);
    memcpy(icons, "SHI1", 4);
    icons[4] = 1;
    icons[5] = SM_ICON;
    Sm_Put16(icons + 8, (unsigned)nrec);
    {
        long q;
        for (q = SM_BLOB_HDR; q < len; q += Sm_RecordLen(blob + q))
        {
            unsigned char* d = icons + ilen;
            d[0] = blob[q];
            d[1] = blob[q + 1];
            d[2] = d[3] = 0;
            Sm_RenderIcon(blob + q, SM_ICON, d + SM_ICON_HDR);
            if (dump != NULL)
                Sm_DumpRecord(dump, blob + q, d + SM_ICON_HDR);
            ilen += SM_ICON_HDR + (long)SM_ICON * SM_ICON * 4;
        }
    }

    /* Which items have a model at all, and where. Every inventory item id the
     * game names is listed; the unnamed ids are gaps in the enum. */
    {
        extern const char* Pc_Inventory_ItemName(u8 id);

        char  report[16384];
        int   rl = 0, id, missing = 0;

        rl += snprintf(report + rl, sizeof(report) - rl, "item  packs    name\n");
        for (id = 32; id < 256 && rl < (int)sizeof(report) - 200; id++)
        {
            const char* nm = Pc_Inventory_ItemName((u8)id);
            char        packs[SM_PACKS + 1];
            int         any = 0;

            if (nm == NULL || nm[0] == '\0' || (id >= InvItemId_CutscenePhone && id <= InvItemId_CutsceneBloodPack))
                continue;
            for (p = 0; p < SM_PACKS; p++)
            {
                packs[p] = have[id][p] ? (char)('0' + p) : '.';
                any |= have[id][p];
            }
            packs[SM_PACKS] = '\0';
            if (!any)
                missing++;
            rl += snprintf(report + rl, sizeof(report) - rl, "%3d   %s  %s%s\n", id, packs, any ? "" : "NO MODEL  ", nm);
        }
        __android_log_print(ANDROID_LOG_INFO, SM_TAG, "models: %d distinct models; %d named items have no model in any pack",
                            nrec, missing);
        if (dump != NULL)
            Sm_WriteFile(dump, "coverage.txt", report, rl);
    }

    if (dump != NULL)
        __android_log_print(ANDROID_LOG_INFO, SM_TAG, "models: dumped to %s", dump);

    pthread_mutex_lock(&s_smLock);
    free(s_smBlob);
    free(s_smIcons);
    s_smBlob     = blob;
    s_smBlobLen  = len;
    s_smIcons    = icons;
    s_smIconsLen = ilen;
    s_smSerial   = (s_smSerial % 255) + 1;
    s_smBuilt    = 1;
    pthread_mutex_unlock(&s_smLock);
    blob  = NULL;
    icons = NULL;

    __android_log_print(ANDROID_LOG_INFO, SM_TAG, "models: library ready, %ld bytes of models, %ld bytes of icons", len, ilen);

done:
    free(f07);
    free(f00);
    free(base);
    free(vram);
    free(blob);
    free(icons);
    free(job);

    pthread_mutex_lock(&s_smLock);
    s_smBusy = 0;
    pthread_mutex_unlock(&s_smLock);
    return NULL;
}

/* The overlay header of any map, loaded or not. The Android build links all
 * 43 overlays in (SH_STATIC_MAPS), so every map's item list is plain data
 * that can be read without visiting the map; map0_s00 is built in either way. */
static const s_MapOverlayHdr* Sm_MapHeader(int map)
{
    extern s_MapOverlayHdr g_MapOverlayHeader_map0_s00;

    if (map == MapIdx_MAP0_S00)
        return &g_MapOverlayHeader_map0_s00;
#ifdef SH_STATIC_MAPS
    return MapStatic_Find(MapRegistry_GetName((e_MapIdx)map));
#else
    return NULL;
#endif
}

static void Sm_SetList(s_SmJob* job, int p, int map, const u8* list)
{
    int n = 0;

    if (list == NULL)
        return;
    while (n < SM_LIST_MAX && list[n] != 0)
        n++;
    /* Maps that share a pack index the same TMD, so their lists agree; the
     * longest one is kept in case one stops short. */
    if (n > job->count[p])
    {
        memcpy(job->items[p], list, (size_t)n);
        job->count[p]   = n;
        job->listMap[p] = map;
        job->keyTim[p]  = Sm_KeyTimForMap(map);
    }
}

/* Game thread, every frame (pc_second_screen.c). Builds the library once a
 * game is under way -- the disc and the region's file table are settled by
 * then -- and again when a dump is asked for. */
void Pc_SecondScreenModels_Tick(void)
{
    static const int PACK_FILE[SM_PACKS] = {
        FILE_ITEM_IT_000_TMD, FILE_ITEM_IT_001_TMD, FILE_ITEM_IT_002_TMD, FILE_ITEM_IT_003_TMD,
        FILE_ITEM_IT_004_TMD, FILE_ITEM_IT_005_TMD, FILE_ITEM_IT_006_TMD
    };
    static const int KEY_FILE[7] = {
        0, FILE_ITEM_TIM01_TIM, FILE_ITEM_TIM02_TIM, FILE_ITEM_TIM03_TIM,
        FILE_ITEM_TIM04_TIM, FILE_ITEM_TIM05_TIM, FILE_ITEM_TIM06_TIM
    };

    s_SmJob*       job;
    int            map, p, go;
    pthread_t      th;
    pthread_attr_t attr;

    if (g_GameWork.gameState != GameState_InGame || g_SavegamePtr == NULL)
        return;

    pthread_mutex_lock(&s_smLock);
    go = !s_smBusy && (!s_smBuilt || s_smForce);
    if (go)
    {
        s_smForce = 0;
        s_smBusy  = 1;
    }
    pthread_mutex_unlock(&s_smLock);
    if (!go)
        return;

    job = (s_SmJob*)calloc(1, sizeof(*job));
    if (job == NULL)
        goto fail;

    for (map = MapIdx_MAP0_S00; map <= MapIdx_MAP7_S03; map++)
    {
        const s_MapOverlayHdr* h = Sm_MapHeader(map);

        p = Sm_PackForMap(map);
        if (p >= 0 && h != NULL)
            Sm_SetList(job, p, map, h->loadableItems);
    }
    /* The current map always counts, whatever the build links in. */
    if (g_pMapOverlayHeader != NULL && Sm_PackForMap(g_SavegamePtr->mapIdx) >= 0)
        Sm_SetList(job, Sm_PackForMap(g_SavegamePtr->mapIdx), g_SavegamePtr->mapIdx,
                   g_pMapOverlayHeader->loadableItems);

    for (p = 0; p < SM_PACKS; p++)
    {
        job->packSector[p] = g_FileTable[PACK_FILE[p]].startSector;
        job->packSize[p]   = (unsigned long)g_FileTable[PACK_FILE[p]].blockCount * 256;
        if (job->keyTim[p] > 0)
        {
            job->keySector[p] = g_FileTable[KEY_FILE[job->keyTim[p]]].startSector;
            job->keySize[p]   = (unsigned long)g_FileTable[KEY_FILE[job->keyTim[p]]].blockCount * 256;
        }
    }
    job->sector07 = g_FileTable[FILE_ITEM_TIM07_TIM].startSector;
    job->size07   = (unsigned long)g_FileTable[FILE_ITEM_TIM07_TIM].blockCount * 256;
    job->sector00 = g_FileTable[FILE_ITEM_TIM00_TIM].startSector;
    job->size00   = (unsigned long)g_FileTable[FILE_ITEM_TIM00_TIM].blockCount * 256;

    /* Copies, taken here: they carry the PAL palette homes. */
    job->descKey    = g_InventoryKeyItemTextureImg;
    job->descCommon = g_FirstAidKitItemTextureImg;
    job->descAlways = D_800A9074;

    pthread_mutex_lock(&s_smLock);
    snprintf(job->dumpDir, sizeof(job->dumpDir), "%s", s_smDumpDir);
    pthread_mutex_unlock(&s_smLock);
    if (job->dumpDir[0])
        mkdir(job->dumpDir, 0775);

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, Sm_Worker, job) == 0)
    {
        pthread_attr_destroy(&attr);
        return;
    }
    pthread_attr_destroy(&attr);
    free(job);

fail:
    pthread_mutex_lock(&s_smLock);
    s_smBusy = 0;
    pthread_mutex_unlock(&s_smLock);
}

int Pc_SecondScreenModels_Serial(void)
{
    int s;

    pthread_mutex_lock(&s_smLock);
    s = s_smSerial;
    pthread_mutex_unlock(&s_smLock);
    return s;
}

/* The pack the stock inventory would load right now, or -1. */
int Pc_SecondScreenModels_CurrentPack(void)
{
    if (g_SavegamePtr == NULL)
        return -1;
    return Sm_PackForMap(g_SavegamePtr->mapIdx);
}

/* UI thread: where the dump goes, or null for no dump. A set directory
 * rebuilds the library so the dump happens. */
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

static jbyteArray Sm_ToJava(JNIEnv* env, const unsigned char* data, long len)
{
    jbyteArray arr = NULL;

    if (data != NULL && len > 0)
    {
        arr = (*env)->NewByteArray(env, (jsize)len);
        if (arr != NULL)
            (*env)->SetByteArrayRegion(env, arr, 0, (jsize)len, (const jbyte*)data);
    }
    return arr;
}

/* UI thread: the model library, or null. */
JNIEXPORT jbyteArray JNICALL
Java_com_silenthill_port_SecondScreen_nativeModels(JNIEnv* env, jclass cls)
{
    jbyteArray arr;

    (void)cls;
    pthread_mutex_lock(&s_smLock);
    arr = Sm_ToJava(env, s_smBlob, s_smBlobLen);
    pthread_mutex_unlock(&s_smLock);
    return arr;
}

/* UI thread: the icons, or null. */
JNIEXPORT jbyteArray JNICALL
Java_com_silenthill_port_SecondScreen_nativeIcons(JNIEnv* env, jclass cls)
{
    jbyteArray arr;

    (void)cls;
    pthread_mutex_lock(&s_smLock);
    arr = Sm_ToJava(env, s_smIcons, s_smIconsLen);
    pthread_mutex_unlock(&s_smLock);
    return arr;
}

/* UI thread, once a frame for each turning item: one record, picked by item
 * and pack mask as the icon was, drawn at the given turn and fitted to the
 * circle it sweeps. ARGB into out (size x size). False if there is no such
 * record (yet). */
JNIEXPORT jboolean JNICALL
Java_com_silenthill_port_SecondScreen_nativeRenderModel(JNIEnv* env, jclass cls, jint itemId, jint mask,
                                                       jfloat spin, jint size, jintArray out)
{
    static float         zb[SM_ICON * SM_ICON];
    static unsigned char rgba[SM_ICON * SM_ICON * 4];
    static jint          argb[SM_ICON * SM_ICON];
    const unsigned char* rec = NULL;
    long                 q;
    int                  i;

    (void)cls;
    if (size != SM_ICON || out == NULL || (*env)->GetArrayLength(env, out) < SM_ICON * SM_ICON)
        return JNI_FALSE;

    pthread_mutex_lock(&s_smLock);
    if (s_smBlob != NULL)
    {
        for (q = SM_BLOB_HDR; q < s_smBlobLen; q += Sm_RecordLen(s_smBlob + q))
        {
            if (s_smBlob[q] == (unsigned char)itemId && s_smBlob[q + 1] == (unsigned char)mask)
            {
                rec = s_smBlob + q;
                break;
            }
        }
    }
    if (rec != NULL)
        Sm_RenderPose(rec, SM_ICON, rgba, spin, 1, zb);
    pthread_mutex_unlock(&s_smLock);

    if (rec == NULL)
        return JNI_FALSE;

    for (i = 0; i < SM_ICON * SM_ICON; i++)
    {
        const unsigned char* p = rgba + i * 4;
        argb[i] = (jint)(((unsigned)p[3] << 24) | ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2]);
    }
    (*env)->SetIntArrayRegion(env, out, 0, SM_ICON * SM_ICON, argb);
    return JNI_TRUE;
}

#else

void Pc_SecondScreenModels_Tick(void)
{
}

int Pc_SecondScreenModels_Serial(void)
{
    return 0;
}

int Pc_SecondScreenModels_CurrentPack(void)
{
    return -1;
}

#endif
