#!/bin/sh
# Builds a mod from mods/<name> into the folder generated/mods/<name>, ready to
# copy into game/mods. Needs LLVM (clang, ld.lld) and regen.sh's symbol file.
# Usage: sh tools/build_mod.sh mods/test-mod
set -e
cd "$(dirname "$0")/.."

MOD="${1:?usage: build_mod.sh mods/<name>}"
NAME="$(basename "$MOD")"
OUT="$PWD/generated/mods/$NAME"
TOOL="$PWD/deps/N64Recomp/build/RecompModTool"
LLVM="${LLVM_BIN:-/c/Program Files/LLVM/bin}"
SYMS=generated/winback.us.syms.toml

[ -f "$SYMS" ] || { echo "$SYMS is missing; run tools/regen.sh first" >&2; exit 1; }

mkdir -p "$MOD/build"
rm -rf "$OUT"
mkdir -p "$OUT"

# Mod code is big-endian MIPS, which the mod tool and runtime then recompile.
CFLAGS="--target=mips -mips2 -mabi=32 -O2 -G0 -mno-abicalls -mno-odd-spreg -mno-check-zero-division
    -fno-pic -fomit-frame-pointer -ffreestanding -fno-builtin -nostdinc -fno-stack-protector
    -Wall -Wno-unused-function -Imods/include -I$MOD/include"
OBJS=""
for src in "$MOD"/src/*.c; do
    obj="$MOD/build/$(basename "$src" .c).o"
    "$LLVM/clang.exe" $CFLAGS -c "$src" -o "$obj"
    OBJS="$OBJS $obj"
done
"$LLVM/ld.lld.exe" --nostdlib --emit-relocs -T "$MOD/mod.ld" -o "$MOD/build/mod.elf" $OBJS

"$TOOL" "$MOD/mod.toml" "$OUT"

# The .nrm goes next to the folder; the folder holds the loose files.
mv "$OUT/$NAME.nrm" "$PWD/generated/mods/$NAME.nrm"
[ ! -d "$MOD/assets" ] || cp -r "$MOD"/assets/. "$OUT/"
echo "built $OUT and generated/mods/$NAME.nrm"
