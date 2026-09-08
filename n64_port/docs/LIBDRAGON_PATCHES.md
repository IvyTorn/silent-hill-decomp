# Local libdragon patches (SH_PATCH)

The libdragon SOURCE this port builds against lives OUTSIDE this repository
(`../libdragon`, mounted at `/libdragon` by `docker_run.sh`, installed into
`../n64_inst` by `build_libdragon.sh`). It carries local patches that version
control here cannot see. Every patch site is marked `SH_PATCH` -- to audit or
re-apply after refreshing the checkout:

    grep -rn "SH_PATCH" ../libdragon/src

Current patches (2026-09-05):

1. `src/rspq/rspq.c` (4 sites) and `src/display.c` (1 site): every
   gameplay-reachable `RSP_WAIT_LOOP(200)` raised to 2000ms. The 200ms
   watchdog treats "RSP hasn't finished a buffer in 200ms" as a crash, but a
   fill-saturated frame on this title legitimately queues more RDP work than
   that (loading screens: ~150ms busy + 5ms upload spikes; room transitions
   run `rspq_wait`). The watchdog was executing merely-slow frames: every
   hardware "RSP crash" dump traced to one of these waits, with the reporter
   varying by which caller needed a buffer next. 2s still catches genuine
   hangs.

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

4. `src/rspq/rspq.c` `rspq_try_heal` + calls in the `rspq_next_buffer` and
   `rspq_syncpoint_wait` wait loops (after 300ms, up to 4 per wait); `src/rsp.c`
   `__rsp_set_cur_ucode`. The hardware deadlock behind every "RSP crash pc=018"
   was the RSP parked on ONE zero command word in an otherwise intact queue
   (a rdpq_set_tile header missing, its argument and all later commands
   present). The heal snapshots IMEM/DMEM, runs the crash ucode to read gp,
   computes the parked address (`rspq_dram_addr + gp`), NOOPs the hole and the
   lost command's zero-top-byte argument words, puts the snapshot back and
   restarts the ucode at `_start` with SIG_MORE set and `rspq_dram_addr` moved
   to the (8-byte aligned) parked position -- the word before it, when the
   position is 4 mod 8, is the previous command's already-executed tail and
   is NOOPed too. Refuses (kind 2) when the position is outside the lowpri
   buffers or the hole is followed by a word whose top byte is 0x01..0x0F (an
   internal-command look-alike). The app's weak `ShN64_RspqHealed` logs each
   event as `[RSPQ-HEAL]`; a `kind=1` means a lost wake-up rather than a hole.
   This is a mitigation with instrumentation, not the fix: the writer of the
   zero is still unknown (not the RDP -- scissor now clamped; not the RSP's
   own DMAs; not an interrupt -- no rspq use outside the main loop).

3. `src/t3d/t3dmath.c` in ../tiny3d (pinned c2cdbf2): `t3d_mat4_to_frustum`
   skips normalizing a plane when `len < 1e-6f` (off-centre projections make
   a zero-length plane -> NaN -> FPU trap). Rebuilt into n64_inst the same
   way.

After changing any of these:

    ./n64_port/docker_run.sh bash ./n64_port/build_libdragon.sh
    ./n64_port/docker_run.sh bash ./n64_port/build_n64.sh
