# Author's Notes:
I decided to take on this for 2 reasons. #1.) WinBack is one of the greatest games on the N64, and an astonishingly unloved hidden gem that deserves some spotlight. I love this game. #2.) I've been doing a lot of 'hands-on' work with Claude Code in my other projects, and I thought it would be fun to see how it would work if I flipped the script, and let it mostly do this by itself with only minimal interjection from myself. 

If you love this game, and you don't like AI, then this isn't for you. However, I didn't see anyone else recomping this game, so please if you have the time and the talent, I ask that in all seriousness you make a better project. I'll step aside and take this down to make as much room as possible for a well thought out, human-first approach. 


# WinBack Recompiled

A native PC port of **WinBack: Covert Operations** (N64, USA), made by static recompilation. The game's MIPS code is translated to C and compiled for x64 using [N64Recomp](https://github.com/N64Recomp/N64Recomp), and [RT64](https://github.com/rt64/rt64) renders it. There is no emulator.

> **No game data is included.** You need your own copy of the game: *WinBack - Covert Operations (USA)*, big-endian `.z64`, SHA-1 `fa04477b321a95e1f7a2edcac5c09bfa6ef72041`.

## Status

**Early release (v0.1).** The game plays from boot, through the intros, title screen and Controller Pak save, into the first mission at 60 fps. Mission 1 has been play-tested by hand, including aiming with the laser sight. Later missions haven't been tested much yet.

If the game freezes or crashes, please [open an issue](https://github.com/RetroPulse03/WinBackRecomp/issues) and say where it happened. Running `WinBackRecompiled.exe --console` shows a log that helps with diagnosis.

## Installing

Releases are packaged for [RecompLauncher](https://recomplauncher.com/) and [Quiver Launcher](https://quiverlauncher.com/), which install and update the game from this repository's releases. To install by hand:

1. Download `WinBackRecompiled-vX.Y.Z-Windows-x64.zip` from [Releases](https://github.com/RetroPulse03/WinBackRecomp/releases) and extract it.
2. Run `WinBackRecompiled.exe`. On first start it looks for your ROM next to the executable and otherwise asks you to select it. You can also pass `--rom <path>`. The ROM is verified and a copy is kept in the game folder, so this is only needed once.

Requirements: 64-bit Windows 10 or 11 and a GPU with Vulkan support.

## Playing

| Option | Effect |
|---|---|
| `--fullscreen` | Start in fullscreen. F11 or Alt+Enter toggles it at any time. |
| `--console` | Show log output. |
| `--developer` | Show RT64's debug interface. |

**Keyboard (player 1)**

| Key | Button |
|---|---|
| W A S D | Analog stick |
| F / R | A / B |
| Space | Z |
| Q / E | L / R |
| Y / T / G / H | C-Up / C-Left / C-Down / C-Right |
| Arrow keys | D-Pad |
| Enter | Start |

**Controller:** left stick = analog stick, A = A, X = B, B = C-Down, Y = C-Left, LB = L, RB or RT = R, LT = Z, right stick = C buttons, Start = Start. Players 2-4 use additional controllers.

The game is fully portable: everything it writes (the ROM copy, saves, mods and renderer data) stays in its own folder, and nothing goes to AppData or Documents. Saves are on an emulated Controller Pak in `saves/`. To back up or move your game, copy the whole folder.

## Building from source

Requirements: Visual Studio 2022 or later Build Tools (C++), LLVM/Clang, CMake, Ninja, Python 3 and Git.

```
python -m venv .venv
.venv\Scripts\python -m pip install "splat64[mips]" xxhash pyyaml
sh tools/fetch_deps.sh
```

Build `deps/N64Recomp` using the commands in the header of `tools/fetch_deps.sh`. Then put the ROM in `rom/` as `baserom.us.z64` and run:

```
sh tools/regen.sh
tools\vsenv.cmd cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo
tools\vsenv.cmd cmake --build build
```

`tools/regen.sh` generates all game-specific code from your ROM into `generated/`. The game is built as `build/WinBackRecompiled.exe`, and `tools/package.ps1` packages it into `dist/`. [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) explains the pipeline, debugging options and runtime constraints. [docs/ROM_MAP.md](docs/ROM_MAP.md) describes the ROM layout.

## Project layout

| Path | Contents |
|---|---|
| `config/winback.us.yaml` | splat configuration |
| `config/symbol_addrs.txt` | Hand-maintained symbol names |
| `config/winback.us.toml` | N64Recomp configuration (stubs, replaced functions, hooks) |
| `config/aspMain.us.toml` | RSPRecomp configuration (audio microcode) |
| `src/game/` | Game registration, patches and Controller Pak |
| `src/main/` | Host application: window, renderer, audio, input |
| `patches/` | Patches applied to dependencies |
| `tools/` | Code generators and build helpers |
| `docs/` | Development notes and ROM map |
| `recomplauncher.json` | Launcher manifest shipped in the release zip |

These folders are created locally and never committed: `rom/` (your ROM), `generated/` (everything produced from the ROM, plus logs and screenshots), `deps/`, `build/` and `dist/` (release zips).

## Pinned versions

| Dependency | Version |
|---|---|
| N64Recomp | `ffb39cdad1da5de07eaaa48bd1db4a89a7986771` |
| N64ModernRuntime | `cdf5abbd5026fef5c364c676e4667c45e42b6863` |
| RT64 | `23cab603c4f9f4a8b369b38e036f1aa484603878` (plus `patches/rt64`) |
| splat64 | 0.50.1 |
| SDL2 (Windows) | 2.30.3 |

## License

GPL-3.0, as required by N64ModernRuntime. See [LICENSE](LICENSE).

WinBack is a trademark of its respective owners. This project is not affiliated with or endorsed by them, and it contains no game assets.

## Credits

- [N64Recomp and N64ModernRuntime](https://github.com/N64Recomp): Wiseguy and contributors
- [RT64](https://github.com/rt64/rt64): Dario and contributors
- [splat](https://github.com/ethteck/splat)
- [n64sym](https://github.com/shygoo/n64sym) signatures: shygoo
- [SDL2](https://www.libsdl.org/)
