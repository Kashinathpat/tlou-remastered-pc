# The Last of Us Remastered — PC port

Runs the PS4 **The Last of Us Remastered** (CUSA00552, with its update 1.11) natively on Windows.
The game's own x86-64 code runs directly on the PC; a runtime supplies the PS4 system libraries,
and a Vulkan renderer (the shadPS4 video core) executes the PS4 GPU work.

> **No game files are in this repository.** You need your own copy of the game (the dumped
> CUSA00552 folder, and its update). This project is not affiliated with Sony Interactive
> Entertainment or Naughty Dog.

## Why

Why The Last Of Us Remastered (PS4 version) is superior to The Last Of Us Part I (remake)?

- The PS4 remaster preserves the artistic style of the PS3 version and just improves the resolution and graphics.
- Last version with multiplayer support, having multiplayer code is much easier to implement community/unofficial multiplayer
- Use less resources and storage space (no 4K textures, less polygons, no ray-tracing) than the remake version, which has a history of performance problems and poor optimization on PC.

## Status

Work in progress, reported playable from start to finish by a player, with one crash along the way
that is not fixed yet (a workaround is under Known issues), and with rough edges.

- **Frame rate:** on a laptop with a Core i7-8750H and a GTX 1050 Ti Mobile (4 GB), 60 FPS in light
  areas and about 40-44 FPS in the heaviest area tested, where the CPU side of the renderer is the
  limit (the draw recording thread at about 87%): lowering the resolution barely helps there.
- **Frame pacing:** frame times swing while the game runs below its limit, and a fixed 30 FPS limit
  (launcher, or the in-game menu) makes motion clearly more even.
- **Known issues:**
  - NPCs sometimes move in jumps. It comes and goes between runs of the same build; the cause is
    not settled. The bursts of long frames that seemed to go with it shrank with the faster guest
    condition variables, and a fixed 30 FPS limit had no jumps in the sessions tried.
  - A frame that compiles many new shaders can pause the game for a moment, mostly the first time
    an area is played. The game's own frame watchdog is patched from 2 s to 20 s for that
    (`TLOU_FIXES=none` leaves the game unpatched). If a load ever hangs, start with
    `GOW3_FRAMES_IN_FLIGHT=0` (slower).
  - Shots pass through some parts of objects with no effect, though they hit walls and most
    objects where the crosshair is. One firefly pendant, hanging from a tree in the courtyard
    after the quarantine zone, cannot be shot down for that reason. Ruled out so far: the
    renderer's CPU/GPU data exchange, the port's patches and red-zone protection, and the CPU's
    approximate math instructions.
  - On NVIDIA graphics cards older than Turing (GTX 900 and 10 series), the game's cloth solver for
    its largest cloths (`Cs_ClothSimulate512`) lost the Vulkan device in Pittsburgh, once Henry and
    Sam appear: the runtime stopped with `Device lost during submit`, and Windows' System event log
    shows `nvlddmkm` "SKEDCHECK19_CTA_REGISTER_CONSUMPTION failed". A replay of that route lost it
    on 17 of 18 runs, on drivers 582.53 and 582.78, whatever was changed in the shader. Its one
    workgroup of 512 threads needs all the registers of a multiprocessor; on those cards it now
    runs as 256 threads that simulate 2 vertices each, with the same arithmetic (the shader
    compiler's "lanes", `lane_coarsening.cpp`): no device loss in 6 replays of
    that route. "Cloth workaround" in the launcher's Startup tab, unchecked
    (`GOW3_PASCAL_CLOTH512=1`), runs the solver as it is; `GOW3_PASCAL_CLOTH512=skip` does not
    run it there (that cloth then follows the body). Newer cards run it as it is (an RTX 5060
    went through the same scene). The two-stage draw
    pipeline, blamed before, made the loss more frequent but was not its cause, and is on again
    ("Draw pipeline" in the launcher's Startup tab, `GOW3_DRAW_PIPE=0` turns it off).
  - Soft shadows can look detached from objects, with a thin light outline around them and little
    darkening where they touch the ground, most visibly under vehicle wheels. The same spot on a
    PS4 has the contact darkening this port misses, so it is a port bug; the pass that draws it
    has not been identified yet.
- **Not available for this game yet:** a true mouse camera (the mouse drives the right stick
  instead, though no longer capped at the stick's top speed), the draw distance sliders and
  keyboard button prompts, which need the game's own camera code, settings block and prompt
  textures to be located.

## Requirements

- Windows 10 or 11 (64-bit).
- A CPU from Intel's 4th-generation Core (Haswell) or newer, or any AMD Ryzen: the game needs
  AVX, F16C, BMI1, MOVBE and LZCNT.
- A graphics card or integrated GPU with Vulkan 1.3 and a current driver.
- About 6 GB of free RAM for the game.

## Playing

**From a release:** extract the zip, open `TLOU Launcher.exe`, choose the game folder under Folders
and press Play. Nothing else needs installing. After that, a shortcut with the game's icon appears
beside the launcher. `TLOU Game.exe` starts the game directly with the last settings selected in
the launcher.

**From this repository** (needs Python 3.10 or newer):

1. Put the game folder `CUSA00552` next to this repository's folder, and the update next to it as
   `CUSA00552-patch` (or name another update folder with `TLOU_UPDATE=<folder>`). An update you
   only have as a PKG unpacks there with `python tools/install_update.py <update.pkg> <game folder>`.
2. Build the runtime (below), then start the game with `launcher/TLOU Launcher.exe`,
   `Start-TLOU.cmd`, or `python tools/probe_tlou.py --boot --seconds 0 [--game <game folder>]`.
3. Saves and settings stay in `analysis/tlou-user`; the log is `analysis/tlou/boot.log`. Each
   session also records your controller input (`analysis/tlou/pad-record-*.txt`, the newest 5),
   so a crash can be replayed on the same route; `TLOU_RECORD_PAD=0` turns that off.
4. With this repository on a hard disk, put the shader cache (`analysis/tlou-user/cache`, about
   250 MB in some 12,000 files) on an SSD: the game reads all of it in its first seconds, and
   from a hard disk that takes long enough for the game to build pipelines itself, pausing for
   seconds. With the game closed, move the folder, then link it back:
   `mklink /J analysis\tlou-user\cache <folder on the SSD>` (in `cmd`).

The launcher sets the display, frame limit and folders, in nine languages; its Debug menu switches
each optimization off, some of them while the game runs. The in-game menu (Insert, or R3+L2 on a
controller) holds the graphics, controls and frame rate settings.

**Without a DualShock 4:** the touchpad (backpack, crafting) is Tab / Backspace on the keyboard
or Back/View on an Xbox-style controller. Shaking the controller is G on the keyboard or the
Share/Capture button (`GOW3_KEY_MAP` / `GOW3_PAD_MAP` `shake=...` rebind it). The flashlight
never flickers out by default, so it needs no shake: Celogamez's GoldHEN cheat "Flashlight Does
Not Turn Off" (`TLOU_CHEATS=none` turns it off).

## Building

From an MSYS2 **CLANG64** shell:

```bash
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross}
git submodule update --init --recursive
gow3_pc/build.sh                 # the runtime: gow3_pc/out/gow3-probe.exe
```

Clone with `core.autocrlf=false`: line-ending conversion breaks the FSR-Vulkan patches.

`gow3_pc/build.sh` also builds `launcher/TLOU Launcher.exe`. With a profile in
`gow3_pc/pgo/tlou.profdata` it builds with PGO and LTO (about 10% more FPS in the heaviest area
tested); the profile is not in the repository, and without it the build is plain. `GOW3_PGO=off`
gives quicker development builds. A new profile needs a clean build.

`tools/package_windows.sh` builds `dist/TLOU-Remastered-windows.zip`, the release: the runtime and
its DLLs, the launcher, the scripts and a Python of its own. `tools/package_check.py` refuses game
files and paths of the build machine.

The launcher reads its background and icon from the player's own copy of the game. A local icon goes
in `launcher/assets/icon.ico` (not in the repository).

## Repository layout

| Path | Contents |
|---|---|
| `gow3_pc/` | The runtime: C runtime (`src/`), GPU library (`gpu/`: shadPS4 video core and shims), preparation scripts (`scripts/`). Diagnostics settings: [gow3_pc/README.md](gow3_pc/README.md). |
| `launcher/` | The launcher (`tlou_launcher.pyw`, Tkinter) and its starter (`tlou-launcher.c`). |
| `tools/package_windows.sh` | The Windows release zip. |
| `tools/probe_tlou.py` | Prepares the game's executable (with its update) and starts the runtime with this game's settings. |
| `tools/tlou_pad.py`, `tools/tlou_bench.py` | Scripted controller input, frame captures and analysis; a repeatable frame rate measurement on a test profile. |
| `tools/tlou_replay.py`, `tools/tlou_compare_shots.py` | A recorded route replayed on a copy of the player's saves, reporting a lost device, with frames saved at the same moments (and two runs' frames compared). |
| `tools/tlou_find_shader.py` | The game file a shader comes from, by the hash the logs name it by (or shaders by file name). |
| `tools/decode_dump.py`, other `tools/` | Image dump decoding and other diagnostics. |
| `tools/install_update.py` | Unpacks an update PKG next to the game folder (PkgTool, `tools/pkgtool`). |

## Credits and license

- The runtime comes from BilalReloaded's PS4 port runtime
  ([GitHub](https://github.com/bc160201983)), which started from FelipePrado0's
  [SpartanPort](https://github.com/FelipePrado0/SpartanPort) (gow3_pc; whose internal names, such as
  `GOW3_*` settings, it keeps), and the [shadPS4](https://github.com/shadps4-emu/shadPS4) video core.
- The camera motion for temporal anti-aliasing (`CameraMotion::OnVelocityConstants` and the motion
  vector mode) comes from SuperMedo's PS4 port ([GitHub](https://github.com/Supermedo), GPL v2).
- Windows red-zone protection and the reader-preferring memory table lock come from HayatoG's
  [tlou_pc](https://github.com/HayatoG/tlou_pc) (GPL-2.0-or-later); the protection follows
  shadPS4's design.
- The 1 ms timer, the power throttling opt-out, the faster guest condition variables, the idle job
  worker patch's method and the package's Python copy come from BilalReloaded's
  [Uncharted: The Nathan Drake Collection port](https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port).
- **NVIDIA DLSS:** DLAA uses NVIDIA DLSS technology through the DLSS bridge adapted from
  [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR)
  (MIT). NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation.
- AMD FSR 4: the v07 INT8 model as built by [Q2RTX](https://github.com/FireBurn/Q2RTX) from AMD's
  MIT-licensed FidelityFX SDK source.
- Third-party code: FSR (AMD), imgui, magic_enum, miniz, xbyak, LibAtrac9, FFmpeg, SDL3, PkgTool
  (LibOrbisPkg, LGPL-3.0).
- Licensed under the GNU GPL v2 or later (see `LICENSE`).
