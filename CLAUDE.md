# WinBack Recompiled

Static recompilation of *WinBack: Covert Operations* (N64, USA) to a native PC executable. See [README.md](README.md) for building and playing, [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for the pipeline, debugging and runtime constraints, and [WinBack Data](https://github.com/RetroPulse03/WinBackData) for how the game works and its ROM layout.

@docs/DEVELOPMENT.md

## Contribution rules

- Never commit the ROM or anything derived from it.
- Fix rendering bugs in the graphics emulation, never by patching game code.
- Keep `deps/` in sync with `patches/`.
- Never commit mods, including test mods. They are not part of the recomp. Only the interface in `mods/include/` belongs here.
- Record what is learned about the game in [WinBack Data](https://github.com/RetroPulse03/WinBackData), along with tools that read its data, as part of the work rather than afterwards. Mark what is inferred and hasn't been confirmed. `docs/DEVELOPMENT.md` keeps what is needed to work on the recomp itself.
- Write files with UTF-8 encoding. Edit C string literals that contain escape sequences directly rather than through scripts.
