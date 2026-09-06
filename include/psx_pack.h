/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * psx_pack.h - big-endian-correct forms of the decomp's type-punned
 * multi-field primitive stores.
 *
 * Its own header, rather than living in gpu.h, because there are TWO gpu.h
 * files -- include/gpu.h and pc_port/include/gpu.h, the PC override that
 * shadows it on the include path -- and which one a TU gets depends on whether
 * it reached the header through a quoted include from include/ or through the
 * -I list. Putting these macros in either one leaves the other half of the tree
 * without them, and the failure is a link-time implicit function call rather
 * than anything obvious. Both gpu.h files include this.
 */
#ifndef PSX_PACK_H
#define PSX_PACK_H

/* ---------------------------------------------------------------------------
 * Type-punned multi-field stores.
 *
 * The decomp is faithful to MIPS, so it inherits the PSX trick of writing
 * several sub-word fields with ONE wide store:
 *
 *     *(s32*)&poly->u1 = ((v1 << 8) + 0x2B0000 + u1);
 *
 * The value is composed in LITTLE-ENDIAN FIELD ORDER -- first field in the low
 * bits -- which is the same thing as writing the fields only on a little-endian
 * host. On big-endian every byte lands in the wrong field, and it fails
 * SILENTLY: garbage UVs, tpages and geometry, never a crash.
 *
 * There is NO single macro for this, which is the trap. The right byte
 * placement depends on the SHAPE of the field group being written, because the
 * multi-byte subfields inside it (a u_short tpage, an s16 coordinate) are
 * themselves read big-endian. A plain byte-reverse fixes the u8 fields and
 * breaks the u16 one. So each shape gets its own macro, chosen by the
 * destination field:
 *
 *     ->r0            RGBC  (u8,u8,u8,u8)   colour + command
 *     ->r1 ->r2 ->r3  RGB   (u8,u8,u8,u8)   colour + pad
 *     ->u0 .. ->u3    UV    (u8,u8,u16)     u, v, clut/tpage/pad
 *     ->x0 .. ->x3    XY    (s16,s16)       screen coordinates
 *
 * Every expansion is the plain store on little-endian, so PC / Xbox / Android /
 * iOS codegen does not move at all.
 * ------------------------------------------------------------------------- */
/* Deliberately NOT the decomp's u32: this header is included by both gpu.h
 * files and by standalone tools, and reaching for a typedef that may not be
 * declared yet is exactly the include-order trap this file exists to avoid.
 * unsigned int is 32 bits on every target this port builds for (ILP32 and
 * LP64 alike). */
typedef unsigned int SH_PACK_U32;

#define SH_BSWAP32(v) ((SH_PACK_U32)((((SH_PACK_U32)(v)) >> 24) | ((((SH_PACK_U32)(v)) >> 8) & 0x0000FF00u) | \
                             ((((SH_PACK_U32)(v)) << 8) & 0x00FF0000u) | (((SH_PACK_U32)(v)) << 24)))

#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)

/* r0's quartet is declared MSB-first on big-endian (PSX_PRIM_CMD in PsyCross's
 * libgpu.h), so its memory image ALREADY equals the composed PSX word and the
 * value needs no transform -- only the group's base address differs, because
 * `code` is the first-declared member there instead of `r0`. */
#define PSX_ST_RGBC(p, w)    (*(SH_PACK_U32*)&(p)->code = (SH_PACK_U32)(w))

/* The r1/r2/r3 quartets are not reversed (nothing dispatches on their pad
 * byte), so they are still declared LSB-first and the word is byte-reversed. */
#define PSX_ST_RGB(p, f, w)  (*(SH_PACK_U32*)&(p)->f = SH_BSWAP32(w))

/* (u8 u, u8 v, u16 half): u->byte0, v->byte1, half->bytes2-3 big-endian. */
#define PSX_ST_UV(p, f, w)   (*(SH_PACK_U32*)&(p)->f = (SH_PACK_U32)((((SH_PACK_U32)(w)) << 24) | \
                                                    ((((SH_PACK_U32)(w)) & 0x0000FF00u) << 8) | \
                                                    (((SH_PACK_U32)(w)) >> 16)))

/* (s16 x, s16 y): x->bytes0-1, y->bytes2-3, each big-endian. */
#define PSX_ST_XY(p, f, w)   (*(SH_PACK_U32*)&(p)->f = (SH_PACK_U32)((((SH_PACK_U32)(w)) << 16) | \
                                                    (((SH_PACK_U32)(w)) >> 16)))

/* Store of a word that is ALREADY the {x,y} memory image (a gte_stsxy /
 * RotTransPers* output -- see SH_MEM_SXY): raw on both endians. Feeding an
 * image through PSX_ST_XY double-converted it, which is what drew every
 * GsTMDfast model (inventory items, pickups, the wall map) transposed. */
#define PSX_ST_XY_IMG(p, f, w) (*(SH_PACK_U32*)&(p)->f = (SH_PACK_U32)(w))

/* Load of a CVECTOR-shaped quartet (memory order r,g,b,code -- the GTE store
 * macros keep PSX byte order on both endians) as the composed PSX word with r
 * in the LOW byte, i.e. what every packed colour STORE macro above takes. */
#define PSX_LD_CVEC(ptr)     SH_BSWAP32(*(const SH_PACK_U32*)(ptr))

/* Load of an (s16 x, s16 y) PAIR as the LE-composed word the ST macros take
 * (x in the LOW half). A raw 32-bit read of that pair on big-endian yields
 * x in the HIGH half, so the halves must be exchanged -- NOT byte-reversed,
 * which would also flip the bytes inside each coordinate. Pairs with a packed
 * store fed by a packed load were only half-fixed without this: aligned, but
 * with x and y swapped. */
#define PSX_LD_XY(ptr)       ((SH_PACK_U32)(((*(const SH_PACK_U32*)(ptr)) << 16) |                                             ((*(const SH_PACK_U32*)(ptr)) >> 16)))

#else

#define PSX_ST_RGBC(p, w)    (*(SH_PACK_U32*)&(p)->r0 = (SH_PACK_U32)(w))
#define PSX_ST_RGB(p, f, w)  (*(SH_PACK_U32*)&(p)->f  = (SH_PACK_U32)(w))
#define PSX_ST_UV(p, f, w)   (*(SH_PACK_U32*)&(p)->f  = (SH_PACK_U32)(w))
#define PSX_ST_XY(p, f, w)   (*(SH_PACK_U32*)&(p)->f  = (SH_PACK_U32)(w))
#define PSX_ST_XY_IMG(p, f, w) (*(SH_PACK_U32*)&(p)->f = (SH_PACK_U32)(w))
#define PSX_LD_CVEC(ptr)     (*(const SH_PACK_U32*)(ptr))
#define PSX_LD_XY(ptr)       (*(const SH_PACK_U32*)(ptr))

#endif /* big-endian */

#endif /* PSX_PACK_H */
