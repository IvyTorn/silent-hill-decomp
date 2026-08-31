/* PSY-Q to PsyCross compatibility shim */
#ifndef _PSYQ_COMPAT_INLINE_C_H
#define _PSYQ_COMPAT_INLINE_C_H
#include <inline_c.h>

/* PsyCross GTE macros dereference args as pointers (*(uint*)(r0)),
 * but real PSX inline_c passes them as values (mtc2 r0, $reg).
 * Override all affected macros to match PSX value-passing behavior. */

#undef gte_lddp
#define gte_lddp( r0 ) { uint _v = (uint)(r0); MTC2(_v, 8); }

#undef gte_ldsxy0
#define gte_ldsxy0( r0 ) { MTC2((uint)(r0), 12); }

#undef gte_ldsxy3
#define gte_ldsxy3( r0, r1, r2 ) \
    { MTC2((uint)(r0), 12); MTC2((uint)(r2), 14); MTC2((uint)(r1), 13); }

#undef gte_ldv3c
/* Through GTE_P32, the half-composition gte_ldv0 / gte_ldv3 already use: a
 * raw word read of {s16 x, s16 y} equals the PSX (y<<16)|x only on
 * little-endian. Raw here loaded VX=y, VY=x, VZ=pad on big-endian for every
 * caller -- the unlit model transform (func_80057B7C), the lit path's
 * lighting normals, the particle emitters -- while the lit VERTEX path
 * composed its pairs and was fine: Harry rendered, the world and his gun
 * came out as stretched blobs. Value-identical on little-endian. */
#define gte_ldv3c( r0 ) do { \
    char *_b = (char*)(r0); \
    MTC2(GTE_P32(_b + 0), 0);  MTC2(GTE_P32(_b + 4), 1); \
    MTC2(GTE_P32(_b + 8), 2);  MTC2(GTE_P32(_b + 12), 3); \
    MTC2(GTE_P32(_b + 16), 4); MTC2(GTE_P32(_b + 20), 5); \
} while(0)

/* PSX swc2 of SZ1..3: a 32-bit store of a zero-extended 16-bit depth whose
 * LOW half lands AT the address, and the caller reads the s16 there
 * (func_80057B7C reads m[0][2] / m[2][0] back). On big-endian the low half
 * of a word store is two bytes further on, so the depth must be placed in
 * the high half to sit at the address. Value-identical on little-endian. */
#undef gte_stsz3
#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define gte_stsz3( r0, r1, r2 ) do { \
    *(uint*)((char*)(r0)) = (MFC2(17) & 0xFFFFu) << 16; \
    *(uint*)((char*)(r1)) = (MFC2(18) & 0xFFFFu) << 16; \
    *(uint*)((char*)(r2)) = (MFC2(19) & 0xFFFFu) << 16; \
} while(0)
#else
#define gte_stsz3( r0, r1, r2 ) do { \
    *(uint*)((char*)(r0)) = MFC2(17); \
    *(uint*)((char*)(r1)) = MFC2(18); \
    *(uint*)((char*)(r2)) = MFC2(19); \
} while(0)
#endif

#undef gte_stsxy3c
#define gte_stsxy3c( r0 ) do { \
    uint *_p = (uint*)((char*)(r0)); \
    _p[0] = MFC2(12); _p[1] = MFC2(13); _p[2] = MFC2(14); \
} while(0)

/* gte_stsxy3_g3: store SXY0/1/2 (GTE C12-14) into the X/Y slots of a
 * POLY_G3 / POLY_FT4 / POLY_GT4 packet. PSX layout has XYs at 8,16,24
 * (DECLARE_P_ADDR=4 B); PC with USE_EXTENDED_PRIM_POINTERS has them
 * at 16,24,32 (DECLARE_P_ADDR=12 B). Using PSX offsets on PC clobbers
 * the prim header (len/pgxp_index) AND shifts vertex data by one
 * slot, leaving x2,y2 uninitialized — see crash dump from
 * 2026-05-01 21:37 (ParsePrimitivesLinkedList +0xa5, bad next-pointer
 * with the muzzle-flash tpage byte 0x2B in upper bytes). */
#undef gte_stsxy3_g3
/* Struct-derived slot offsets -- see the full explanation on the canonical copy
 * in pc_port/include/inline_no_dmpsx.h. Short version: the old
 * `defined(SH_PC_PORT)` branch used the LP64 offsets 16/24/32, but a 32-bit
 * SH_PC_PORT build (the Xbox port) has P_LEN=2 and its real slots at 12/20/28,
 * so all three coordinates were written into the texture words and x0..x2 kept
 * stale packet bytes. BOTH copies must be fixed: particle_glass.c includes
 * inline_no_dmpsx.h first but then pulls this header in via bodyprog.h, and
 * this later #undef/#define is the one that wins in those TUs. */
#define gte_stsxy3_g3( p ) do { \
    POLY_FT3 *_q = (POLY_FT3*)(void*)(p); \
    *(uint*)&_q->x0 = MFC2(12); \
    *(uint*)&_q->x1 = MFC2(13); \
    *(uint*)&_q->x2 = MFC2(14); \
} while(0)

/* gte_stsz3c: store SZ1/SZ2/SZ3 (GTE C17-19) with PSX `swc2` stride (4 bytes).
 * Mirrors the canonical fix in pc_port/include/inline_no_dmpsx.h. Kept here
 * defensively for any TU that includes this shim without inline_no_dmpsx.h.
 *
 * PSX uses `swc2 $N,K($p)` — a 32-bit store at offsets 0, 4, 8. Earlier the
 * macro here used `short*` stride 2 (6 bytes total), leaving the upper half
 * of each destination s32 stale → particle code that averages four
 * consecutive s32 fields after gte_stsz3c (e.g. func_80063A50 muzzle-flash
 * field_1BC..field_1C8) produces garbage Z, lands in OT bucket 0, and the
 * quads render as a one-frame full-screen flash. */
#undef gte_stsz3c
#define gte_stsz3c( p ) do { \
    int *_w = (int*)(p); \
    _w[0] = (int)(MFC2(17) & 0xFFFF); \
    _w[1] = (int)(MFC2(18) & 0xFFFF); \
    _w[2] = (int)(MFC2(19) & 0xFFFF); \
} while(0)

#endif
