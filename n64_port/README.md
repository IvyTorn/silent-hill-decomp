# Silent Hill — Nintendo 64

A demake. The other ports exist to make the game run *better*; this one has to
make it run *smaller*, and that is a different job with a different set of
answers.

## Status

| Milestone | State |
|---|---|
| 0 — toolchain + repo | done |
| 1 — VR4300 compile gate | **green, 199/199** |
| 2 — link a `.z64`, reach `MainLoop` | **done** |
| 4 — storage (SD card + ROM pack) | **done — reads the disc** |
| 3 — RDP renderer | geometry done; **textures all miss TMEM, see below** |
| 5 — map overlays via libdragon DSO | **done — all 42 build, 2.3 MB** |
| 6 — save device | **done — SD, or a RAM device (sav:/) as fallback** |

Boot now runs Konami → KCET → AutoLoad → MovieIntro and draws 12 triangles a
frame.

Where it is today: the ROM boots, mounts a disc source, reads it, draws the
Konami logo, fades it, walks on through the TIM loads and holds 60 VPS. The
renderer draws real geometry. Textures are implemented but have not yet been
*seen* — nothing in the boot sequence so far puts a small-enough textured
triangle where a screenshot proves it, so treat that as written-but-unverified
until a map renders.

## Renderer

No Z-buffer, and that is correctness rather than economy: the PSX had none,
ordering is painter's from an OT that arrives already sorted, and a depth test
would start rejecting fragments the PSX drew — particularly among the
semitransparent draws that lean hardest on submission order.

`ShVertex` is `{pos[4], col[4], tex[2], spec[4]}`, which is exactly
`rdpq_triangle`'s layout, so untextured triangles are passed through as
`const float*` with no conversion. Textured ones stage through a 9-float array,
because rdpq wants S,T,W contiguous and `ShVertex` keeps W up in `pos[3]`.

**TMEM is the whole problem.** 4 KB total, and a 256-entry TLUT takes half,
leaving 2048 bytes of texels against a PSX that sampled a 1 MB VRAM freely. So
each triangle uploads its own UV bounding box, and 64×32 is the largest tile
that fits. Anything larger draws flat-shaded and increments `big=` in the
`[GPU]` census rather than drawing with wrong texels.

That counter is the number to watch, and it is now measured: **`tris=12 tex=0
big=8`**. Every textured triangle the boot screens submit exceeds 64×32, so
nothing has yet been drawn with a texture. That is the next piece of work, and
the fix is splitting oversized triangles or dropping to CI4 — *not* raising the
tile size, because there is nowhere for it to go.

### Open

- **`g_Nv2aFrameCount` reads 0 in the census every frame.** Ruled out: guard
  words either side of it survive (so it is not a ranged wild write), `s_inited`
  in the same file persists (so the GPU is not re-initialising), and nothing in
  the tree assigns to it. It matters because `psx_vram.c` keys its page LRU on
  it. A VR4300 `WatchLo` write-watch never fired — ares appears not to implement
  the Watch exception, so catching this may need hardware.
- **124 ms/frame** as measured, but the census is almost certainly measuring the
  debug overlay rather than the game: 22 rows × 39 characters of
  `graphics_draw_text` is ~55k CPU-blitted pixels per frame on a 93 MHz CPU.
  Measure again with `ShLogN64_ScreenEnable(0)` before drawing any conclusion
  about renderer cost.

### A bug class this target has and no other

A PSX RAM address such as `0x80191834` is **also a valid N64 KSEG0 address**.
On PC, Xbox or PSP a missed `PSX_ADDR()` remap writes to an unmapped pointer and
faults immediately; here it lands silently in our `.bss`. Anything that looks
like memory corrupting itself for no reason should be suspected of this first.

## Save device

`XboxFs_ResolveSaveDir` prefers `sd:/silenthill` (persistent) and falls back to
`sav:/`, a RAM filesystem in `savefs_n64.c`. Saving works for the session there
and the data is gone at power-off, which the log says at mount time.

This is not optional. `GameState_KcetLogo_MemCardCheck` loops on "rerun me next
frame" until the cards report ready, so **with no save location at all the boot
wedges on the KCET logo forever** and never reaches the title screen.

Measured image: text 911 KB, data 521 KB, bss 5.0 MB, **6.6 MB total**, against
8 MB. ROM 1.3 MB without a pack, 37 MB with the default one.

## Storage

`cd_n64.c` probes two sources, in this order:

| Source | What it is | Limit |
|---|---|---|
| `sd:/` | flashcart SD via libdragon's FAT | none in practice — put the 616 MB BIN on it |
| `rom:/` | DragonFS in the cartridge | 64 MB address window |

SD is probed **first** so a card always beats whatever is baked into the ROM.
Otherwise a trimmed test image silently shadows the real disc, and the symptom
is missing files rather than an obvious error.

The disc does not fit a cartridge, and truncating it does not work either —
`VIN/MAP0_S00.BIN` is at LBA 37551 while `1ST/` starts at 64, so what boot needs
is spread across the whole thing. `tools/mkdiscpack.py` instead builds a sparse
pack holding only the sectors the chosen directories occupy, cooked to 2048
bytes, with an index that preserves the original LBAs so the file table still
resolves:

```
python n64_port/tools/mkdiscpack.py "Silent Hill (USA).bin" \
    n64_port/filesystem/disc.shpak --dirs 1ST,ANIM,TIM,SND,MISC
```

| `--dirs` | cooked size |
|---|---|
| `1ST,ANIM` | 7.8 MB — fast iteration |
| `1ST,ANIM,TIM,SND,MISC` | 34.1 MB — default; boot, title, audio banks |
| everything except XA | 77.8 MB — SD only |

XA cannot be packed: Mode-2 Form-2 payloads do not survive the cooked form.
Streaming audio needs the raw BIN on SD, or the VADPCM conversion that milestone
6 will do anyway.

`n64_port/filesystem/` is gitignored. It holds disc data, which is not ours to
redistribute; the ROM builds fine without it and simply finds no disc.

### The SD card someone else can build

`sh_hardware.z64` carries the native world and character assets inside the ROM,
which makes it 36 MB and ties it to one person's converted data. For a build
that can be handed to someone, `tools/sh_n64_setup.py` does the whole card from
their own disc image instead:

```
python n64_port/tools/sh_n64_setup.py "Silent Hill (USA).bin" --out E:\
```

It writes `<out>/silenthill/` with `disc.shpak`, `gamedata/load/N64W`,
`gamedata/load/N64C`, a default `silenthill.cfg` and a README, and leaves an
existing cfg and the logs alone. The runtime probes the card before the
cartridge (`WOpenRoot` in `t3d_world.c`), so those files win over anything
baked in.

`tools/pack_setup_tool.py` bundles the script, the converters it drives, the USA
file table and a drag-and-drop `.cmd` into `build/sh_n64_setup.zip` — about
70 KB, no game data in it.

Three texture directories go into every `mkworld` run: `BG`, `TIM`, `ITEM`, in
that order. `ITEM` is the one that is easy to forget, and forgetting it drops a
pickup model from `<AREA>ITEM.SHW` with no error. `ER` has no `*_GLB.PLM`;
`SPR` and `THR` do.

## Debugging

There is no log file on a cartridge, and ares surfaces IS-Viewer only in a GUI
window a headless run never sees. `g_ShDebugLog` is therefore a `funopen` stream
that fans out to libdragon's stderr (IS-Viewer, and USB back to `sc64deployer`)
**and** a ring buffer painted over the frame. `ShLogN64_ScreenEnable(0)` turns
the on-screen half off once there is a picture worth looking at.

Two things that cost real time and will again:

- **rdpq calls are queued; `graphics_*` are immediate CPU writes.** Painting text
  before the queue drains puts it underneath whatever runs next. Use
  `rdpq_detach_wait()`, never `rdpq_detach_show()`, if the CPU is drawing too.
- **Build with `-g`** or libdragon's on-screen exception handler gives function
  names with no line numbers, and the frame that matters is usually the inlined
  one.

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
