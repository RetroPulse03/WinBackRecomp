# WinBack Recompiled

A native PC port of **WinBack: Covert Operations** (N64, USA) made by static recompilation: the game's MIPS code is translated to C and compiled for x64, with [RT64](https://github.com/rt64/rt64) rendering through Direct3D 12 or Vulkan. No emulator.

> **No game data is included.** You need your own ROM: *WinBack - Covert Operations (USA)*, big-endian `.z64`, SHA-1 `fa04477b321a95e1f7a2edcac5c09bfa6ef72041`. Everything game-specific is generated from it on your machine.

## Status

**Boots and plays.** Verified on Windows 11 (Direct3D 12): intro cutscenes → title screen → Controller Pak save → first mission (Outer Ground) with the pause/map menu, at 60 fps with audio tasks running every frame. Only short scripted sessions have been tested so far.

Known issues:
- A grey square is drawn behind the "GO INTO MISSION!" banner (rendering bug, not investigated yet).
- Only the paths exercised so far are verified; more busy-wait loops may exist (`tools/find_spinloops.py`). The symptom is a freeze with one game thread spinning.

How it got here: all ~2,086 functions recompile with no hand edits. The game-specific fixes are three busy-wait loops (idle thread, graphics task counter, main loop state wait) patched to yield to the runtime, plus a file-backed Controller Pak replacing the runtime's "no pak" stub.

## Playing

1. Put your ROM next to `WinBackRecompiled.exe` (or run it once with `--rom <path>`). The ROM is checked and stored in `%APPDATA%\WinBackRecompiled`, so you only do this once.
2. Run `WinBackRecompiled.exe`. Options: `--fullscreen`, `--developer` (RT64 debug UI). Toggle fullscreen with F11 or Alt+Enter.

**Controls.** Keyboard (player 1): WASD = stick, F = A, R = B, Space = Z, Q = L, E = R, Y/T/G/H = C-up/left/down/right, arrows = D-pad, Enter = Start. Gamepad: left stick, A = A, X = B, B = C-down, Y = C-left, LB = L, RB/RT = R, LT = Z, right stick = C-buttons, Start. Players 2–4 use more gamepads.

Saves go to the emulated Controller Pak: `%APPDATA%\WinBackRecompiled\saves\controller_pak_1.bin`.

## Building (Windows)

Requirements: Visual Studio 2022+ Build Tools (C++), LLVM/Clang, CMake, Ninja, Python 3, Git.

```
python -m venv .venv
.venv\Scripts\python -m pip install "splat64[mips]" xxhash pyyaml
```

Run `sh tools/fetch_deps.sh` to clone the pinned dependencies into `deps/`, build `deps/N64Recomp` (commands in that script), put the ROM in the repo root as `baserom.us.z64`, then:

```
sh tools/regen.sh
tools\vsenv.cmd cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo
tools\vsenv.cmd cmake --build build
```

## How it's generated

`tools/regen.sh` rebuilds everything derived from the ROM:

1. **splat** splits the ROM using `winback.us.yaml` and disassembles the code segment.
2. **`tools/sigmatch.py`** identifies libultra, libc, n_audio and libmus functions by matching n64sym's signature data against the functions splat found. The runtime replaces those functions with native versions, so their names matter.
3. splat runs again with those names.
4. **`tools/gen_recomp_syms.py`** writes N64Recomp's symbol file. It also adds entry points where GCC's tail-merging jumps into the middle of another function.
5. **N64Recomp** writes `RecompiledFuncs/`, and **RSPRecomp** writes `rsp/aspMain.cpp` (audio microcode).

`gen_recomp_syms.py` also extends functions whose jump tables reach into a tail that splat split off.

`tools/find_spinloops.py` lists busy-wait loops that may need patching.

**Debugging:** `WINBACK_DEBUG=1` prints per-second task counts. Every 5 seconds it adds each game thread's recent calls (N64Recomp trace mode) and OSThread states, and it prints a trace on a crash. `WINBACK_WATCH=addr,...` adds memory words to that dump. `WINBACK_AUTOPRESS=start@6,a@10,...` scripts player 1's buttons. `tools/capture_test.ps1` runs the game and screenshots its window. See `docs/ROM_MAP.md` for the ROM layout.

## Layout

| Path | What |
|---|---|
| `winback.us.yaml` | splat config |
| `symbol_addrs.txt` | hand-maintained names; add names here as functions are identified |
| `winback.us.toml` | N64Recomp config (stubs, ignored functions, hooks) |
| `aspMain.us.toml` | RSPRecomp config (audio microcode) |
| `src/game/` | game registration and the file-backed Controller Pak |
| `src/main/` | host app: SDL window, RT64, audio, input |
| `tools/` | generators and build helpers |

## Pinned versions

| Dependency | Version |
|---|---|
| N64Recomp | `ffb39cdad1da5de07eaaa48bd1db4a89a7986771` |
| N64ModernRuntime | `cdf5abbd5026fef5c364c676e4667c45e42b6863` |
| RT64 | `23cab603c4f9f4a8b369b38e036f1aa484603878` |
| splat64 | 0.50.1 |
| SDL2 (Windows) | 2.30.3 |

## License

GPL-3.0 (N64ModernRuntime is GPL-3.0). WinBack is a trademark of its owners; this project is not affiliated with them.

Credits: [N64Recomp and N64ModernRuntime](https://github.com/N64Recomp) (Wiseguy and contributors), [RT64](https://github.com/rt64/rt64) (Dario and contributors), [splat](https://github.com/ethteck/splat), [n64sym](https://github.com/shygoo/n64sym) signatures (shygoo), SDL2.
