# WinBack Recompiled — notes for Claude

Static recompilation of *WinBack: Covert Operations* (N64, USA) to a native Windows exe: splat → `tools/sigmatch.py` → N64Recomp/RSPRecomp → N64ModernRuntime + RT64. README.md covers user-facing docs, build steps and pinned versions; `docs/ROM_MAP.md` covers the ROM layout. Read both before starting.

## Working with the user

- The user wants this driven **autonomously**: make the calls yourself and only stop when truly blocked (e.g. something needs their GitHub account).
- Goals, in priority order: (1) as automated as possible, (2) as native as possible, (3) mod-capable (split-screen, custom characters), (4) released through GitHub Releases so [N64RecompLauncher](https://github.com/SirDiabo/N64RecompLauncher) can install it.
- Never commit ROM-derived data. `.gitignore` already excludes the ROM, `asm/`, `RecompiledFuncs/`, `rsp/`, the generated syms/ld files, `deps/`, `build/`, logs and `captures/`.

## Current state (2026-11-01)

- Boots through the intros, title screen and Controller Pak save into the first mission at 60 fps. Only scripted runs of 45 seconds or less have been tested.
- Public repo: https://github.com/RetroPulse03/WinBackRecomp (branch `main`). `gh` is installed at `C:/Program Files/GitHub CLI/gh.exe` and logged in. Commit only as RetroPulse with the noreply email already set in .git/config.
- The working tree is ready to regenerate and build (`baserom.us.z64` is in the root, and the `deps/` and `build/` trees are already built).

## Everyday commands (from the repo root)

```
sh tools/regen.sh                                  # ROM -> asm, syms, RecompiledFuncs, rsp/aspMain.cpp
tools\vsenv.cmd cmake --build build                # incremental build (build/ is already configured)
$env:WINBACK_DEBUG=1; build\WinBackRecompiled.exe --rom baserom.us.z64
powershell -ExecutionPolicy Bypass -File tools\capture_test.ps1 -Seconds 40 -Shots 4
```

- Fresh machine: run `sh tools/fetch_deps.sh`, build N64Recomp (the commands are in that script's header), then follow README.md "Building".
- Scripted input for unattended runs: `WINBACK_AUTOPRESS=start@6,a@10,...`. `r@31+14` holds R (aim) for 14 s. The sequence used to reach the first mission (with the pause menu at the end) was `start@6,start@9,start@12,a@14,a@16,start@18,a@20,a@23,a@26,start@30,a@33,a@36`.
- `WINBACK_WATCH=8011xxxx,...` adds memory words to the debug dump that prints every 5 seconds.
- `WINBACK_DLDUMP=1` logs every fill/texture rect of 32x32 or more with its combiner, other modes and texture (stderr). Extend `dump_rects` in `src/main/rt64_context.cpp` for other render bugs.
- Test runs that `Stop-Process` the game look like crashes to the user; say so before launching one.
- Re-run `regen.sh` after changing `symbol_addrs.txt`, `winback.us.toml`, `winback.us.yaml` or the generators. Then rebuild; CMake globs RecompiledFuncs, so re-configure if the file count changes.

## How the runtime bites (read before debugging a hang)

- **Thread switches only happen at OS calls.** ultramodern switches game threads only inside libultra calls. VI/SP/DP messages are delivered only during message calls or `pause_self`/`yield_self`. Any loop that polls memory written by another thread therefore deadlocks.
  - Symptom: one thread's trace (WINBACK_DEBUG) repeats the same few functions forever while gfx/vi drop to 0.
  - Fix with a `[[patches.hook]]` that calls `yield_self_1ms(rdram)` at the loop head, or with `ignored` plus a `RECOMP_FUNC` in `src/game/patches.c`. Existing examples are in both files.
  - `tools/find_spinloops.py` lists candidates. It's noisy (53 hits), so only fix the ones you actually reach.
- **Library function names matter.** N64Recomp replaces libultra functions by name (`deps/N64Recomp/src/symbol_lists.cpp`). A missed or misnamed libultra function runs the original MIPS and touches hardware registers. Signatures that differ only by relocation can't tell some functions apart; name those by hand in `symbol_addrs.txt` (e.g. `osViGetNextFramebuffer`).
- **GCC tail-merging.**
  - `gen_recomp_syms.py` adds overlapping entry points at mid-function branch targets.
  - It also extends functions whose jump table cases fall in a tail splat split off. The symptom of a missed one is a "Switch-case out of bounds" abort.
  - A hook's `before_vram` must be inside a function that exists in the syms file, not at a label.
- **Audio microcode.** The audio ucode the game uses is ROM 0xDAD50, size 0xC60 (`aspMain.us.toml`). The one at 0xD9F30 is unused. Missing indirect jump targets go in `extra_indirect_branch_targets`.
- **Render bugs: fix the emulation, never the game.** Check what the real microcode/RDP does (F3DEX2 disassembly: github.com/Mr-Wiseguy/f3dex2; RDP: parallel-rdp) and compare against a ParaLLEl-RDP capture. Changes to dependencies live in `patches/<dep>/*.patch`, applied by `fetch_deps.sh` on clone; keep `deps/` in sync with them.
  - Renderer decision (2026-11-01): stay on RT64 (enhancements, widescreen/mod support, N64Recomp ecosystem) and treat ParaLLEl-RDP as ground truth. Option on the table if RT64 patches keep piling up: a developer-only build mode that runs the real F3DEX2 ucode through RSPRecomp and renders with ParaLLEl-RDP for side-by-side checks (or, as a last resort, as the renderer). RT64's CONTRIBUTING.md bans AI-generated contributions, so fixes stay as local patches; the user may file plain bug reports.
  - `patches/rt64/0001-f3dex2-stateful-texrect.patch`: in F3DEX2, `G_TEXRECT` only stores its words and `G_RDPHALF_2` sends the rectangle. WinBack writes a 16-byte RDP-form texrect before the sky cloud plane to load its TMEM; RT64 used to draw it (the grey square) and swallow the following `G_MTX` (no clouds).
  - `patches/rt64/0002-l3dex2-line3d.patch`: the laser sight is drawn with L3DEX2 2.08 (text ROM 0xDCD40, data 0xF08B0) `G_LINE3D`, which RT64 left as a stub. The line is drawn as a screen-space quad built the way the microcode builds its RDP primitive (handler at IMEM 0x1618). Width is `(w0 & 0xFF) + 3` quarter pixels on each side: horizontal for steep lines, vertical with square ends for shallow ones. Lines ignore culling. L3DEX2 TRI1/TRI2 draw wireframes.
- **AMD D3D12 driver crash.** On the user's AMD GPU (amdxc64.dll 32.0.32015.7029), the D3D12 backend crashes in the driver's shader compiler (thread "RT64 Shader") when the laser's pipeline is first compiled. Vulkan (`WINBACK_GFX_API=vulkan`) runs fine. lldb is at `C:\Program Files\LLVM\bin\lldb.exe` for backtraces.
- **Saves.** The Controller Pak is ours (`src/game/controller_pak.cpp`), stored in `%APPDATA%\WinBackRecompiled\saves\`. librecomp's `pak.cpp` is removed from the build in CMakeLists.txt.

## Toolchain quirks on this machine

- Build through `tools\vsenv.cmd`, which sets up vcvars64 and puts LLVM on PATH.
- The toolchain is clang-cl with Ninja. LLVM 23 needs `__PRFCHWINTRIN_H` (already set) and RT64 needs `-Wno-error`.
- The CMake on PATH (from WinLibs) fails GitHub TLS, so download anything with Windows `curl` instead.
- Python lives in `.venv` (splat64 0.50.1, xxhash, pyyaml). Run the tools with `-I`.
- Don't use heredoc/Python edits for C string literals containing `\n`; they got mangled several times. Use the Edit tool.
- Write files from Python with `encoding="utf-8"`. cp1252 once truncated README.md.
- If linking fails with "permission denied", an old `WinBackRecompiled.exe` is still running; kill it.
- `deps/ref-mkt` and `deps/ref-ob64` are other recomp projects, kept for API reference only. ref-mkt has no license, so don't copy code from it.

## Roadmap (do in order unless the user redirects)

1. **Soak tests.** Run longer autopress sessions through more missions, menus, game over and the cutscenes, and patch any new spin loops. A crash was reported while shooting the first enemy in mission 1 but not reproduced; get its message (run from a terminal). Check audio (pitch/stereo; `audio.cpp` swaps the channels).
2. **Release build settings.** Set `trace_mode = false` in `winback.us.toml` (it costs performance), use the WIN32 subsystem for Release (keep a console in debug builds), and add an icon and version info.
3. **Packaging.** Produce a zip with the exe, SDL2.dll, dxil.dll, dxcompiler.dll, README and license.
4. **GitHub.** This needs the user: they create the repo or install `gh`.
   - Add an Actions workflow that builds on windows-latest. The ROM can't go in CI, so CI needs the generated sources from somewhere else; look at how Zelda64Recomp and other N64Recomp projects handle this.
   - Publish tagged releases with the zip. Check N64RecompLauncher's expected release asset naming before the first tag.
5. **Mods.** Add a RecompModTool mod template and SDK headers (the mod game id is `winback`), a settings menu (RmlUi as in Zelda64Recomp, or minimal), then widescreen. After that come split-screen and custom characters.
6. **Linux build.** Optional, after the above.
