#!/usr/bin/env python3
"""Identify libultra/libc/audio functions in the ROM's main code segment.

Reads an n64sym-format signature file (plain text, see deps/n64sym's
signature-file-format.md) and matches every symbol against the ROM, the same
way n64sym does: CRC32 of the function bytes with relocated fields masked.
Names of functions and data referenced through relocations of matched
functions are recovered too.

With an asm directory (splat output), a signature is only accepted where it
starts exactly at a function splat found and covers that function (allowing
for trailing alignment padding). Without it every word offset is tried, which
gives many false positives on short signatures.

Usage: python tools/sigmatch.py <rom.z64> <signatures.sig> <text_rom_start>
       <text_rom_end> <vram_start> [asm_dir [lib_vram_start [manual_syms]]]
       > symbol_addrs.libultra.txt

lib_vram_start: function matches below this address (in game code) are
dropped; game code is full of small routines that collide with library
signatures.

manual_syms: a splat symbol file (symbol_addrs.txt); addresses named there
win over signature matches. Some functions differ only in a relocated
address (osViGetCurrentFramebuffer/osViGetNextFramebuffer), which the
signatures mask out.
"""
import glob
import re
import struct
import sys
import zlib
from collections import defaultdict

TARG26, HI16, LO16 = 4, 5, 6
RELTYPES = {".targ26": TARG26, ".hi16": HI16, ".lo16": LO16}


def load_sigs(path):
    syms = []
    cur = None
    for line in open(path):
        p = line.split()
        if not p or p[0].startswith("#"):
            continue
        if p[0] in RELTYPES:
            for o in p[2:]:
                cur["relocs"].append((int(o, 16), RELTYPES[p[0]], p[1]))
            continue
        cur = dict(name=p[0], size=int(p[1], 16), crcA=int(p[2], 16), crcB=int(p[3], 16), relocs=[])
        syms.append(cur)
    for s in syms:
        s["relocs"].sort()
    return syms


def stripped(buf, off, size, relocs):
    """Function bytes with relocated fields zeroed (n64sym's ReadStrippedWord)."""
    b = bytearray(buf[off:off + size])
    for ro, t, _ in relocs:
        if ro + 4 > size:
            continue
        if t == TARG26:
            b[ro] &= 0xFC
            b[ro + 1:ro + 4] = b"\0\0\0"
        else:
            b[ro + 2:ro + 4] = b"\0\0"
    return bytes(b)


def is_local(name):
    return "_text_" in name or "_rodata_" in name or "_data_" in name or "_bss_" in name


def load_funcs(asm_dir):
    """{rom offset: size} for every function in splat's asm output."""
    funcs = {}
    for path in glob.glob(f"{asm_dir}/**/*.s", recursive=True):
        size = None
        want = False
        for line in open(path):
            m = re.match(r"nonmatching \S+, 0x([0-9A-Fa-f]+)", line)
            if m:
                size = int(m.group(1), 16)
                continue
            if line.startswith("glabel "):
                want = True
                continue
            m = re.match(r"\s*/\* ([0-9A-F]+) [0-9A-F]{8} [0-9A-F]{8} \*/", line)
            if m and want:
                funcs[int(m.group(1), 16)] = size
                want = False
    return funcs


def main():
    rom = open(sys.argv[1], "rb").read()
    sigs = load_sigs(sys.argv[2])
    start, end, vram0 = (int(x, 16) for x in sys.argv[3:6])
    funcs = load_funcs(sys.argv[6]) if len(sys.argv) > 6 else None
    lib_start = int(sys.argv[7], 16) if len(sys.argv) > 7 else 0
    manual = set()
    if len(sys.argv) > 8:
        for line in open(sys.argv[8]):
            m = re.match(r"\s*\w+\s*=\s*0x([0-9A-Fa-f]+);", line)
            if m:
                manual.add(int(m.group(1), 16))

    # Group signatures by the reloc mask over their first 8 bytes so each ROM
    # offset needs one crcA per distinct mask.
    by_mask = defaultdict(lambda: defaultdict(list))
    for s in sigs:
        if s["size"] < 0x10:
            continue  # stubs like "jr $ra; nop" match unrelated game code
        mask = tuple((o, t) for o, t, _ in s["relocs"] if o < 8)
        by_mask[mask][s["crcA"]].append(s)

    matches = {}  # rom offset -> sig
    offsets = sorted(o for o in funcs if start <= o < end) if funcs else range(start, end, 4)
    for off in offsets:
        if off - start + vram0 < lib_start:
            continue
        fsize = funcs[off] if funcs else None
        for mask, table in by_mask.items():
            head = stripped(rom, off, 8, [(o, t, None) for o, t in mask])
            cands = table.get(zlib.crc32(head))
            if not cands:
                continue
            for s in cands:
                if off + s["size"] > end:
                    continue
                # splat may merge a function with its neighbour, so the
                # signature may be shorter than splat's size, never longer.
                if fsize is not None and s["size"] > fsize:
                    continue
                if zlib.crc32(stripped(rom, off, s["size"], s["relocs"])) == s["crcB"]:
                    prev = matches.get(off)
                    if prev is None or s["size"] > prev["size"]:
                        matches[off] = s

    names = {}  # vram -> (name, kind)
    for off, s in matches.items():
        names[off - start + vram0] = (s["name"], "func")

    # Recover referenced symbols from relocations.
    for off, s in matches.items():
        hi = {}
        for ro, t, rname in s["relocs"]:
            w = struct.unpack(">I", rom[off + ro:off + ro + 4])[0]
            if t == TARG26:
                addr = (vram0 & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if not is_local(rname):
                    names.setdefault(addr, (rname, "func"))
            elif t == HI16:
                hi[rname] = (w & 0xFFFF) << 16
            elif t == LO16 and rname in hi and not is_local(rname):
                lo = w & 0xFFFF
                addr = (hi[rname] + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
                names.setdefault(addr, (rname, "data"))

    # A name may be found at several addresses (static copies); keep all but
    # disambiguate.
    seen = defaultdict(int)
    for addr in manual:
        names.pop(addr, None)
    for addr in sorted(names):
        n, kind = names[addr]
        seen[n] += 1
    count = defaultdict(int)
    for addr in sorted(names):
        n, kind = names[addr]
        if seen[n] > 1:
            count[n] += 1
            if count[n] > 1:
                n = f"{n}_{addr:08X}"
        print(f"{n} = 0x{addr:08X};" + (" // type:func" if kind == "func" else ""))
    print(f"// {len(matches)} functions matched by signature, {len(names)} names total",
          file=sys.stderr)


if __name__ == "__main__":
    main()
