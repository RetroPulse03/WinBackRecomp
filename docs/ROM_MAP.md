# WinBack (USA) ROM map

| Field | Value |
|---|---|
| File | `WinBack - Covert Operations (USA).z64`, big-endian, 16 MiB |
| SHA-1 | `fa04477b321a95e1f7a2edcac5c09bfa6ef72041` |
| XXH3-64 (runtime hash) | `0x1432B2EFE9B1573E` |
| Header name / ID | `WIN BACK` / `NWDE` |
| Entrypoint | `0x80025C00`: clears BSS `0x8011D3B0`–`0x801DB700`, sets `sp = 0x80187FB0`, jumps to `0x800E3C90` |
| Memory | 4 MiB (no Expansion Pak); framebuffers at `0x803B5000` |
| Compiler | GCC (uses `__divdi3`/`__udivdi3`/`__umoddi3`); GCC tail-merging produces jumps into the middle of other functions |
| Saves | Controller Pak (`osPfs*`); no EEPROM/SRAM/Flash code |
| Audio | libmus (Software Creations) on n_audio |

## Main segment (ROM `0x1000`–`0xF87B0`, vram `0x80025C00`)

The whole game is one statically linked segment. No code is loaded at run
time: no `jal`/`j` in the code targets anything outside it.

| ROM | Contents |
|---|---|
| `0x001000`–`0x0D9E60` | CPU code: game (`0x80025C00`–`0x800E3C90`), then libmus, n_audio/al, libultra, libc, GCC long-long helpers |
| `0x0D9E60`–`0x0D9F30` | rspboot |
| `0x0D9F30`–`0x0DAD50` | n_audio `aspMain` text (0xE20 bytes) → RSPRecomp |
| `0x0DAD50`– | F3DEX2 / L3DEX2 2.08 text → RT64 |
| `0x0EFF20` | n_audio `aspMain` data (command table at +0x10) |
| `0x0F0628` | F3DEX2 / L3DEX2 version strings (in ucode data) |
| BSS | `0x8011D3B0`, size `0xBE350` |

## Assets (ROM `0xF87B0`–`0xEF0000`)

Raw (low entropy) data up to about `0x278000`, then large high-entropy blocks
(compressed with a custom format; no Yay0/Yaz0/MIO0). An asset table of
`{rom_start, rom_end}` pairs sits in data at ROM `~0xDDED0`. ROM is padded
with zero from about `0xEF0000`.
