#!/usr/bin/env python3
"""Write the names of the firmware rebuild as "address<TAB>name" lines for ApplyNames.java:
its functions (tools/functions.tsv: original entry -> function), the entries of its assembly
modules (asm/*.json) and its RAM variables (src/globals.c, each at the original's address).

Usage: python3 -I rebuild_names.py <ms1-firmware dir> <out.tsv>
"""
import json
import os
import re
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root, out = sys.argv[1:]
    names = {}
    for line in open(os.path.join(root, "tools", "functions.tsv")):
        if line.strip() and not line.startswith("#"):
            f = line.split()
            names[int(f[0], 16)] = f[1]
    asm = os.path.join(root, "asm")
    for fn in sorted(os.listdir(asm)):
        if fn.endswith(".json"):
            for a, n in json.load(open(os.path.join(asm, fn))).get("names", {}).items():
                names.setdefault(int(a, 16), n)
    for line in open(os.path.join(root, "src", "globals.c")):
        m = re.match(r"\s*RAM(?:_DATA)?\((0x[0-9A-Fa-f]+)\)\s+[^;=]*?(\w+)\s*(?:\[[^\]]*\])*\s*(?:=[^;]*)?;", line)
        if m:
            names.setdefault(int(m.group(1), 16), m.group(2))
    with open(out, "w") as f:
        f.write("# address\tname (the firmware rebuild's)\n")
        for a in sorted(names):
            f.write(f"{a:04x}\t{names[a]}\n")
    print(f"{out}: {len(names)} names")


main()
