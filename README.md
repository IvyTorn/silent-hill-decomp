# Silent Hill - Native Android Port

  **A native Android port built on the Silent Hill PSX decompilation.** It shares the whole engine with the [PC port](https://github.com/SlickAmogus/silent-hill-decomp/tree/pc-port): the same decompiled C source, compiled for ARM and running on [PsyCross](https://github.com/SlickAmogus/PsyCross) (our fork of the PsyQ compatibility layer) through SDL2 and OpenGL ES 3.0. It is **not an emulator** and **not a static recompilation**. Game logic and feel match the PSX original, and every enhancement is optional.

  **Development is heavily AI-assisted** (Claude Opus 4.6, 4.7 and the newer Fable model), and we're open about that. Every change is a reviewed, hand-directed edit to real source, tested against the original behaviour.

  Project Website: https://sh1pc.com/ <br/>
  Discord: https://discord.gg/JWuNzVsQbr

## Licensing at a glance

**The port is GPL-3.0.** That covers `pc_port/`, `android_port/` and every `SH_PC_PORT` addition to the decompiled sources.

**The decompilation is not licensed by this project.** `src/`, `include/`, `configs/`, `asm/`, `lib/` and `rom/` remain the work of the [silent-hill-decomp](https://github.com/shdecompilations/silent-hill-decomp) contributors.

**No game data is included.** Silent Hill is © Konami. You need your own legally obtained disc image; nothing from it ships in the APK.

Full breakdown: [COPYRIGHT.md](COPYRIGHT.md).

## Features

- **The full game.** Every map, boss, cutscene and ending. All 43 map overlays are linked into the app.
- **Touch controls.** A floating movement stick, drag-to-look, tap to act, and on-screen Aim / Item / Map / Start buttons. Menus work by tapping. The buttons send whatever your controller config has bound, so rebinds carry over.
- **Controllers.** Bluetooth and USB pads work, and the touch overlay steps aside when one is in use. Remap them under **Options > Controller Config**.
- **Graphics.** Renders at the device's native resolution in widescreen (Hor+) by default, at 60 fps. It starts locked to landscape; **Options > Screen Rotation** switches between Landscape, Auto (follows the sensor) and Portrait after a restart. PGXP perspective correction and the rest of the PC port's graphics options are available too.
- **Quick options overlay.** Graphics, HUD, audio and cheats, opened from an on-screen button.
- **RetroAchievements.** Sign in from **Options > System > Achievements** (softcore only).
- **Mods.** Loose-file replacements and DuckStation-style texture packs. See [Modding](#modding).
- **All regions.** USA, PAL and NTSC-J discs are auto-detected.

## Known Issues / Bugs

- **No FMV overrides.** There is no libjpeg or ffmpeg on Android, so HD AVI/MP4 movie replacements are ignored. The disc's own cutscenes still play.
- **No surround audio.** Audio is stereo, rendered in software through SDL, because Android has no OpenAL.
- **Memory on low-end devices.** Every map's working buffers are reserved up front (about 195 MB of `.bss`). It is paid only as maps are visited, but it is never released, so a long session on a 1 GB device can get killed by Android's low-memory killer.
- **Unsigned debug builds only**, no Play Store release.

Everything else that's known is on the [issues page](https://github.com/SlickAmogus/silent-hill-decomp/issues).

## Short Instructions

1. Install the APK (Android 5.0+, OpenGL ES 3.0, arm64-v8a or armeabi-v7a).
2. Open the app. On first run it asks where to keep game data: **phone storage** or an **SD card** (the card is the default when one is present).
3. Give it your disc image. The setup screen looks in Downloads and other common folders and offers to move the `.bin` into place, or you can pick the file yourself. Any filename works and no `.cue` is needed.

Game data lives in `Android/media/com.silenthill.port/` on the volume you chose. That folder needs no permissions and any file manager can open it, so you can reach your saves, `config.cfg`, mods and `SilentHill_<timestamp>.log` there.

## Controls

| Touch | Action |
|-------|--------|
| Drag, left side | Move (push to the edge to run) |
| Drag, right side | Look |
| Tap | Action: attack with a weapon ready, interact otherwise |
| On-screen buttons | Aim (hold), Item, Map, Start, and the quick options overlay |

A physical controller uses the standard PSX layout.

## Building

### How the Android build works

`android_port/` holds only the APK shell: the Gradle project, manifest, Java activities and a CMake shim. The game itself is `pc_port/`, built as `libmain.so`. Android-specific code stays behind `if(ANDROID)` / `#ifdef __ANDROID__`, so the port keeps up with PC development instead of turning into a fork.

- **Renderer:** PsyCross's GLES 3.0 path (`RENDERER_OGLES`) through SDL2.
- **Audio:** OpenAL is compiled out (`SH_NO_OPENAL`). The SPU and XA audio are rendered in software and played through SDL's AAudio/OpenSL output.
- **Maps:** Android can't `dlopen` map DLLs from a data folder, so `SH_STATIC_MAPS` is turned on automatically. Each overlay's symbols get a per-map prefix via `llvm-objcopy --redefine-syms` so all 43 can be linked together, and a generated registry replaces `dlsym`.
- **Disabled:** ffmpeg, the libjpeg MJPEG decoder and the launcher (it's C#/.NET). RetroAchievements stays on and uses `HttpURLConnection` through JNI in place of libcurl.
- **Startup:** `SetupActivity` handles choosing where data goes and installing the disc. `SilentHillActivity` (a subclass of `SDLActivity`) unpacks the port's own assets from the APK (fonts, `decal.png`, language packs, UI sounds) without overwriting existing files. `main()` then `chdir()`s into the data folder so every relative path resolves there.
- **ABIs:** `arm64-v8a` and `armeabi-v7a`. The 32-bit build exists for devices like the Arcade1Up MT8163 board, which run a 32-bit userspace. `RelWithDebInfo` is pinned in `app/build.gradle` because unoptimized builds crawl on weak hardware.

### Prerequisites

| Component | Version |
|-----------|---------|
| JDK | 17 (AGP 8.x will not run on newer) |
| Gradle | 8.9 |
| Android SDK | platform 34, build-tools 34.0.0 |
| NDK | 27.3.13750724 |
| CMake | 3.22.1 (SDK-bundled) |

Fetch the submodules (SDL2 and PsyCross):
```
git submodule update --init --recursive android_port/app/jni/SDL pc_port/PsyCross pc_port/third_party/rcheevos
```

### Build

```sh
./android_port/build_android.sh                  # assembleDebug
./android_port/build_android.sh assembleRelease
adb install -r android_port/app/build/outputs/apk/debug/app-debug.apk
```

The script defaults to the toolchain under `C:/Android/`. Set `JAVA_HOME_ANDROID` and `GRADLE_BIN` to use your own. See [`android_port/README.md`](android_port/README.md) for more technical notes.

## Modding

Mods are **on by default** on Android, because there is no launcher to turn them on. On first launch the game creates the mod folders and writes a quick-reference `gamedata/load/README.txt` inside the data folder. Restart the game after adding or removing files. **Options > Graphics > Load Mods** turns loose-file mods off.

- **Loose-file replacements:** `gamedata/load/<FOLDER>/<NAME>`, using the disc's own folder and file names (e.g. `gamedata/load/CHARA/HERO.TIM`). Case doesn't matter.
  - **Textures:** `.png` or `.dds` next to the name (`HERO.TIM.png` or `HERO.png`), or a replacement `.TIM`
  - **Models:** replacement `.TMD` / `.ILM` / `.IPD`, and `.glb` for inventory items
  - **Sounds:** `SND/<BANK>.VAB`, or a single sound as `SND/<BANK>.001.wav`
  - **Voices:** `XA/xa_0001.wav`, or `XA/msg_<KEY>.wav` for a text box
  - **Text:** `text_overrides.txt`, or `text_overrides/<name>.txt`
- **Texture packs:** DuckStation-format packs in `gamedata/texturemods/`, either as a folder or a `.zip`. Always on. Memory budgets for packs scale down to the device's RAM.
- **Not available:** FMV overrides (see Known Issues).

The mod *tools* (disc extraction, TIM ↔ PNG, character texture reference sheets, ILM ↔ OBJ model export) are in the PC launcher's Mod Manager. Build mods on a PC, then copy the resulting `gamedata/` files to the phone. Full file formats and workflows: [`Modding_And_Extraction_Guide.md`](pc_port/docs/Modding_And_Extraction_Guide.md), [`Model_Modding_Guide.md`](pc_port/docs/Model_Modding_Guide.md), [`Modern_Item_GLTF_Modding_Guide.md`](pc_port/docs/Modern_Item_GLTF_Modding_Guide.md).

## Support

I'm on Discord as **@KushAstronaut**, and the project has a Discord server: https://discord.gg/JWuNzVsQbr

<br/>

Silent Hill is © Konami and this does not contain any game assets. You must provide a legally obtained dump of Silent Hill for PSX to use.
