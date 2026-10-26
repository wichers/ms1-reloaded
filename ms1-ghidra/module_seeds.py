#!/usr/bin/env python3
"""Where to disassemble the firmware rebuild's assembly modules in a release image, for
DisassembleSeeds.java (./ghidra.sh listing): per module its range, its entries (the module
description's "names"), the code its jump tables lead to (each "tables" entry: table
address, number of words; words pointing into the table itself are unused indices) and any
other code ("code": code reached only through an address the module pushes, or not at all).

Writes "range LO HI" (HI exclusive) and "seed ADDR" lines, hex.

Usage: python3 -I module_seeds.py <release.bin> <module dir (ms1-firmware asm/...)> <out.txt>
"""
import json
import os
import sys


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    image_path, mod_dir, out = sys.argv[1:]
    image = open(image_path, "rb").read()    # page order $3C $3E $3D $3F: offset = CPU address from 0x4000
    if len(image) != 0x10000:
        sys.exit(f"{image_path}: not a 64 KiB application image")
    lines = []
    for fn in sorted(os.listdir(mod_dir)):
        if not fn.endswith(".json"):
            continue
        desc = json.load(open(os.path.join(mod_dir, fn)))
        ranges = [(int(a, 16), int(b, 16)) for a, b in desc["ranges"]]
        seeds = {int(a, 16) for a in list(desc.get("names", {})) + desc.get("code", [])}
        for a, n in desc.get("tables", []):
            t = int(a, 16)
            for p in range(t, t + 2 * n, 2):
                w = image[p] << 8 | image[p + 1]
                if any(lo <= w < hi for lo, hi in ranges) and not t <= w < t + 2 * n:
                    seeds.add(w)
        lines += [f"range {lo:04x} {hi:04x}" for lo, hi in ranges]
        lines += [f"seed {a:04x}" for a in sorted(seeds)]
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")


main()
