"""The application versions this source builds, and where their layouts differ.

Version 4011 (releases 4011 and c011, the default) and version 4010 (releases 4010 and c010,
the hardware without track current measurement) are one source; MS1_RELEASE selects the
version's code (README: Release images). Their layouts differ in two places:

- RAM: 4010 has no variables at 4011's 0x10F2-0x10FC (track current, overload accumulators,
  shutdown counter), and every variable from 0x10FD up sits 0x0B lower, the GCC soft
  registers included. src/globals.c gives 4011's addresses; ram() maps them.
- Flash: the original's .rodata, which the build takes from the release image, starts at
  rodata_start, so the application must end below it.

The other per-version files are named here: the entry table for difftest (functions), the
assembly module descriptions (asm_dir), the build directory (as the Makefile's B) and its
release images; the linker script part with the table addresses is gcc/<version>/release.ld.

Used by tools/ramlayout.py, tools/asmgen.py (environment variable RELEASE), tools/mkimage.py
and tools/regress.py (--release).
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Release:
    def __init__(self, name, version, rodata_start, ram_gap, functions, asm_dir, build, images):
        self.name = name
        self.version = version            # application version word at 0xF73C
        self.rodata_start = rodata_start  # first byte of the original's .rodata
        self.ram_gap = ram_gap            # 4011 RAM addresses this version does not have
        self.functions = os.path.join(ROOT, functions)
        self.asm_dir = os.path.join(ROOT, asm_dir)
        self.build = os.path.join(ROOT, build)
        self.images = images              # its release images (original-firmware/<name>.bin)

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
    "4011": Release("4011", 0x4011, 0xE11B, None, "tools/functions.tsv", "asm", "build",
                    ["4011_1-8_mobile-station-2_A", "c011_1-8_mobile-station-2_A"]),
    "4010": Release("4010", 0x4010, 0xDFA1, (0x10F2, 0x10FD), "tools/functions_4010.tsv", "asm/4010",
                    "build/4010", ["4010_1-8_mobile-station_A", "c010_1-8_mobile-station_A"]),
}
DEFAULT = "4011"


def get(name=None):
    name = name or os.environ.get("RELEASE") or DEFAULT
    if name not in RELEASES:
        raise SystemExit(f"unknown release {name} (known: {', '.join(RELEASES)})")
    return RELEASES[name]
