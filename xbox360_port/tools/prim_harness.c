/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * prim_harness.c - does DrawOTag's dispatch byte survive big-endian?
 *
 * A PSX packet's first payload word gets written two different ways:
 *   POLY/SPRT   set `code` as a BYTE FIELD  (setcode)
 *   DR_TPAGE    set a u_int VALUE, command in bits 24-31 (setDrawTPage)
 * DrawOTag dispatches on P_TAG.code for BOTH. On little-endian the two land on
 * the same byte; on big-endian they do not, and every primitive is skipped as an
 * unknown command (Xbox 360 log 025: unk=840, oversize=0).
 *
 * This builds one packet of each kind exactly as the game does and checks the
 * dispatcher reads back the command it wrote -- no GPU, no game, so it runs
 * under qemu-ppc in a second instead of costing a BadUpdate run.
 */
#include <stdio.h>
#include <string.h>

#include <libgte.h>   /* SVECTOR, used by libgpu.h's GsBOXF etc. */
#include <libgpu.h>

static int failures;

static void Check(const char* what, int got, int want)
{
    int ok = (got == want);
    if (!ok)
        failures++;
    printf("  %-34s got=0x%02x want=0x%02x  %s\n", what, got, want, ok ? "ok" : "FAIL");
}

int main(void)
{
    printf("endian: %s   sizeof(P_TAG)=%d P_LEN=%d\n",
           (*(const unsigned char*)(const unsigned int[]){1}) ? "little" : "BIG",
           (int)sizeof(P_TAG), (int)P_LEN);

    /* 1. A textured quad, code written as a byte field. */
    {
        POLY_FT4 p;
        memset(&p, 0, sizeof(p));
        setPolyFT4(&p);
        Check("POLY_FT4 -> P_TAG.code", ((P_TAG*)&p)->code, 0x2C);
    }

    /* 2. A draw-mode packet, command written as bits 24-31 of a u_int. This is
     *    the one that broke: the dispatcher must see 0xE1, and ProcessDrawMode
     *    must still find the tpage with `w >> 24`. */
    {
        DR_TPAGE d;
        memset(&d, 0, sizeof(d));
        setDrawTPage(&d, 0, 1, 42);
        {
            const unsigned* w = (const unsigned*)((const char*)&d + P_LEN * 4);
            Check("DR_TPAGE -> P_TAG.code", ((P_TAG*)&d)->code, 0xE1);
            Check("DR_TPAGE -> (word >> 24)", (int)(w[0] >> 24), 0xE1);
            printf("  %-34s tpage=%d want=42  %s\n", "DR_TPAGE -> word & 0x1FF",
                   (int)(w[0] & 0x1FF), ((int)(w[0] & 0x1FF) == 42) ? "ok" : "FAIL");
            if ((int)(w[0] & 0x1FF) != 42)
                failures++;
        }
    }

    /* 3. A flat triangle, so the poly path is covered at both ends of the range. */
    {
        POLY_F3 p;
        memset(&p, 0, sizeof(p));
        setPolyF3(&p);
        Check("POLY_F3 -> P_TAG.code", ((P_TAG*)&p)->code, 0x20);
    }

    /* 4. The colour bytes must still be reachable by name after any reorder. */
    {
        POLY_FT4 p;
        memset(&p, 0, sizeof(p));
        setPolyFT4(&p);
        setRGB0(&p, 0x11, 0x22, 0x33);
        Check("POLY_FT4 r0", p.r0, 0x11);
        Check("POLY_FT4 g0", p.g0, 0x22);
        Check("POLY_FT4 b0", p.b0, 0x33);
        Check("POLY_FT4 code survives setRGB0", ((P_TAG*)&p)->code, 0x2C);
    }

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
