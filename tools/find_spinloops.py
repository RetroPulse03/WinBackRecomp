#!/usr/bin/env python3
"""List likely busy-wait loops in splat's asm.

The runtime only switches game threads at OS calls (osRecvMesg etc.), so a
loop that polls memory another thread or an interrupt changes, without
calling anything, hangs forever. A candidate is a backward branch whose loop
body makes no calls, stores nothing, and loads from a global (%lo(sym)).

Usage: python tools/find_spinloops.py [asm_dir]
"""
import glob
import re
import sys

INSN = re.compile(r"\s*/\* ([0-9A-F]+) ([0-9A-F]{8}) [0-9A-F]{8} \*/\s+(\S+)\s*(.*)")
LABEL = re.compile(r"\s*\.L([0-9A-F]{8}):")


def main():
    asm_dir = sys.argv[1] if len(sys.argv) > 1 else "asm"
    for path in sorted(glob.glob(f"{asm_dir}/**/*.s", recursive=True)):
        func = None
        insns = []  # (vram, op, args)

        def scan():
            by_vram = {v: i for i, (v, _, _) in enumerate(insns)}
            for i, (vram, op, args) in enumerate(insns):
                m = re.search(r"\.L([0-9A-F]{8})", args)
                if not (op.startswith("b") and m):
                    continue
                target = int(m.group(1), 16)
                if target >= vram or target not in by_vram:
                    continue
                body = insns[by_vram[target]:i + 2]  # include delay slot
                ops = [o for _, o, _ in body]
                if any(o in ("jal", "jalr", "syscall") for o in ops):
                    continue
                if any(o.startswith("s") and o in ("sw", "sh", "sb", "sd", "swc1", "sdc1") for o in ops):
                    continue
                if len(body) > 24:
                    continue
                globals_read = [a for _, o, a in body if o.startswith("l") and "%lo(" in a]
                if globals_read:
                    syms = sorted(set(re.findall(r"%lo\((\w+)\)", " ".join(globals_read))))
                    print(f"{func}: loop {target:08X}-{vram:08X} polls {', '.join(syms)}")

        for line in open(path):
            if line.startswith("glabel "):
                if func:
                    scan()
                func = line.split()[1]
                insns = []
                continue
            m = INSN.match(line)
            if m and func:
                insns.append((int(m.group(2), 16), m.group(3), m.group(4)))
        if func:
            scan()


if __name__ == "__main__":
    main()
