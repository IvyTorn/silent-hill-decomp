#ifndef SH_T3D_H
#define SH_T3D_H

/* Tiny3D bridge. Deliberately libdragon-free so both sides of the include
 * firewall can call it; everything Tiny3D lives in n64_port/src/t3d_*.c. */

void ShT3d_Init(void);

/* Stage-0 spike: draws a shaded spinning quad through the RSP each frame.
 * Exists to prove ucode load + RSP transform + RDP output coexist with the
 * PSX path in one frame. Remove when the world renderer replaces it. */
void ShT3d_SpikeDraw(void);

/* ---- native world renderer (SHW chunks + SHT tiles) ---- */

/* Called from IpdHeader_FixOffsets once a chunk's IPD is loaded and fixed.
 * ipdName is the file-table name ("ERFF00.IPD"); the HAL loads the matching
 * N64W/<name>.SHW and the area's N64W/<prefix>.SHT on first use. Safe to call
 * repeatedly (keyed on cell coords). */
void ShT3d_WorldChunkLoaded(const char* ipdName, int cellX, int cellZ);

/* Called when a chunk slot is being reused for a different cell, and on map
 * teardown with no arguments via ShT3d_WorldReset. */
void ShT3d_WorldChunkEvict(int cellX, int cellZ);
void ShT3d_WorldReset(void);

/* Draw one model buffer of a resident native chunk. Returns 1 if drawn
 * (caller skips its PSX per-prim path), 0 if the chunk/buffer has no native
 * data (caller falls back). Runs inside the game's world-draw pass; the HAL
 * lazily performs its per-frame camera/state setup on the first call of a
 * frame. */
int ShT3d_WorldDrawBuffer(int cellX, int cellZ, int bufIdx);

/* Frame boundary notifications from gpu_rdp.c. */
void ShT3d_NotifyFrameBegin(void);
void ShT3d_NotifyFrameEnd(void);

#endif
