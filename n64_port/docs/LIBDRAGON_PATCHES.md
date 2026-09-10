# Local libdragon patches (SH_PATCH)

The libdragon SOURCE this port builds against lives OUTSIDE this repository
(`../libdragon`, mounted at `/libdragon` by `docker_run.sh`, installed into
`../n64_inst` by `build_libdragon.sh`). It carries local patches that version
control here cannot see. Every patch site is marked `SH_PATCH` -- to audit or
re-apply after refreshing the checkout:

    grep -rn "SH_PATCH" ../libdragon/src

Current patches (2026-09-05):

1. `src/rspq/rspq.c` (4 sites) and `src/display.c` (1 site): every
   gameplay-reachable `RSP_WAIT_LOOP(200)` raised to 5000ms. The 200ms
   watchdog treats "RSP hasn't finished a buffer in 200ms" as a crash, but a
   fill-saturated frame on this title legitimately queues more RDP work than
   that (loading screens: ~150ms busy + 5ms upload spikes; room transitions
   run `rspq_wait`). The watchdog was executing merely-slow frames: every
   hardware "RSP crash" dump traced to one of these waits, with the reporter
   varying by which caller needed a buffer next. Raised 200->2000->5000: every
   dump showed the RSP ALIVE (halt=0) + RDP idle = a transient stall, and a
   next-room chunk-stream frame exceeded 2s. 5s still catches genuine hangs.

2. `src/rsp.c` `__rsp_crash`: calls the weak hooks `ShN64_RspCrashCommit`
   and `ShN64_RspCrashDetail` (both in `n64_port/src/sh_log_n64.c`) BEFORE
   `console_init`. The console's display re-init (640x240, 2 buffers) OOMs on
   this heap-starved game (sbrk_top framebuffers cannot be reclaimed mid-run)
   and dies on its own `surfaces[i].buffer != NULL` assert, so the on-screen
   dump only ever shows the assert cascade. The hooks land in the SD log
   what the console would have shown: ucode/pc/site (`[CRASH] RSP crash:`),
   then SP/DP status with the rspq signal bits decoded, libdragon's own
   "did DP_CURRENT move" RDP-hang probe, the RSP's queue read pointer
   (`rsp_queue_t.rspq_dram_addr + gp`, as rspq_crash_handler computes it)
   against the CPU-side buffers, and the 16 queue words at that pointer.
   Reading note: pc=018 in rsp_queue is `wakeup:` right after the `break`
   in RSPQCmd_WaitNewInput -- the RSP read a ZERO command word and slept.
   `rsp.c` includes `rspq/rspq_internal.h` for the struct; `-Isrc` is on the
   libdragon build's include path (rdpq.c does the same).

6. `include/rdpq_constants.h` `RDPQ_DYNAMIC_BUFFER_SIZE` -- **REVERTED to the
   64KB default (2026-09-09).** The 256/128KB bumps were chasing an RDP overrun
   that was actually INDUCED by the app's per-frame `rspq_wait()` drain (item 5,
   also removed). With clean libdragon backpressure the 64KB default is stable,
   and reverting freed the heap to re-enable the character Z-buffer.

5. `include/rspq_constants.h` `RSPQ_DRAM_LOWPRI_BUFFER_SIZE` 0x200 -> 0x4000
   (KEPT). The title emits ~5000 rspq words/frame, so the 512-word queue
   switched ~10x/frame and the RSP raced a buffer-end switch, running off the
   buffer into the uncleared gap. 16K words holds a whole frame. **The
   companion per-frame `rspq_wait()` drain in GpuNv2a_FrameBegin was REMOVED
   (2026-09-09)** -- it reached into libdragon's buffer machinery every frame
   and INDUCED the RDP-overrun crashes; the enlarged queue alone + native
   backpressure is the stable config. Do NOT re-add the drain.

4. `src/rspq/rspq.c` `rspq_try_heal` -- **DISABLED via a `0 &&` gate on both
   call sites (2026-09-09)**, kept in source for reference. It only ever
   refused (kind=2), and was part of the band-aid stack that induced the RDP
   crashes. `src/rsp.c` `__rsp_set_cur_ucode` remains (harmless). The heal
   snapshotted IMEM/DMEM and NOOPed a lone zero command word; the "RSP crash"
   it targeted turned out to be the induced buffer-switch race, gone once the
   drain/heal were removed.

3. `src/t3d/t3dmath.c` in ../tiny3d (pinned c2cdbf2): `t3d_mat4_to_frustum`
   skips normalizing a plane when `len < 1e-6f` (off-centre projections make
   a zero-length plane -> NaN -> FPU trap). Rebuilt into n64_inst the same
   way.

After changing any of these:

    ./n64_port/docker_run.sh bash ./n64_port/build_libdragon.sh
    ./n64_port/docker_run.sh bash ./n64_port/build_n64.sh
