# WinBack Recompiled

Static recompilation of *WinBack: Covert Operations* (N64, USA) to a native PC executable. See [README.md](README.md) for building and playing, [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for the pipeline, debugging and runtime constraints, and [docs/ROM_MAP.md](docs/ROM_MAP.md) for the ROM layout.

@docs/DEVELOPMENT.md

## Contribution rules

- Never commit the ROM or anything derived from it.
- Fix rendering bugs in the graphics emulation, never by patching game code.
- Keep `deps/` in sync with `patches/`.
- Write files with UTF-8 encoding. Edit C string literals that contain escape sequences directly rather than through scripts.
