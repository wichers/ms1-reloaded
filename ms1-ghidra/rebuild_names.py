#!/usr/bin/env python3
"""Write the names of the firmware rebuild as "address<TAB>name" lines for ApplyNames.java:
its functions (tools/functions.tsv: original entry -> function), the other entries its source
files list with their original address (the "Functions:" lists of their headers, such as the
interrupt handlers: "name (0xADDR) - ...") and its RAM variables (src/globals.c, each at the
original's address).

Usage: python3 -I rebuild_names.py <ms1-firmware dir> <out.tsv>
"""
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
    src = os.path.join(root, "src")
    for d, _, files in sorted(os.walk(src)):
        for fn in sorted(files):
            if fn.endswith(".c"):
                for line in open(os.path.join(d, fn)):
                    m = re.match(r"\s\*\s+(\w+)\s+\((0x[0-9A-Fa-f]{4})\)", line)
                    if m:
                        names.setdefault(int(m.group(2), 16), m.group(1))
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
