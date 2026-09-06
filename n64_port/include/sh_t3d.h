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

/* Camera handoff, once per chunk before the buffer draws: wsMatrix is
 * &GsWSMATRIX for the view ROTATION (s16 m[3][3] Q12; its t is zero on this
 * port). camX/Y/Z are the camera world position, Q8 (D_800C3868.t). plX/Y/Z
 * are the player's world position, Q8 (the foreground/background split plane).
 * h is the GTE projection distance; ofx/ofy the geometry offset. */
void ShT3d_WorldViewSet(const void* wsMatrix, int camX, int camY, int camZ,
                        int plX, int plY, int plZ,
                        int h, int ofx, int ofy);

/* Compose the game's OWN instance->view matrix (defined in
 * bodyprog_80040B74.c beside the chunk-draw path it mirrors): runs the
 * fabricated coord through Vw_CoordToWorldAndViewMatrices so the native world
 * inherits VbWvsMatrix (3/4 Y NTSC scale included), the camera subtract, and
 * any future view change. rot9 row-major Q12 (file order), trans3 Q8 world;
 * outputs the composed view rotation/translation likewise. */
void ShT3d_ComposeInstanceView(const short* rot9, const int* trans3,
                               short* outRot9, int* outTrans3);

/* Frame boundary notifications from gpu_rdp.c. */
void ShT3d_NotifyFrameBegin(void);
void ShT3d_NotifyFrameEnd(void);

/* Draw the recorded world buffers. WorldFlush draws the BACKGROUND (instances
 * farther than the player) before GsDrawOt(OT0); WorldFlushForeground draws
 * the FOREGROUND (nearer than the player) after OT0 and before OT2, so world
 * geometry can occlude the characters. Both run in one frame. */
void ShT3d_WorldFlush(void);
void ShT3d_WorldFlushForeground(void);

/* 1 if a native chunk drew geometry this frame. The full-screen fog/
 * brightness tint quads in Gfx_2dEffectsDraw are meant to blend INTO the PSX
 * OT world; the native world is already on the framebuffer, so on this port
 * those quads darken it to black instead. Skip them where native drew (its
 * own fog/brightness pass replaces them); unconverted areas keep theirs. */
int ShT3d_WorldDrewThisFrame(void);

/* Native characters (Phase C2). A character .ILM is rigid parts, one per
 * bone; each part is drawn by the RSP with the game's own bone view matrix.
 * DrawBegin(isHarry) returns 1 when the character draws natively -- the bone
 * loop then feeds CharaBone(partIdx = the bone's ILM model index, the bone's
 * composed view matrix: Q12 rotation row-major m9, Q8 translation t3) and
 * SKIPS the software-GTE per-part draw; 0 = PSX path as before. CharaFlush
 * draws all fed parts tile-grouped, between the background world pass and
 * GsDrawOt(OT0). */
int  ShT3d_CharaDrawBegin(int isHarry);
void ShT3d_CharaDrawEnd(void);
int  ShT3d_CharaBone(int partIdx, const short* m9, const int* t3);
void ShT3d_CharaFlush(void);

#endif
