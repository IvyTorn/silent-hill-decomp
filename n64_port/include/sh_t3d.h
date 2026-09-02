#ifndef SH_T3D_H
#define SH_T3D_H

/* Tiny3D bridge. Deliberately libdragon-free so both sides of the include
 * firewall can call it; everything Tiny3D lives in t3d_n64.c. */

void ShT3d_Init(void);

/* Stage-0 spike: draws a shaded spinning quad through the RSP each frame.
 * Exists to prove ucode load + RSP transform + RDP output coexist with the
 * PSX path in one frame. Remove when the world renderer replaces it. */
void ShT3d_SpikeDraw(void);

#endif
