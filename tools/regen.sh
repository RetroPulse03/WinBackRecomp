#!/bin/sh
# Regenerates everything derived from the ROM (rom/baserom.us.z64) into
# generated/: splat (twice: once to find functions, once with library names),
# N64Recomp symbols, recompiled C and the audio microcode.
set -e
cd "$(dirname "$0")/.."
PY=.venv/Scripts/python
[ -x "$PY" ] || PY=.venv/bin/python
SIGS=deps/n64sym/src/builtin_signatures.sig
G=generated
LOG=$G/logs
mkdir -p $LOG
: > $G/symbol_addrs.libultra.txt
rm -rf $G/asm && $PY -m splat split config/winback.us.yaml > $LOG/splat.log 2>&1
$PY -I tools/sigmatch.py rom/baserom.us.z64 $SIGS 1000 D9E60 80025C00 $G/asm 800E3C90 config/symbol_addrs.txt > $G/symbol_addrs.libultra.tmp
mv $G/symbol_addrs.libultra.tmp $G/symbol_addrs.libultra.txt
rm -rf $G/asm && $PY -m splat split config/winback.us.yaml > $LOG/splat.log 2>&1
$PY -I tools/gen_recomp_syms.py
rm -rf $G/RecompiledFuncs && mkdir $G/RecompiledFuncs
mkdir -p $G/rsp
deps/N64Recomp/build/N64Recomp config/winback.us.toml > $LOG/n64recomp.log 2>&1 || { grep -v "Tail call" $LOG/n64recomp.log | tail -5; exit 1; }
echo "recompiled: $(ls $G/RecompiledFuncs | wc -l) files"
deps/N64Recomp/build/RSPRecomp config/aspMain.us.toml > $LOG/rsprecomp.log 2>&1
