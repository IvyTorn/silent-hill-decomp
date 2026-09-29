## Silent Hill AI-Assisted Nintendo 64 Port

  This is an experimental Nintendo 64 port of Silent Hill 1, built on top of the PSX decompilation and the PC/Xbox port lineage, made with heavy AI-Assistance from Claude Opus 4.6, 4.7 and 5.<br/>
  <br/>PsyCross: https://github.com/OpenDriver2/PsyCross
  <br/>libdragon: https://github.com/DragonMinded/libdragon
  <br/>Tiny3D: https://github.com/HailToDodongo/tiny3d

### Status

The world and characters render natively, and the game is playable from the title screen through the streets and into the hospital. Performance is the main thing being worked on, and some cutscenes and room transitions are still wrong. FMVs are not wired up yet.

### How it is made

The N64 is the hardest target this codebase has been pointed at. It has 8 MB of RAM against the PC port's gigabytes, a fixed-function rasteriser instead of a shader pipeline, and a big-endian MIPS CPU reading disc data that was authored little-endian. A few of the things that follow from that:

- **Two rendering paths.** Menus, characters, items and effects still go through the PSX GPU emulation inherited from the Xbox port: the game builds an ordering table of PSX primitives and those are translated to RDP commands. The world does not. It is drawn natively through Tiny3D, because pushing a room's geometry per-primitive through the emulated path is far too slow.

- **The world is compiled ahead of time.** `n64_port/tools/mkworld.py` turns each map cell's IPD into a draw stream (`.SHW`) plus a per-area tile store (`.SHT`): texture tiles cut along the UV clusters the polygons actually use, palettes converted to the RDP's format, vertices kept as 16-bit model-local values. At runtime a chunk's draw stream is recorded once into an RSP command block and replayed. `mkchara.py` does the same for Harry, with his six weapons baked in as extra parts so an equipped weapon draws with the right bone.

- **One map overlay in memory at a time.** The other ports link all 42 map modules into the executable, which is how the PSP build reaches 17.7 MB. That does not fit here, so each map is built as a libdragon DSO and loaded on room transition, which is close to what the PSX itself did. They are loaded into the address window the dead PSX BODYPROG segment used to occupy.

- **Disc data.** The port reads the original disc. A flashcart SD card can hold the whole BIN, but an emulator has no SD card and a cartridge has a 64 MB window, so `mkdiscpack.py` builds a sparse pack holding only the sectors the chosen directories occupy, cooked to 2048 bytes, with the original LBAs preserved so the game's file table still resolves.

- **Getting your own data onto a card.** `n64_port/tools/sh_n64_setup.py`, with a drag-and-drop `.cmd` wrapper, takes a disc image and writes the whole `silenthill/` tree for the card: the sector pack, the world and character assets, a default config and a README.

- **Endianness.** Anything read straight off the disc is little-endian and has to be decoded explicitly. This is the single most common source of bugs on this target, and it shows up in places that look nothing like data: animation tables, collision fields, packed bitfields.

Build and debugging notes live in `n64_port/README.md`.

### Planned Features

- Better performance, which mostly means feeding the RDP less per pixel
- FMV playback
- Native rendering for the remaining areas

 ### Support
  Project Website: https://sh1pc.com/ <br/>
  Discord: https://discord.gg/JWuNzVsQbr
  
  I work with more than just AI. If you like what I do:\
  [![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/F1F3K8V3B)


<br/>

Silent Hill is © Konami and this does not contain any game assets. You must provide a legally obtained dump of Silent Hill for PSX to use.
