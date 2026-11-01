#!/usr/bin/env python3
"""Build N64Recomp's symbol file (generated/winback.us.syms.toml) from splat's asm.

WinBack is a single statically linked segment, so this writes one [[section]]
covering the CPU text (ROM 0x1000 up to rspboot) with every function splat
found, named from symbol_addrs*.txt where known.

Usage: python tools/gen_recomp_syms.py [asm_dir] [out_toml]
"""
import glob
import re
import struct
import sys

TEXT_ROM_START = 0x1000
TEXT_ROM_END = 0xD9E60  # rspboot starts here
TEXT_VRAM = 0x80025C00
ROM_PATH = "rom/baserom.us.z64"


def skipped_names():
    """Functions N64Recomp won't recompile: its built-in ignored/reimplemented
    libultra lists plus the project's own ignored list."""
    names = set()
    try:
        src = open("deps/N64Recomp/src/symbol_lists.cpp").read()
        names |= set(re.findall(r'"(\w+)"', src[:src.index("renamed_funcs {")]))
    except OSError:
        pass
    m = re.search(r"ignored\s*=\s*\[(.*?)\]", open("config/winback.us.toml").read(), re.S)
    if m:
        names |= set(re.findall(r'"(\w+)"', m.group(1)))
    return names


def jump_table_targets(rom, vram, size):
    """Targets of each `jr` through a jump table in a function, found from the
    usual sequence: sltiu bound; lui $at, %hi(tbl); addu; lw %lo(tbl); jr."""
    import rabbitizer

    insns = []
    for addr in range(vram, vram + size, 4):
        off = addr - TEXT_VRAM + TEXT_ROM_START
        insns.append(rabbitizer.Instruction(struct.unpack(">I", rom[off:off + 4])[0], addr))
    targets = []
    for i, ins in enumerate(insns):
        if not ins.isJrNotRa():
            continue
        hi = lo = bound = None
        for prev in reversed(insns[max(0, i - 16):i]):
            uid = prev.uniqueId
            if uid == rabbitizer.InstrId.cpu_lw and lo is None:
                lo = prev.getProcessedImmediate()
            elif uid == rabbitizer.InstrId.cpu_lui and hi is None and lo is not None:
                hi = prev.getProcessedImmediate()
            elif uid == rabbitizer.InstrId.cpu_sltiu and bound is None:
                bound = prev.getProcessedImmediate()
        if hi is None or lo is None or bound is None or not 0 < bound < 1024:
            continue
        table = ((hi << 16) + lo) & 0xFFFFFFFF
        toff = table - TEXT_VRAM + TEXT_ROM_START
        if not TEXT_ROM_START <= toff < len(rom) - bound * 4:
            continue
        targets += [struct.unpack(">I", rom[toff + j * 4:toff + j * 4 + 4])[0] for j in range(bound)]
    return targets


def extend_for_jump_tables(funcs, rom):
    """splat sometimes splits a function's shared tail (reached via `j`) into a
    function of its own. If that tail is also a jump table case, N64Recomp
    truncates the table and the switch fails at run time, so extend the
    function to cover every case target."""
    funcs = [list(f) for f in funcs]
    extended = 0
    for f in funcs:
        vram, _, size = f
        targets = [t for t in jump_table_targets(rom, vram, size) if t >= vram + size]
        while targets:
            last = max(targets)
            for v2, _, s2 in funcs:
                if v2 <= last < v2 + s2:
                    f[2] = v2 + s2 - vram
                    break
            else:
                break
            extended += 1
            targets = [t for t in jump_table_targets(rom, vram, f[2]) if t >= vram + f[2]]
    if extended:
        print(f"extended {extended} functions to cover their jump tables")
    return funcs


def add_branch_entries(funcs, rom):
    """GCC tail-merges code, so some functions jump into the middle of another.
    N64Recomp only accepts branches out of a function when they land on a
    function start (it emits a tail call), so add an overlapping entry point at
    each such target, running to the end of the function that contains it."""
    import rabbitizer

    funcs = [list(f) for f in funcs]
    starts = {f[0] for f in funcs}
    skip = skipped_names()
    while True:
        added = {}
        for vram, name, size in funcs:
            if name in skip:
                continue
            for addr in range(vram, vram + size, 4):
                off = addr - TEXT_VRAM + TEXT_ROM_START
                ins = rabbitizer.Instruction(struct.unpack(">I", rom[off:off + 4])[0], addr)
                if ins.isBranch():
                    target = ins.getBranchVramGeneric()
                elif ins.isJump() and not ins.isJrNotRa() and not ins.doesLink() \
                        and ins.uniqueId == rabbitizer.InstrId.cpu_j:
                    target = ins.getInstrIndexAsVram()
                else:
                    continue
                if vram <= target < vram + size or target in starts or target in added:
                    continue
                for v2, n2, s2 in funcs:
                    if v2 < target < v2 + s2 and n2 not in skip:
                        added[target] = v2 + s2 - target
                        break
        if not added:
            return sorted(funcs)
        for target, size in added.items():
            funcs.append([target, f"func_{target:08X}", size])
            starts.add(target)
        print(f"added {len(added)} overlapping entry points for mid-function branches")


def main():
    asm_dir = sys.argv[1] if len(sys.argv) > 1 else "generated/asm"
    out_path = sys.argv[2] if len(sys.argv) > 2 else "generated/winback.us.syms.toml"

    funcs = []  # (vram, name, size)
    for path in glob.glob(f"{asm_dir}/**/*.s", recursive=True):
        for line in open(path):
            m = re.match(r"nonmatching (\S+), 0x([0-9A-Fa-f]+)", line)
            if m:
                funcs.append([None, m.group(1), int(m.group(2), 16)])
                continue
            m = re.match(r"\s*/\* [0-9A-F]+ ([0-9A-F]{8}) [0-9A-F]{8} \*/", line)
            if m and funcs and funcs[-1][0] is None:
                funcs[-1][0] = int(m.group(1), 16)
    funcs = sorted(f for f in funcs if f[0] is not None
                   and TEXT_VRAM <= f[0] < TEXT_VRAM + TEXT_ROM_END - TEXT_ROM_START)
    rom = open(ROM_PATH, "rb").read()
    funcs = extend_for_jump_tables(funcs, rom)
    funcs = add_branch_entries(funcs, rom)

    out = ["# Generated by tools/gen_recomp_syms.py from the splat output; do not edit.\n\n",
           "[[section]]\n", 'name = "main"\n',
           f"rom = 0x{TEXT_ROM_START:X}\n", f"vram = 0x{TEXT_VRAM:08X}\n",
           f"size = 0x{TEXT_ROM_END - TEXT_ROM_START:X}\n\n", "functions = [\n"]
    seen = set()
    for vram, name, size in funcs:
        if name in seen:
            name = f"{name}_{vram:08X}"
        seen.add(name)
        out.append(f'    {{ name = "{name}", vram = 0x{vram:08X}, size = 0x{size:X} }},\n')
    out.append("]\n")
    open(out_path, "w").write("".join(out))
    print(f"{len(funcs)} functions -> {out_path}")


if __name__ == "__main__":
    main()
