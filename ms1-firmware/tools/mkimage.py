"""Build the images of the GNU build: the linked application, page $3C and the release's
identification area, with the application CRC made valid for the bootloader.

The parts, as the Makefile builds them:
  <app.s19>     the application (S-records from objcopy, CPU addresses): 0x4000 up to the
                identification part, and the interrupt trampolines at 0xF740-0xF7FF
  <page3c.bin>  flash page $3C (0x8000-0xBFFF in the window, 16 KiB): the entry table and the
                code behind it, the device library's blocks erased
  <ident.s19>   the release's identification area 0xF700-0xF73F and its strings below it
                (src/sys/ident.c), with the application version word at 0xF73C, which must be
                the version's
The CPU address space maps to flash as 0x4000 page $3E, 0x8000 page $3D (the PPAGE window),
0xC000 page $3F.

The bootloader (bootloader_crc_check_and_dispatch) runs CRC-16/CCITT (0x1021, initial 0xFFFF,
data bits shifted in last) over page $3C 0x8000-0xBFFF then 0x4000-0xF7FF with page $3D, and
starts the application only if the result is 0. The word at 0xF73E is chosen so that it is:
the CRC is affine in those 16 bits, so they come from a 16x16 linear system over GF(2).

--bin writes the 64 KiB image in the update file's page order ($3C, $3E, $3D, $3F), with
0xF800-0xFFFF zero as in a release image (an update file carries no bootloader): the input of
tools/bci.py encode. --s19 writes the flashable banked S-record image, with 0xF800-0xFFFF (the
bootloader, its flash metadata and the vector table) from --bootloader, a device dump.

The bootloader starts the application only if the application version word at 0xF73C agrees
with its flash metadata word at 0xFF1A (bootloader_crc_check_and_dispatch: the low bytes
equal, the low nibbles of the high bytes equal, and bits 5-6 of the high byte where 0xF73C
has them set). A device's bootloader carries the word for its version, so the bootloader of
a 4011 device refuses 4010. When the check fails, the low byte at 0xFF1B becomes the
application's, which passes it for 4010 and c010 with a 4011 device's bootloader; the
S-record image says so on its line. A 4011 image keeps the bootloader as it is.

--version-from IMAGE.s19 takes the application version at 0xF704 (INFO AP:, CAN
identification) from IMAGE instead of <ident.s19>: tools/regress.py compares the rebuild with
the original that way, so that the version, a deliberate difference, does not show as one.

--original writes the image of an original release, for the emulator: everything below
0xF800 from <release.bin> (a decoded update file), the bootloader from <dump.s19> (with the
0xFF1B rule above).

Usage: python3 tools/mkimage.py [--release R] [--bin OUT.bin] [--bootloader DUMP.s19 --s19 OUT.s19]
                                [--version-from IMAGE.s19] <app.s19> <page3c.bin> <ident.s19>
       python3 tools/mkimage.py [--release R] --original <release.bin> <dump.s19> <out.s19>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # releases.py, also under python3 -I
import releases

PAGE = 0x4000
COMP = 0xF73E
VERSION = 0xF73C        # the application version word
AP_VERSION = 0xF704     # the application version, major.minor
BOOT_VERSION = 0xFF1A   # the bootloader's: the application version it starts


def read_srec(path):
    """{(page, offset): byte}. S1/S3 records are CPU addresses (S3 as written by objcopy for
    the flat link), S2 records PPAGE:addr."""
    mem = {}
    for line in open(path):
        line = line.strip()
        kind = line[:2]
        if kind not in ("S1", "S2", "S3"):
            continue
        alen = {"S1": 2, "S2": 3, "S3": 4}[kind]
        count = int(line[2:4], 16)
        raw = bytes.fromhex(line[4:4 + 2 * count])
        addr = int.from_bytes(raw[:alen], "big")
        for i, b in enumerate(raw[alen:-1]):
            a = addr + i
            if kind == "S2" and a > 0xFFFF:
                page, off = a >> 16, (a & 0xFFFF) - 0x8000
            else:
                if a > 0xFFFF:
                    sys.exit(f"{path}: address {a:#x} outside the CPU address space")
                page, off = cpu_to_flash(a)
            mem[(page, off)] = b
    return mem


def cpu_to_flash(a):
    if 0x4000 <= a < 0x8000:
        return 0x3E, a - 0x4000
    if 0x8000 <= a < 0xC000:
        return 0x3D, a - 0x8000
    if a >= 0xC000:
        return 0x3F, a - 0xC000
    sys.exit(f"address {a:#06x} is not flash")


def cpu_addr(page, off):
    return {0x3E: 0x4000, 0x3D: 0x8000, 0x3F: 0xC000}[page] + off


def crc_update(crc, byte):
    for _ in range(8):
        msb = crc & 0x8000
        crc = (crc << 1) & 0xFFFF
        if msb:
            crc ^= 0x1021
        if byte & 0x80:
            crc ^= 0x0001
        byte = (byte << 1) & 0xFF
    return crc


def app_crc(pages, comp):
    crc = 0xFFFF
    for b in pages[0x3C]:
        crc = crc_update(crc, b)
    for a in range(0x4000, 0xF800):
        if a in (COMP, COMP + 1):
            b = (comp >> 8) if a == COMP else (comp & 0xFF)
        else:
            page, off = cpu_to_flash(a)
            b = pages[page][off]
        crc = crc_update(crc, b)
    return crc


def solve_compensation(pages):
    """The 16-bit value at COMP that makes the CRC 0 (unique: the map is a bijection)."""
    c = app_crc(pages, 0)
    cols = [app_crc(pages, 1 << i) ^ c for i in range(16)]   # column i: effect of bit i
    # Gaussian elimination over GF(2): find x with sum(x_i * cols[i]) = c
    rows = []                                                 # (pivot mask, combination of x bits)
    basis = {}
    for i, col in enumerate(cols):
        comb = 1 << i
        for bit in sorted(basis, reverse=True):
            if col >> bit & 1:
                col ^= basis[bit][0]
                comb ^= basis[bit][1]
        if not col:
            sys.exit("CRC compensation: singular system")
        basis[col.bit_length() - 1] = (col, comb)
    target, x = c, 0
    for bit in sorted(basis, reverse=True):
        if target >> bit & 1:
            target ^= basis[bit][0]
            x ^= basis[bit][1]
    if target:
        sys.exit("CRC compensation: no solution")
    if app_crc(pages, x) != 0:
        sys.exit("CRC compensation: check failed")
    return x


def write_srec(path, pages):
    def rec(kind, addr, data, alen):
        body = addr.to_bytes(alen, "big") + data
        n = len(body) + 1
        return f"S{kind}{n:02X}{body.hex().upper()}{(~(n + sum(body))) & 0xFF:02X}\n"

    with open(path, "w") as out:
        for page in (0x3E, 0x3F, 0x3C, 0x3D):
            data = pages[page]
            for off in range(0, PAGE, 16):
                chunk = bytes(data[off:off + 16])
                if page in (0x3E, 0x3F):
                    out.write(rec(1, cpu_addr(page, off), chunk, 2))
                else:
                    out.write(rec(2, page << 16 | (0x8000 + off), chunk, 3))
        out.write("S9030000FC\n")


def read_bin(path):
    """{(page, offset): byte} of a decoded firmware update image"""
    data = open(path, "rb").read()
    if len(data) != 4 * PAGE:
        sys.exit(f"{path}: not a 64 KiB application image")
    return {(page, off): data[i * PAGE + off]
            for i, page in enumerate((0x3C, 0x3E, 0x3D, 0x3F)) for off in range(PAGE)}


def word(pages, a):
    page, off = cpu_to_flash(a)
    return pages[page][off] << 8 | pages[page][off + 1]


def bootloader_accepts(app, boot):
    """bootloader_crc_check_and_dispatch's check of the application version word"""
    x = app ^ boot
    return (x & 0xFF) == 0 and (x >> 8) & 0x0F == 0 and (x >> 8) & 0x60 & (app >> 8) == 0


def fit_bootloader(pages):
    """Let the bootloader start the application (module docstring); a note, or ''."""
    app, boot = word(pages, VERSION), word(pages, BOOT_VERSION)
    if bootloader_accepts(app, boot):
        return ""
    page, off = cpu_to_flash(BOOT_VERSION + 1)
    pages[page][off] = app & 0xFF
    if not bootloader_accepts(app, word(pages, BOOT_VERSION)):
        sys.exit(f"the bootloader's 0x{boot:04X} at 0x{BOOT_VERSION:04X} refuses application version 0x{app:04X}")
    return f", bootloader word 0x{BOOT_VERSION:04X} 0x{boot:04X} -> 0x{word(pages, BOOT_VERSION):04X}"


def original_image(rel, bin_path, boot_path, out_path):
    release = read_bin(bin_path)
    pages = {p: bytearray(release[(p, off)] for off in range(PAGE)) for p in (0x3C, 0x3D, 0x3E, 0x3F)}
    add_bootloader(pages, boot_path)
    if word(pages, VERSION) != rel.version:
        sys.exit(f"{bin_path}: application version 0x{word(pages, VERSION):04X}, not {rel.name}")
    if app_crc(pages, word(pages, COMP)) != 0:
        sys.exit(f"{bin_path}: application CRC not 0")
    note = fit_bootloader(pages)
    print(f"{out_path}: the original {rel.name}{note}")
    write_srec(out_path, pages)


def add_bootloader(pages, boot_path):
    boot = read_srec(boot_path)
    for a in range(0xF800, 0x10000):
        page, off = cpu_to_flash(a)
        if (page, off) not in boot:
            sys.exit(f"{boot_path}: no byte at {a:#06x}")
        pages[page][off] = boot[(page, off)]


def image(rel, app_path, page3c_path, ident_path, version_from=None):
    """The four pages with the parts placed, the CRC word set; 0xF800-0xFFFF 0xFF. With
    version_from, the version at AP_VERSION from that S-record image."""
    pages = {p: bytearray([0xFF] * PAGE) for p in (0x3C, 0x3D, 0x3E, 0x3F)}
    page3c = open(page3c_path, "rb").read()
    if len(page3c) != PAGE:
        sys.exit(f"{page3c_path}: not 16 KiB")
    pages[0x3C][:] = page3c
    ident = read_srec(ident_path)
    ident_start = min(cpu_addr(p, o) for p, o in ident)
    if any(page == 0x3C or not ident_start <= cpu_addr(page, off) < 0xF740 for page, off in ident):
        sys.exit(f"{ident_path}: not in 0x{ident_start:04X}-0xF73F")
    app_allowed = [(0x4000, ident_start), (0xF740, 0xF800)]
    app = read_srec(app_path)
    for (page, off), b in app.items():
        a = cpu_addr(page, off) if page != 0x3C else None
        if a is None or not any(lo <= a < hi for lo, hi in app_allowed):
            sys.exit(f"{app_path}: application byte at {page:02X}:{off:04X} outside "
                     f"0x4000-0x{ident_start - 1:04X} and the trampolines")
        pages[page][off] = b
    for (page, off), b in ident.items():
        pages[page][off] = b
    if version_from:
        other = read_srec(version_from)
        for a in (AP_VERSION, AP_VERSION + 1):
            page, off = cpu_to_flash(a)
            pages[page][off] = other[(page, off)]
    if word(pages, VERSION) != rel.version:
        sys.exit(f"{ident_path}: application version 0x{word(pages, VERSION):04X}, not {rel.name} (--release)")
    comp = solve_compensation(pages)
    page, off = cpu_to_flash(COMP)
    pages[page][off], pages[page][off + 1] = comp >> 8, comp & 0xFF
    used = sum(1 for (p, o) in app if p != 0x3C)
    return pages, f"{used} application bytes, CRC compensation 0x{comp:04X} at 0x{COMP:04X}"


def main():
    args = sys.argv[1:]
    opts = {}
    while args[:1] and args[0] in ("--release", "--bin", "--bootloader", "--s19", "--version-from") \
            and len(args) >= 2:
        opts[args[0]], args = args[1], args[2:]
    rel = releases.get(opts.get("--release"))
    if args[:1] == ["--original"] and len(args) == 4:
        original_image(rel, *args[1:])
        return
    if len(args) != 3 or ("--s19" in opts) != ("--bootloader" in opts) or not ({"--bin", "--s19"} & set(opts)):
        sys.exit(__doc__)
    pages, summary = image(rel, *args, version_from=opts.get("--version-from"))
    if "--bin" in opts:
        data = bytearray()
        for page in (0x3C, 0x3E, 0x3D, 0x3F):
            data += pages[page]
        data[3 * PAGE + 0xF800 - 0xC000:] = bytes(0x800)
        open(opts["--bin"], "wb").write(data)
        print(f"{opts['--bin']}: {summary}")
    if "--s19" in opts:
        add_bootloader(pages, opts["--bootloader"])
        note = fit_bootloader(pages)
        write_srec(opts["--s19"], pages)
        print(f"{opts['--s19']}: {summary}{note}")


main()
