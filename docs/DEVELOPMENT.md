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
| `WINBACK_JUMP=5@20,12.1@60` | Level Select jumps at the given seconds: stage 5's start at 20 s, then stage 12's first checkpoint at 60 s. |
| `WINBACK_MENUPRESS=select@6,a@8,down@9` | Scripts recomp menu presses: `select up down left right a b`. |
| `WINBACK_RECORD_AT=34` | Records the game's current checkpoint snapshot at that second, as if a checkpoint had triggered. |
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

The Controller Pak is implemented in `src/game/controller_pak.cpp` and stored in `saves/` next to the executable. librecomp's `pak.cpp` is excluded from the build. Level Select's recorded checkpoints are in `saves/checkpoints/` (see below).

### Portable data

The runtime's config path is the executable's folder (`main.cpp`), and RT64's data path is `rt64/` inside it (`rt64_context.cpp`). The stored ROM copy, `saves/`, `mods/`, `mod_config/`, `mods.json`, `settings.json` and `rt64/` all live there. Nothing may be written to per-user system folders such as AppData or Documents. The release zip must not contain any of these files, because launcher updates unpack over the install folder.

## Rendering

Rendering bugs are fixed in the graphics emulation, not by patching the game. Check what the real microcode and RDP do, using the [F3DEX2 disassembly](https://github.com/Mr-Wiseguy/f3dex2) and [parallel-rdp](https://github.com/Themaister/parallel-rdp), and compare against a ParaLLEl-RDP capture.

RT64 is the renderer, and ParaLLEl-RDP is the reference for correct output. If RT64 changes keep accumulating, a developer build mode that runs the real F3DEX2 microcode through RSPRecomp and renders with ParaLLEl-RDP would allow side-by-side comparison.

Changes to dependencies live in `patches/<dep>/*.patch`. `tools/fetch_deps.sh` applies them on clone, and `deps/` must stay in sync with them. RT64's contribution policy doesn't accept AI-assisted changes, so these remain local patches.

| Patch | Fix |
|---|---|
| `rt64/0001-f3dex2-stateful-texrect.patch` | In F3DEX2, `G_TEXRECT` only stores its words and `G_RDPHALF_2` issues the rectangle. WinBack writes a 16-byte texrect before the sky cloud plane to load TMEM. RT64 drew it (a grey square) and consumed the following `G_MTX`, so no clouds were drawn. |
| `rt64/0002-l3dex2-line3d.patch` | The laser sight uses L3DEX2 2.08 `G_LINE3D`, which RT64 left unimplemented. The line is drawn as a screen-space quad built the same way as the microcode's RDP primitive (handler at IMEM `0x1618`). Its width is `(w0 & 0xFF) + 3` quarter-pixels on each side: horizontal for steep lines, vertical with square ends for shallow ones. Lines ignore culling, and L3DEX2 TRI1/TRI2 draw wireframes. |
| `rt64/0003-msaa-change-deadlock.patch` | Changing anti-aliasing or the resolution makes the workload thread wait until the present thread has shown the previous frame's interpolated frames, before it destroys their targets. With MSAA, frames can be left unshown (for example when two workloads share one present), and the present thread then waits on the workload thread: rendering stopped for good. The workload thread now also stops waiting once the present thread has moved on to the current workload's present. It waits without holding the configuration lock, which the present thread needs. |
| `rt64/0004-per-pixel-noise-seed.patch` | The laser sight uses a dithered alpha compare (`G_AC_DITHER`). RT64 seeded each pixel's noise with `instanceIndex * y * x`, which is 0 for every pixel of a workload's first draw call. The laser is the first draw of its own task, so the whole line passed or failed together and flickered. The seed now mixes each pixel's position with the instance index. |

RT64's frame interpolation is off (`RefreshRate::Original` in `display.cpp`). It treats each task as a frame, and WinBack draws each frame with two tasks (the scene, then the laser sight), so interpolated frames drew the laser inconsistently.

## Recomp menu

`src/ui/menu.cpp` is the overlay menu. Select (`SDL_CONTROLLER_BUTTON_BACK`) or Escape toggles it from the window thread, which turns input into menu actions and keeps the menu state. RT64's render hooks (`RT64::SetRenderHooks`, set before the renderer starts) create RmlUi on the renderer's thread and draw it into the swap chain after the game on every present. While the menu is open, `input.cpp` sends the game no input, and keeps doing so after it closes until every button is released. The game itself keeps running.

- `src/ui/ui_renderer.cpp` is the RmlUi render interface on RT64's RHI, adapted from Zelda64Recomp's. It uses RmlUi's compatibility interface with generated textures only. RmlUi's vertex colours are premultiplied but its font textures aren't, so the pixel shader premultiplies the texture and blending is `ONE / INV_SRC_ALPHA`.
- `src/ui/menu.rml` holds the layout and style. It is compiled into the executable with the Lato fonts (`file_to_c`), so the release needs no extra files. Sizes are in `dp`, where 1080 dp is the window height. The look imitates the game's pause screen. Reading the game's own plate textures and fonts from the ROM at run time is a later step.
- The visible rows are rebuilt as RML whenever the state changes. Only 10 rows are shown at a time.
- Entries: Level Select, Display, Return to Game and Exit Game. Exit Game asks for confirmation, then calls `ultramodern::quit()` as closing the window does. Display changes and quitting run on the window thread after the menu's lock is released.

### Display settings

`src/main/display.cpp` keeps the window mode, resolution, aspect ratio and anti-aliasing, saves them to `settings.json` and applies them live. The renderer options go through ultramodern's graphics config (`update_config` in `rt64_context.cpp`). The window mode is applied with SDL on the window thread: `SDL_WINDOW_FULLSCREEN_DESKTOP` for Borderless, and `SDL_WINDOW_FULLSCREEN` at the desktop display mode for Fullscreen. ultramodern's `wm_option` stays Windowed, because RT64's own full-screen mode is a borderless window that would fight SDL. `--fullscreen` and Big Picture mode start borderless without changing the saved setting.

Changing anti-aliasing during gameplay could stop rendering for good (RT64 deadlock, fixed by `rt64/0003-msaa-change-deadlock.patch`). A rendering stop seen once after a resolution change went through the same wait and was probably the same bug. To diagnose a stuck renderer, attach lldb (`lldb --batch -p <pid> -o "bt all"`) and look at the `RT64 Present` and workload threads.

## Level Select

`src/game/level_select.cpp` lists the stages, performs jumps and records checkpoints. Its hooks are in the Level Select section of `config/winback.us.toml`.

**Catalogue.** It is read from the ROM on first use. `D_8010D5F4[stage]` is a stage's first map, and its maps run up to the next stage's first map (64 maps). Entering map `m` loads event file `m + 1` from the archive at ROM `0x931470` (`u32 count`, then `count + 1` offsets from the archive start). An event file has `u16 count, condition offset, script offset` and then 8-byte events (`id, ?, condition, script`). Each script is a `u16` command count followed by commands: an opcode byte, then the arguments listed in `D_8010DF38[opcode]` (`flags, argc, u16 mask of 2-byte arguments`). The checkpoint opcode is the one whose `func_8009AA9C` jump table entry is `0x8009BADC`, and its first argument is the checkpoint id. The opcode at `0x8009BA34` loads another event file, which is scanned too. Each stage's checkpoints are its unique (map, id) pairs in script order. The area name is one of the game's four (`char*` table at `0x80111EF0`), chosen from the first map's area group in `D_8010D5A0`: 0-8 OUTER GROUND, 9-15 MAIN OFFICE, 16-19 FACTORY, 20 and above CONTROL CENTER. These ranges are inferred from how the groups run through the stages.

**Game state.** `D_80187FB8` is the main loop state: 0 runs the front end (`func_80073600` each frame), 1 gameplay (`func_8004BA40`). In the front end, `D_8010DBEA = 2` with a mode in `D_8010DBE9` (`func_8008F29C`) switches to gameplay after a 15-frame fade, setting `D_8016A41C = 0x0101` (normal play). The title intro and attract demo are gameplay with `D_8016A41C = 0x0808`, which skips loading event scripts, and `D_801390D4 = 1`. During gameplay, `D_8016A418 = 1` with a map in `D_8016A41A` makes the frame callback reload into that map, as doors and map-change scripts do. A checkpoint is the event block `D_80137990` (0x89C bytes; id at `+0x894`, map at `+0x895`) plus the progress block `D_80158760` (0x12E0 bytes; difficulty at `+0`, stage at `+1`, health at `+2`). `func_8009C8E0` copies both to `D_8011DAB0` / `D_8011E350`, and `func_8009C998` restores them. `func_8008EF28` resets the mission (clears the event block and the progress block's first 0xA0 bytes, the save record) and makes the next stage init take a start snapshot.

**Jumps** run in the frame callback hooks on the game thread.

- *Stage start:* `func_8008EF28`, keeping the difficulty and setting health to 100, then `func_8008EEAC(stage)`. From the front end the game then enters gameplay through `func_8008F29C(9)`, like New Game. During gameplay the map reload request is used instead.
- *Recorded checkpoint:* the snapshot is copied into `D_8011DAB0` / `D_8011E350`, then `func_8008EF28` + `func_8009C998` as on the continue screen, followed by the same transition.
- *Basic checkpoint:* a stage start. When the stage init takes its start snapshot (hook after `0x8004C068`), the snapshot's checkpoint id and map are replaced with the target's, and the next gameplay frame restores it like a recorded one.

**Recording.** A hook after the script checkpoint command's `func_8009C8E0` call (`0x8009BB00`) marks the checkpoint. The command still changes the saved copy afterwards, so the next frame writes `saves/checkpoints/stageSS_mapMM_cpIII.bin`: `WBCP`, then the two saved blocks in N64 byte order. Checkpoints aren't recorded during attract demos, or after a basic load until the next stage start or recorded load, because their state would be the basic one. Map entry scripts often trigger a checkpoint straight away, so this matters.

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

1. **Menu polish:** the game's own plate textures and lettering read from the ROM at run time, pausing the game while the menu is open, and mouse support. Level Select (Select opens it at any time) is done.
2. **Soak testing:** play through the game in short sessions using Level Select, patching new spin loops and fixing rendering bugs as they turn up. One crash has been reported while shooting the first enemy in mission 1 but hasn't been reproduced. Verify audio pitch and stereo (`audio.cpp` swaps the channels).
3. **Settings:** controls (Classic/Modern, remapping), the graphics API, audio, added to the same menu. Display (window mode, resolution, aspect ratio, anti-aliasing) is done.
4. **Continuous integration:** a GitHub Actions build on `windows-latest`. The ROM can't be used in CI, so this needs the approach other N64Recomp projects use for generated sources.
5. **Mods:** a RecompModTool template and SDK headers (mod game id `winback`), then widescreen, split-screen and custom characters.
6. **Linux build.**

The long-term goal for the menu is to move its options into rebuilt in-game menus that match the game's own style.
