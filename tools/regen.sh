#!/bin/sh
# Regenerates everything derived from the ROM: splat (twice: once to find
# functions, once with library names), N64Recomp symbols, recompiled C.
set -e
cd "$(dirname "$0")/.."
PY=.venv/Scripts/python
[ -x "$PY" ] || PY=.venv/bin/python
SIGS=deps/n64sym/src/builtin_signatures.sig
: > symbol_addrs.libultra.txt
rm -rf asm && $PY -m splat split winback.us.yaml > splat.log 2>&1
$PY -I tools/sigmatch.py baserom.us.z64 $SIGS 1000 D9E60 80025C00 asm 800E3C90 > symbol_addrs.libultra.tmp
mv symbol_addrs.libultra.tmp symbol_addrs.libultra.txt
rm -rf asm && $PY -m splat split winback.us.yaml > splat.log 2>&1
$PY -I tools/gen_recomp_syms.py
rm -rf RecompiledFuncs && mkdir RecompiledFuncs
mkdir -p rsp
deps/N64Recomp/build/N64Recomp winback.us.toml > n64recomp.log 2>&1 || { grep -v "Tail call" n64recomp.log | tail -5; exit 1; }
echo "recompiled: $(ls RecompiledFuncs | wc -l) files"
deps/N64Recomp/build/RSPRecomp aspMain.us.toml > rsprecomp.log 2>&1
