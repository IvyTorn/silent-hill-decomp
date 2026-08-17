/*
 * savefs_n64.h - the RAM save device, mounted at "sav:/".
 *
 * The fallback save location when no flashcart SD card is present. Without SOME
 * writable location the KCET-logo memory-card check never completes and the
 * boot wedges there forever; see savefs_n64.c.
 */
#ifndef SAVEFS_N64_H
#define SAVEFS_N64_H

/* Mount it. Returns 1 on success. Safe to call more than once. */
int SaveFs_N64Init(void);

#endif /* SAVEFS_N64_H */
