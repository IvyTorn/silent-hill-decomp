/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_hwperf.h - the two architecture-specific instructions the port leans on.
 *
 * A store barrier before handing a buffer to the GPU, and a free-running counter
 * for the render/FS phase timers. Both were open-coded as x86 asm (sfence /
 * rdtsc) in nine places, which meant adding a second console architecture
 * required editing the same two instructions nine times.
 */
#ifndef SH_HWPERF_H
#define SH_HWPERF_H

#if defined(__powerpc__) || defined(__PPC__)

#include <ppc/timebase.h>

/* eieio orders device-memory stores, which is what sfence was doing here.
 * sync would also work but is a full pipeline flush. */
#define SH_STORE_BARRIER() __asm__ __volatile__("eieio" ::: "memory")

/* NOT cycles on 360: the Xenon time base ticks at PPC_TIMEBASE_FREQ
 * (~49.875 MHz), not core clock. Only RATIOS between readings carry across
 * platforms -- any absolute "cycles -> ms" divisor is x86-only. */
#define SH_CYCLES() ((unsigned long long)mftb())

#elif defined(__i386__) || defined(__x86_64__)

#define SH_STORE_BARRIER() __asm__ __volatile__("sfence" ::: "memory")

static inline unsigned long long sh_cycles_(void)
{
    unsigned lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}
#define SH_CYCLES() sh_cycles_()
/* Original Xbox: rdtsc at 733 MHz. */
#define SH_CYCLES_TO_US(c) ((unsigned)((unsigned long long)(c) / 733ULL))

#elif defined(SH_N64_PORT)

/* libdragon's COUNT register, 46.875 MHz (CPU/2). Implemented in stubs_n64.c so
 * this header stays free of <libdragon.h>; the per-frame [OTT] split read 0
 * for every phase while this fell through to the stub below. */
unsigned long long sh_cycles_(void);
void               sh_dma_writeback_(const void* p, unsigned n);
#define SH_STORE_BARRIER() __sync_synchronize()
#define SH_CYCLES()        sh_cycles_()
#define SH_CYCLES_TO_US(c) ((unsigned)(((unsigned long long)(c) * 8ULL) / 375ULL))
/* Make CPU-written memory visible to a DMA reader (the RDP). The N64's data
 * cache is write-back and the RDP reads RAM, so a buffer decoded by CPU stores
 * has to be written back once, at decode time -- doing it per texture tile
 * cost dozens of cache calls per triangle. */
#define SH_DMA_WRITEBACK(p, n) sh_dma_writeback_((p), (n))

#else

#define SH_STORE_BARRIER() __sync_synchronize()
#define SH_CYCLES()        0ULL
#define SH_CYCLES_TO_US(c) ((unsigned)(c))

#endif

#ifndef SH_CYCLES_TO_US
/* Xenon time base, ~49.875 MHz. */
#define SH_CYCLES_TO_US(c) ((unsigned)(((unsigned long long)(c) * 8ULL) / 399ULL))
#endif
#ifndef SH_DMA_WRITEBACK
#define SH_DMA_WRITEBACK(p, n) ((void)0)
#endif

#endif /* SH_HWPERF_H */
