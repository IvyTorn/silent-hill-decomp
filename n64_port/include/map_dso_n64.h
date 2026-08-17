/*
 * map_dso_n64.h - map overlay loading, across the include firewall.
 *
 * dlopen/dlsym live in libdragon's dlfcn.h, and map_n64.c is a GAME TU that
 * must not see libdragon (its kernel.h collides with psyq's). So the loader
 * lives in a HAL TU and is reached through this header, which mentions no
 * libdragon type at all -- the same arrangement libgs_stub.c uses to read COP0
 * Count without including n64sys.h.
 */
#ifndef MAP_DSO_N64_H
#define MAP_DSO_N64_H

/* Load <mapName>.dso and return its g_MapOverlayHeader_<mapName>, or NULL.
 *
 * Closes whatever overlay was open before, which is the PSX's own behaviour:
 * one overlay resident at a time, loaded on room transition. The caller must
 * not hold a header across a call for a different map. */
void* MapDso_Open(const char* mapName);

/* Name of the currently open overlay, or "" -- for logging. */
const char* MapDso_Current(void);

#endif /* MAP_DSO_N64_H */
