# Development Guide

Technical notes for working on WinBack Recompiled. [README.md](../README.md) covers building and playing. [ROM_MAP.md](ROM_MAP.md) describes the ROM layout.

## Pipeline

The ROM is turned into a native executable in these stages:

1. **splat** splits the ROM (`rom/baserom.us.z64`, configured by `config/winback.us.yaml`) and disassembles the code segment.
2. **`tools/sigmatch.py`** names libultra, libc, n_audio and libmus functions using n64sym signatures.
3. **`tools/gen_recomp_syms.py`** writes the N64Recomp symbol file.
4. **N64Recomp** writes `generated/RecompiledFuncs/` and **RSPRecomp** writes `generated/rsp/aspMain.cpp` (audio microcode).
5. The output is compiled with **N64ModernRuntime** and rendered by **RT64**.

Hand-written inputs live in `config/`. Everything derived from the ROM goes to `generated/` (asm, symbols, recompiled C, logs and test screenshots) and is never committed. `.gitignore` excludes `rom/`, `generated/`, `deps/`, `build/` and `dist/`. Paths in the config files are relative to the file itself.

## Common commands

Run these from the repository root.

```
sh tools/regen.sh                         # rom/ -> generated/ (asm, symbols, RecompiledFuncs, rsp)
tools\vsenv.cmd cmake --build build       # incremental build
game\WinBackRecompiled.exe --console      # play the latest build, with log output
powershell -ExecutionPolicy Bypass -File tools\capture_test.ps1 -Seconds 40 -Shots 4
powershell -ExecutionPolicy Bypass -File tools\package.ps1   # dist/WinBackRecompiled-v<version>-Windows-x64.zip
```

- Every build copies the exe, its `.pdb` and DLLs into `game/`, the folder to play and test from. Saves, the stored ROM, `rt64/` and mods live there and survive rebuilds. `tools/capture_test.ps1` runs `build\` instead so scripted runs don't touch those saves.
- Re-run `regen.sh` after changing anything in `config/` or the generators, then rebuild. Logs go to `generated/logs/`. CMake globs `generated/RecompiledFuncs/`, so re-configure if the number of files changes.
- `tools\vsenv.cmd` loads the Visual Studio environment and puts LLVM on `PATH`. The toolchain is clang-cl with Ninja.
- Python tools run from `.venv` (splat64 0.50.1, xxhash, pyyaml) with `python -I`.
- If linking fails with "permission denied", a previous `WinBackRecompiled.exe` is still running.

- The icon is drawn in `src/main/icon/icon.svg`. After changing it, run `sh tools/make_icon.sh` (ImageMagick 7) to rebuild `icon.ico` and the GitHub social preview `docs/images/social_preview.png`. Upload the preview by hand in the repository's Settings.

### Build options

| Option | Default | Effect |
|---|---|---|
| `WINBACK_CONSOLE` | `OFF` | Build with the console subsystem. Otherwise use `--console` or set `WINBACK_DEBUG` to get a console at runtime. |
| `WINBACK_VERSION_SUFFIX` | empty | Appended to the version (e.g. `-dev`). The version itself comes from `project(VERSION)` in `CMakeLists.txt`. |
| `trace_mode` in `config/winback.us.toml` | `false` | Records each thread's recent function calls for `WINBACK_DEBUG` dumps. It costs performance, so it stays off in releases. |

## Debugging

These environment variables are read at startup.

| Variable | Purpose |
|---|---|
| `WINBACK_DEBUG=1` | Prints per-second task counts and, every 5 seconds, OSThread states (plus call traces when `trace_mode` is on). Opens a console. |
| `WINBACK_WATCH=8011xxxx,...` | Adds memory words to the periodic dump. |
| `WINBACK_AUTOPRESS=start@6,a@10,...` | Scripts player 1's buttons. `r@31+14` holds R for 14 seconds. |
| `WINBACK_DLDUMP=1` | Logs every fill/texture rectangle of 32x32 or larger with its combiner, other modes and texture. |
| `WINBACK_GFX_API=d3d12` | Uses Direct3D 12 instead of Vulkan. |
| `RECOMPLAUNCHER_MODE=bigpicture` | Set by RecompLauncher's controller mode; starts in fullscreen. |

This sequence reaches the first mission and ends at the pause menu:

```
WINBACK_AUTOPRESS=start@6,start@9,start@12,a@14,a@16,start@18,a@20,a@23,a@26,start@30,a@33,a@36
```

## Runtime constraints

### Thread switches happen only at OS calls

ultramodern switches game threads only inside libultra calls. VI/SP/DP messages are delivered only during message calls or `pause_self`/`yield_self`. A loop that polls memory written by another thread therefore deadlocks.

- **Symptom:** one thread's trace repeats the same few functions while the gfx and vi counts drop to 0.
- **Fix:** add a `[[patches.hook]]` that calls `yield_self_1ms(rdram)` at the loop head, or list the function under `ignored` and reimplement it with `RECOMP_FUNC` in `src/game/patches.c`. Both files contain examples.
- `tools/find_spinloops.py` lists candidate loops. It reports many false positives, so patch only loops that are actually reached.

### Library function names

N64Recomp replaces libultra functions by name (`deps/N64Recomp/src/symbol_lists.cpp`). If a libultra function is missed or misnamed, the original MIPS code runs and touches hardware registers. Some functions differ only by relocations, so signatures can't tell them apart. Name those by hand in `config/symbol_addrs.txt` (for example `osViGetNextFramebuffer`).

### GCC tail merging

- `gen_recomp_syms.py` adds overlapping entry points at branch targets in the middle of functions.
- It also extends functions whose jump-table cases fall in a tail that splat split off. If one is missed, the build aborts with "Switch-case out of bounds".
- A hook's `before_vram` must be inside a function listed in the symbol file, not at a label.

### Audio microcode

The game uses the audio microcode at ROM `0xDAD50`, size `0xC60` (`config/aspMain.us.toml`). The copy at `0xD9F30` is unused. Missing indirect jump targets go in `extra_indirect_branch_targets`.

### Saves

The Controller Pak is implemented in `src/game/controller_pak.cpp` and stored in `saves/` next to the executable. librecomp's `pak.cpp` is excluded from the build.

### Portable data

The runtime's config path is the executable's folder (`main.cpp`), and RT64's data path is `rt64/` inside it (`rt64_context.cpp`). The stored ROM copy, `saves/`, `mods/`, `mod_config/`, `mods.json` and `rt64/` all live there. Nothing may be written to per-user system folders such as AppData or Documents. The release zip must not contain any of these files, because launcher updates unpack over the install folder.

## Rendering

Rendering bugs are fixed in the graphics emulation, not by patching the game. Check what the real microcode and RDP do, using the [F3DEX2 disassembly](https://github.com/Mr-Wiseguy/f3dex2) and [parallel-rdp](https://github.com/Themaister/parallel-rdp), and compare against a ParaLLEl-RDP capture.

RT64 is the renderer, and ParaLLEl-RDP is the reference for correct output. If RT64 changes keep accumulating, a developer build mode that runs the real F3DEX2 microcode through RSPRecomp and renders with ParaLLEl-RDP would allow side-by-side comparison.

Changes to dependencies live in `patches/<dep>/*.patch`. `tools/fetch_deps.sh` applies them on clone, and `deps/` must stay in sync with them. RT64's contribution policy doesn't accept AI-assisted changes, so these remain local patches.

| Patch | Fix |
|---|---|
| `rt64/0001-f3dex2-stateful-texrect.patch` | In F3DEX2, `G_TEXRECT` only stores its words and `G_RDPHALF_2` issues the rectangle. WinBack writes a 16-byte texrect before the sky cloud plane to load TMEM. RT64 drew it (a grey square) and consumed the following `G_MTX`, so no clouds were drawn. |
| `rt64/0002-l3dex2-line3d.patch` | The laser sight uses L3DEX2 2.08 `G_LINE3D`, which RT64 left unimplemented. The line is drawn as a screen-space quad built the same way as the microcode's RDP primitive (handler at IMEM `0x1618`). Its width is `(w0 & 0xFF) + 3` quarter-pixels on each side: horizontal for steep lines, vertical with square ends for shallow ones. Lines ignore culling, and L3DEX2 TRI1/TRI2 draw wireframes. |

### Graphics API

Vulkan is the default on every platform (`src/main/main.cpp`), which keeps behavior consistent across platforms. On some AMD drivers, Direct3D 12 crashes in the driver's shader compiler the first time the laser sight's pipeline is compiled. A graphics API option is planned for the settings menu.

## Releases

Versions follow [Semantic Versioning](https://semver.org/) and are tagged `vX.Y.Z`. To cut a release:

1. Bump `project(VERSION)` in `CMakeLists.txt`.
2. Build, then run `tools/package.ps1`.
3. Tag the commit and attach the zip to a GitHub release. Mark test builds as pre-releases.

Releases are installed by [RecompLauncher](https://recomplauncher.com/sdk) and [Quiver Launcher](https://quiverlauncher.com/), both of which read GitHub releases directly:

- The asset name carries the system and processor (`-Windows-x64.zip`, later `-Linux-x64.AppImage`). Launchers choose the file for the player's PC from those words.
- The executable sits at the top of the zip. Updates unpack over the install folder, so the release must not contain files the game writes at run time.
- `recomplauncher.json` (RecompLauncher SDK level 1) sits next to the executable. It names the program, the save folder (`{install}/saves`) and the message for exit code 2 (no ROM selected). RecompLauncher's game check only understands disc images and folders, so the executable finds the ROM itself and shows an Open dialog when there isn't one.
- Listing requires a submission on each site: [recompstore.com/submit](https://recompstore.com/submit?platform=pc) and [quiverlauncher.com/submit](https://quiverlauncher.com/submit).

## Roadmap

1. **Soak testing:** longer sessions through more missions, menus, game over and cutscenes, patching new spin loops. One crash has been reported while shooting the first enemy in mission 1 but hasn't been reproduced. Verify audio pitch and stereo (`audio.cpp` swaps the channels).
2. **Continuous integration:** a GitHub Actions build on `windows-latest`. The ROM can't be used in CI, so this needs the approach other N64Recomp projects use for generated sources.
3. **Mods:** a RecompModTool template and SDK headers (mod game id `winback`), a settings menu, then widescreen, split-screen and custom characters.
4. **Linux build.**
