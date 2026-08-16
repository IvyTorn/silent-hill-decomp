# Silent Hill — Nintendo 64

A demake. The other ports exist to make the game run *better*; this one has to
make it run *smaller*, and that is a different job with a different set of
answers.

## Status

| Milestone | State |
|---|---|
| 0 — toolchain + repo | done |
| 1 — VR4300 compile gate | **green, 199/199** |
| 2 — link a `.z64`, reach `MainLoop` | **done — boots in ares, runs one frame** |
| 3 — RDP renderer | not started |
| 4 — storage: SD card via `fat.h` | **next, and it is what MainLoop is blocked on** |
| 5 — map overlays via libdragon DSO | not started |

Where it stops today: the ROM boots, `main` brings the HAL up, `MainLoop` runs
exactly one frame, and then blocks. That is the honest consequence of `cd_n64.c`
reporting failure on every read — there is no disc, so the first load never
completes. Milestone 4 is the unblock, and it is deliberately ahead of the
renderer: there is nothing to draw until something can be read.

Measured image: text 833 KB, data 516 KB, bss 5.0 MB, **6.63 MB total**,
against 8 MB. ROM 1.28 MB.

## Base: the Xbox 360 port

Not the PSP port, even though the PSP is the other MIPS target and the instinct
is to reach for it.

libdragon builds `-mabi=o64`: 32-bit pointers, 32-bit `long`, 64-bit registers,
big-endian. The Xbox 360 port's `-m32` PowerPC is the same data model in every
respect that matters to C. The PSP is 32-bit *little*-endian — its gates mean
"PSX's own byte order, no conversion needed", which is the opposite of what this
target needs.

Endianness is also the half that fails silently. It reorders bitfields as well
as bytes, and the decomp overlays structs directly onto disc data. Getting that
wrong does not produce a crash, it produces a subtly wrong game. MIPS-vs-PPC, by
comparison, is nothing at the C level.

The measure of how much of that work was real corrections rather than
platform gates: the entire `SH_XBOX360_PORT` surface in the shared tree is
**8 files**. Almost everything the 360 port fixed, it fixed for good.

## Toolchain

`ghcr.io/dragonminded/libdragon:latest` — mips64-elf-gcc 14.2.0 + newlib.
Already on this machine from the SummerCart64 menu work.

The image is *only* the compiler. libdragon itself is a separate checkout
mounted at `/libdragon`, so the library stays rebuildable and bisectable
without touching a container image.

```sh
./n64_port/docker_run.sh bash ./n64_port/n64_gate.sh          # the gate
./n64_port/docker_run.sh bash ./n64_port/n64_gate.sh src/main # one subtree
SH_OPT='-O2 -fno-strict-aliasing -fwrapv' ./n64_port/docker_run.sh \
    bash ./n64_port/n64_gate.sh                               # warning harvest
```

`SH_OPT` is an override with no default, deliberately: a warning-harvest run
must not be able to change what ships.

### Two include sets

psyq ships a `kernel.h` and a `sys/ioctl.h` that collide by name with
libdragon's. Rather than rename anything, the gate keeps two include paths:
game TUs get an include set with **no libdragon on it at all**, and HAL TUs
under `n64_port/src` get one where libdragon comes first and psyq is absent.
The collision cannot arise in either.

The practical consequence: shared game code cannot call libdragon directly.
Anything it needs goes through a `n64_port/include` header that the HAL
implements. `libgs_stub.c`'s monotonic counter is the first example — it reads
COP0 Count with inline asm rather than `TICKS_READ()`.

## The memory budget

This is the whole problem. Measured from the gate's own objects at `-O0`:

```
text  2,058,615      data  83,066      bss  9,636,035
```

9.2 MB of `.bss` against 4 MB of RDRAM, or 8 MB with an Expansion Pak. But
almost all of it belongs to a HAL this port replaces:

| Symbol | Size | Owner | Fate |
|---|---:|---|---|
| `s_voicePcm` | 3.0 MB | `xbox_port/src/audio_xbox.c` | gone — stream instead |
| `g_PsxRam` | 3.0 MB | `pc_port/src/psx_memory.c` | **stays**, trims to 2 MB |
| `s_vram` | 1.0 MB | `xbox_port/src/gpu_xbox.c` | gone — RDP draws natively |
| `s_fbReadbackBuf` | 573 KB | `xbox_port/src/gpu_xbox.c` | gone |
| `s_spuRam` | 512 KB | `xbox_port/src/audio_xbox.c` | gone — RSP mixer |
| `s_poolBoneCoords` | 205 KB | `pc_port/src/pc_chara_pool.c` | stays, shrinks with the enemy cap |
| `s_combBuf` | 114 KB | `xbox_port/src/audio_xbox.c` | gone |
| `g_SysWork` | 38 KB | game | stays |

`g_PsxRam` is declared 3 MB but `PSX_ADDR` masks with `0x1FFFFF`, so only the
low 2 MB is ever addressable. That 1 MB is free.

Everything the *game itself* owns — `g_SysWork` 38 KB, `g_FileTable` 25 KB,
`g_WorldGfxWork` 24 KB, `g_Particles` 16 KB, `g_Map` 16 KB — is small, because
it was written for a 2 MB PSX in the first place. That is the reason this port
is possible at all.

Projected, with an N64 HAL in place and `-Os`:

```
text + rodata   ~1.5 MB
bss             ~2.9 MB   (2.0 PSX RAM + ~0.9 game statics)
map overlay     ~0.2 MB   (map0_s00 measures 137 KB text + 14 KB data)
framebuffers    ~450 KB   (320x240x16, double-buffered, + Z)
heap / audio / RSP        ~0.5 MB
                -------
                ~5.5 MB
```

**The Expansion Pak is required.** 8 MB fits with ~2.5 MB spare for textures;
4 MB does not fit without breaking the fixed PSX memory layout, which is not
worth doing. Every emulator and every flashcart supports the Pak.

## Plan per subsystem

**Renderer — rdpq, not libdragon GL.** The game does its own transform through
PsyCross's software GTE and hands the result to the OT as screen-space
primitives. It never wants a matrix stack, so OpenGL would mean undoing work
the game already did. `rdpq` takes screen-space triangles directly, which is
the same shape the PSP port's GE backend has.

The wall is **TMEM: 4 KB**. PSX had a 1 MB VRAM and switched tpage per
primitive for free; on N64 every texture change is a DMA. Mitigations, in
order: PSX textures are already 4bpp/8bpp CLUT and N64 CI4/CI8 with a TLUT is
the same idea, so the conversion is near-lossless and cheap; sort the OT by
texture within a bucket; drop to 32×32 tiles for world geometry.

**Audio — no SPU exists.** The RSP mixes in software via libdragon's `mixer`.
VAG converts offline to VADPCM (`wav64` compression level 1). The 496 MB of
CD-XA streaming audio also becomes `wav64`, streamed rather than resident.

**FMV — libdragon has an MPEG-1 decoder** (`mpeg2.h` + RSP YUV blitting in
`yuv.h`). Transcode the STR files to MPEG-1 at roughly 160×120. This is the
same shape of answer RE2 used, without having to write the codec. Not on the
path to a first boot; it comes after milestone 3.

**Map overlays — libdragon DSO** (`dlfcn.h`, `n64dso`). The PC/PSP ports link
all 42 maps statically because they have the RAM to; a 17.7 MB static image is
what that costs on PSP. N64 goes back to what the PSX actually did: one
overlay resident at a time, ~150 KB each, loaded on room transition. The
mechanism already exists in libdragon and does not need writing.

**Storage.** SummerCart64's SD card via libdragon's `fat.h` is the development
target and maps cleanly onto the existing CD abstraction — it is a seekable
block device with a directory, which is all `fsqueue` wants. A ROM build using
`dragonfs` stays possible for a real cartridge later; the constraint there is
64 MB, which means dropping XA and recompressing BG, not a different design.

## What this port does that the others do not

The other ports fight to look better. This one has to look worse on purpose,
and the levers are already in the codebase:

- internal resolution at 256×224 or lower
- culling on, aggressively
- map preloading off
- fewer concurrent chunk loads
- the enemy cap back down from 32 toward the original 6

The Android port is being tuned for a weak arcade cabinet and is the closest
thing to a reference for which of these actually buy frames.
