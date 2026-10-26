"""Generate the linker script with the original firmware's RAM layout.

Variables defined with RAM(addr) / RAM_DATA(addr) (src/globals.c, at 4011's addresses) are in
sections named .bss.ram.<addr> / .data.ram.<addr>. This reads the section headers of the
objects (`objdump -h`, on stdin), places each such section at its address in the version
built (tools/releases.py: 4010 has its variables from 0x10FD up 0x0B lower) - .data
(0x1000-0x102F, initialised) or .bss (0x1030 up to the soft registers) - and writes the
linker script template with the placements filled in.
Overlapping or out-of-range variables, and variables the version does not have, are errors.

Usage: m68hc11-objdump -h <objects> | python3 tools/ramlayout.py [--release R] gcc/ms1.ld > build/ms1.ld
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # releases.py, also under python3 -I
import releases

DATA = (0x1000, 0x1030)     # the original's .data: copied from flash by the start-up code


def main():
    args = sys.argv[1:]
    rel = releases.get(args[1] if args[:1] == ["--release"] else None)
    if args[:1] == ["--release"]:
        args = args[2:]
    # the original's .bss, up to the GCC soft registers (_.tmp: 0x1BB7 in 4011)
    BSS = (0x1030, rel.soft_registers)
    template = open(args[0]).read()
    sections = {}
    current = None
    for line in sys.stdin:
        m = re.match(r"(\S+):\s+file format", line)
        if m:
            current = m.group(1)
            continue
        m = re.match(r"\s*\d+\s+(\.(?:bss|data)\.ram\.0x([0-9A-Fa-f]{4}))\s+([0-9a-f]+)", line)
        if m:
            name, addr, size = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
            if rel.ram(addr) is None or rel.ram(addr + size - 1) is None:
                sys.exit(f"ramlayout: {name} ({current}): version {rel.name} has no variable there")
            addr = rel.ram(addr)
            if name in sections:
                sys.exit(f"ramlayout: {name} defined in {sections[name][2]} and {current}")
            sections[name] = (addr, size, current)
    placed = {"DATA": [], "BSS": []}
    end = 0
    for name, (addr, size, obj) in sorted(sections.items(), key=lambda s: s[1][0]):
        if addr < end:
            sys.exit(f"ramlayout: {name} ({obj}) overlaps the variable before it (ends at {end:#06x})")
        end = addr + size
        for key, (lo, hi) in (("DATA", DATA), ("BSS", BSS)):
            if lo <= addr < hi:
                if name.startswith(".data.") != (key == "DATA"):
                    sys.exit(f"ramlayout: {name} ({obj}): RAM_DATA() is for 0x1000-0x102F, RAM() for the rest")
                if end > hi:
                    sys.exit(f"ramlayout: {name} ({obj}, {size} bytes) runs past {hi:#06x}")
                # inside an output section, '.' is an offset from the section's start
                placed[key].append(f"    . = {addr - lo:#06x}; KEEP (*({name}))")
                break
        else:
            sys.exit(f"ramlayout: {name} ({obj}) is outside the original's .data and .bss")
    placed["DATA"].append(f"    . = {DATA[1] - DATA[0]:#06x};")
    placed["BSS"].append(f"    . = {BSS[1] - BSS[0]:#06x};")
    for key in placed:
        marker = f"/* @RAM_{key}@ */"
        if marker not in template:
            sys.exit(f"ramlayout: {marker} missing from {args[0]}")
        template = template.replace(marker, "\n".join(placed[key]).lstrip())
    sys.stdout.write(template)


main()
