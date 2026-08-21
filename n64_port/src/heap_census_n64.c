/*
 * heap_census_n64.c - names where the heap goes.
 *
 * The map cannot load because at title time only ~87 KB of a ~1.3 MB heap is
 * free, and roughly 600 KB of the usage matches nothing we knowingly allocate.
 * On a machine this small "who malloc'd that" IS the debugging question, and
 * there is no tool for it -- so the linker's --wrap answers it: every
 * allocation of 16 KB or more logs size, caller and running totals. Small
 * allocations stay silent; the mystery is not made of small ones.
 *
 * Diagnostic; costs a compare per malloc when quiet. Remove with the wrap
 * flags in build_n64.sh when the budget question is settled.
 */
#include <libdragon.h>

#include <stdio.h>

#include "sh_log.h"

extern void* __real_malloc(size_t n);
extern void* __real_calloc(size_t n, size_t sz);

#define CENSUS_MIN (16 * 1024)

static void Census(const char* what, size_t bytes, void* ret, void* caller)
{
    heap_stats_t h;
    sys_get_heap_stats(&h);
    SH_DBG("[HEAP] %s %u KB -> %p ra=%p (used %u/%u KB)",
           what, (unsigned)(bytes / 1024), ret, caller,
           (unsigned)(h.used / 1024), (unsigned)(h.total / 1024));
}

void* __wrap_malloc(size_t n)
{
    void* p = __real_malloc(n);
    if (n >= CENSUS_MIN)
        Census("malloc", n, p, __builtin_return_address(0));
    return p;
}

void* __wrap_calloc(size_t n, size_t sz)
{
    void* p = __real_calloc(n, sz);
    if (n * sz >= CENSUS_MIN)
        Census("calloc", n * sz, p, __builtin_return_address(0));
    return p;
}
