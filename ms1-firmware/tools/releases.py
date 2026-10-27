"""The application versions this source builds, and where their layouts differ.

Version 4011 (releases 4011 and c011, the default) and version 4010 (releases 4010 and c010,
the hardware without track current measurement) are one source; MS1_RELEASE selects the
version's code (README: Release images). Their layouts differ in two places:

- RAM: 4010 has no variables at 4011's 0x10F2-0x10FC (track current, overload accumulators,
  shutdown counter), and every variable from 0x10FD up sits 0x0B lower, the GCC soft
  registers included. src/globals.c gives 4011's addresses; ram() maps them.
- Flash: the original's function addresses differ too; each version's entry table for
  difftest (functions) holds them.

The other per-version files are named here: the build directory (as the Makefile's B) and its
release images.

Used by tools/ramlayout.py, tools/mkimage.py and tools/regress.py (--release).
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Release:
    def __init__(self, name, version, ram_gap, functions, build, originals):
        self.name = name
        self.version = version            # application version word at 0xF73C
        self.ram_gap = ram_gap            # 4011 RAM addresses this version does not have
        self.functions = os.path.join(ROOT, functions)
        self.build = os.path.join(ROOT, build)
        self.originals = originals        # the original's release images (original-firmware/<name>.bin)

    def ram(self, addr):
        """This version's address of the RAM byte at addr in 4011 (src/globals.c), or None."""
        if self.ram_gap is None or addr < self.ram_gap[0] or addr >= SOFT_REGISTERS_4011 + 9:
            return addr
        if addr < self.ram_gap[1]:
            return None
        return addr - (self.ram_gap[1] - self.ram_gap[0])

    @property
    def soft_registers(self):
        """_.tmp, the first GCC soft register: the end of the variables"""
        return self.ram(SOFT_REGISTERS_4011)


SOFT_REGISTERS_4011 = 0x1BB7

RELEASES = {
    "4011": Release("4011", 0x4011, None, "tools/functions.tsv", "build",
                    ["4011_1-8_mobile-station-2_A", "c011_1-8_mobile-station-2_A"]),
    "4010": Release("4010", 0x4010, (0x10F2, 0x10FD), "tools/functions_4010.tsv",
                    "build/4010", ["4010_1-8_mobile-station_A", "c010_1-8_mobile-station_A"]),
}
DEFAULT = "4011"


def get(name=None):
    name = name or os.environ.get("RELEASE") or DEFAULT
    if name not in RELEASES:
        raise SystemExit(f"unknown release {name} (known: {', '.join(RELEASES)})")
    return RELEASES[name]

