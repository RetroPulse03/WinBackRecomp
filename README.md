# Author's Notes:
I decided to take on this for 2 reasons. #1.) WinBack is one of the greatest games on the N64, and an astonishingly unloved hidden gem that deserves some spotlight. I love this game. #2.) I've been doing a lot of 'hands-on' work with Claude Code in my other projects, and I thought it would be fun to see how it would work if I flipped the script, and let it mostly do this by itself with only minimal interjection from myself. 

If you love this game, and you don't like AI, then this isn't for you. However, I didn't see anyone else recomping this game, so please if you have the time and the talent, I ask that in all seriousness you make a better project. I'll step aside and take this down to make as much room as possible for a well thought out, human-first approach. 


# WinBack Recompiled

A native PC port of **WinBack: Covert Operations** (N64, USA), made by static recompilation. The game's MIPS code is translated to C and compiled for x64 using [N64Recomp](https://github.com/N64Recomp/N64Recomp), and [RT64](https://github.com/rt64/rt64) renders it. There is no emulator.

> **No game data is included.** You need your own copy of the game: *WinBack - Covert Operations (USA)*, big-endian `.z64`, SHA-1 `fa04477b321a95e1f7a2edcac5c09bfa6ef72041`.

## Status

**Early release (v0.1).** The game plays from boot, through the intros, title screen and Controller Pak save, into the first mission at 60 fps. Mission 1 has been play-tested by hand, including aiming with the laser sight. Later missions haven't been tested much yet.

If the game freezes or crashes, please [open an issue](https://github.com/RetroPulse03/WinBackRecomp/issues) and say where it happened. Please attach `logs/winback.log` from the game folder, which helps with diagnosis.

## Installing

Releases are packaged for [RecompLauncher](https://recomplauncher.com/) and [Quiver Launcher](https://quiverlauncher.com/), which install and update the game from this repository's releases. To install by hand:

1. Download `WinBackRecompiled-vX.Y.Z-Windows-x64.zip` from [Releases](https://github.com/RetroPulse03/WinBackRecomp/releases) and extract it.
2. Run `WinBackRecompiled.exe`. On first start it looks for your ROM next to the executable and otherwise asks you to select it. You can also pass `--rom <path>`. The ROM is verified and a copy is kept in the game folder, so this is only needed once.

Requirements: 64-bit Windows 10 or 11 and a GPU with Vulkan support.

## Playing

| Option | Effect |
|---|---|
| `--fullscreen` | Start in borderless fullscreen. F11 or Alt+Enter switches between windowed and full screen at any time. |
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
| Esc | Recomp menu |

**Controller** (PS2 Style, the default layout, as in the PlayStation 2 release; other layouts are in the recomp menu's **Controls**):

| Control | Action | N64 |
|---|---|---|
| Left stick | Move / aim | Analog stick |
| X | Action, take cover | A |
| A | Reload | B |
| B | Change weapon | C-Up |
| Y | Lock on | C-Down |
| LB | Crouch | Z |
| RB | Aim | R |
| LT / RT, right stick | Look left / right | C-Left / C-Right |
| Right stick click | Camera behind player | C-Left/C-Right twice |
| D-pad down | Place / detonate C4 | D-Down |
| Start | Pause | Start |
| Select (Back/View) | Recomp menu | - |

In menus and on the pause screen, A confirms and B goes back.

### Recomp menu

Press Select (or Esc) at any time to open the recomp menu over the game; press it again to close it. The game gets no input while the menu is open. Move with the D-pad, left stick or arrow keys, choose with A (Enter) and go back with B (Backspace).

**Level Select** lists stages 00 to 30 with their area. Choose a stage, then its **Stage start** or one of its checkpoints, and the game jumps there from wherever you are, including the title screen.

- A checkpoint becomes **Recorded** the first time you reach it in normal play. The game's own checkpoint data is saved to `saves/checkpoints/`, and the menu loads exactly that state from then on.
- Until then it is **Basic**: a fresh start of the stage with you placed at the checkpoint. Items, opened doors and other progress from earlier in the stage are missing, so some areas can behave differently than in normal play. Checkpoints reached after a Basic load aren't recorded.
- **Stage start** begins the stage as the game does, with its opening scene, full health and starting equipment.

**Display** holds the picture settings. Change a setting with left and right; it applies straight away and is saved to `settings.json` next to the executable.

- **Window mode:** Windowed, Borderless (a window covering the screen) or Fullscreen (exclusive full screen at your desktop resolution).
- **Resolution:** how sharply the 3D scene is drawn. Original is the N64's 240 lines (216 with Crop overscan), Double is twice that, and Match window (the default) draws at the window's own size.
- **Crop overscan:** WinBack leaves a black border around its picture, which TVs of the time hid past the edge of the screen. On cuts the border off so the picture fills more of the window.
- **Integer scale:** On enlarges the picture only in whole steps, so every game pixel is the same size. The rest of the window stays black.
- **Anti-aliasing:** off, or 2x (default), 4x or 8x MSAA.

**Controls** chooses the controller layout. Changes apply straight away and are saved to `settings.json`.

- **PS2 Style** (default): the layout above. In menus A confirms and B goes back.
- **N64 Style**: the N64 controller's buttons where they sit on the pad, the same in menus as in play: A is A, X is B, B is C-Down, Y is C-Left, LB is L, RB and RT are R, LT is Z, and the right stick presses the C buttons.
- **Custom**: choose what each controller button presses, whether the right stick turns the camera or presses the C buttons, and whether the game's menus confirm with A and go back with B. A new custom layout starts as PS2 Style, and it can be reset to either preset.

**Exit Game** closes the program after a confirmation. Progress since your last Controller Pak save is lost.

Players 2-4 use additional controllers.

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

`tools/regen.sh` generates all game-specific code from your ROM into `generated/`. Each build copies the game into `game/`: run `game\WinBackRecompiled.exe` to play, and your saves there survive rebuilds. `tools/package.ps1` packages release zips into `dist/`. [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) explains the pipeline, debugging options and runtime constraints. [WinBack Data](https://github.com/RetroPulse03/WinBackData) documents how the game works and where its data is.

## Project layout

| Path | Contents |
|---|---|
| `config/winback.us.yaml` | splat configuration |
| `config/symbol_addrs.txt` | Hand-maintained symbol names |
| `config/winback.us.toml` | N64Recomp configuration (stubs, replaced functions, hooks) |
| `config/aspMain.us.toml` | RSPRecomp configuration (audio microcode) |
| `src/game/` | Game registration, patches and Controller Pak |
| `src/main/` | Host application: window, renderer, audio, input |
| `src/ui/` | Recomp menu (RmlUi), its RT64 renderer and shaders |
| `assets/fonts/` | Menu font (Lato, SIL Open Font License), compiled into the executable |
| `patches/` | Patches applied to dependencies |
| `tools/` | Code generators and build helpers |
| `docs/` | Development notes and ROM map |
| `recomplauncher.json` | RecompLauncher file shipped in the release zip: ROM check, settings, setup step |

These folders are created locally and never committed: `game/` (the playable game, with its saves and data), `rom/` (your ROM), `generated/` (everything produced from the ROM, plus logs and screenshots), `deps/`, `build/` (compiler output) and `dist/` (release zips).

## Pinned versions

| Dependency | Version |
|---|---|
| N64Recomp | `ffb39cdad1da5de07eaaa48bd1db4a89a7986771` |
| N64ModernRuntime | `cdf5abbd5026fef5c364c676e4667c45e42b6863` |
| RT64 | `23cab603c4f9f4a8b369b38e036f1aa484603878` (plus `patches/rt64`) |
| splat64 | 0.50.1 |
| SDL2 (Windows) | 2.30.3 |
| RmlUi | `7a06f27db04fe5d13a5dacc19b2b4544673a4eca` (6.0) |
| FreeType (Windows binaries) | `d6fb49d11a9d0011bf4ecfe7e570beaaa189838a` |

## License

GPL-3.0, as required by N64ModernRuntime. See [LICENSE](LICENSE).

WinBack is a trademark of its respective owners. This project is not affiliated with or endorsed by them, and it contains no game assets.

## Credits

- [N64Recomp and N64ModernRuntime](https://github.com/N64Recomp): Wiseguy and contributors
- [RT64](https://github.com/rt64/rt64): Dario and contributors
- [splat](https://github.com/ethteck/splat)
- [n64sym](https://github.com/shygoo/n64sym) signatures: shygoo
- [SDL2](https://www.libsdl.org/)
- [RmlUi](https://github.com/mikke89/RmlUi) and [FreeType](https://freetype.org/) for the recomp menu
- [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp): the RmlUi renderer for RT64 that `src/ui/ui_renderer.cpp` is adapted from
- [Lato](https://www.latofonts.com/) font: Łukasz Dziedzic, under the SIL Open Font License (`assets/fonts/OFL.txt`)
