"""Build the flashable image of the GNU build: the linked application plus what is taken
verbatim from an original image, with the application CRC made valid for the bootloader.

From the original (a banked S-record file, e.g. the device dump in this directory):
  page $3C (0x8000-0xBFFF)  the locomotive database and its accessors (CALL $BB08/$BB0C,$38)
  0xE11B-0xF73F             the original's .rodata (tables the firmware reads by address),
                            build-information strings, the .data image, the configuration
                            area (pointer table at 0xF720, pointer at 0xF73A, stored CRC at
                            0xF73C) and the CRC compensation word at 0xF73E
  0xF800-0xFFFF             the bootloader, its flash metadata and the vector table
The linked application (S-records from objcopy, CPU addresses) supplies 0x4000-0xE11A and the
interrupt trampolines at 0xF740-0xF7FF; the CPU address space maps to flash as 0x4000 page $3E,
0x8000 page $3D (the PPAGE window), 0xC000 page $3F.

The bootloader (bootloader_crc_check_and_dispatch) runs CRC-16/CCITT (0x1021, initial 0xFFFF,
data bits shifted in last) over page $3C 0x8000-0xBFFF then 0x4000-0xF7FF with page $3D, and
starts the application only if the result is 0. The word at 0xF73E is chosen so that it is:
the CRC is affine in those 16 bits, so they come from a 16x16 linear system over GF(2).

With an application image decoded from a firmware update file (BCI, see the workspace's
README; the four pages in file order $3C, $3E, $3D, $3F), page $3C and 0xE11B-0xF73F come
from it instead, and only the bootloader part from the original S-record file: that builds
the image of one firmware release (4011, c011 ...) rather than of one device.

With a further output name it also writes the 64 KiB image in the update file's page order,
0xF800-0xFFFF as in the release image (an update file carries no bootloader): the input of
tools/bci.py encode.

Usage: python3 tools/mkimage.py <app.s19> <original.s19> <out.s19> [<release.bin> [<out.bin>]]
"""
import sys

PAGE = 0x4000
COPY_FROM_ORIGINAL = [(0xE11B, 0xF740), (0xF800, 0x10000)]
APP_ALLOWED = [(0x4000, 0xE11B), (0xF740, 0xF800)]
COMP = 0xF73E


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


def main():
    if len(sys.argv) not in (4, 5, 6):
        sys.exit(__doc__)
    app_path, orig_path, out_path = sys.argv[1:4]
    app, orig = read_srec(app_path), read_srec(orig_path)
    if len(sys.argv) >= 5:
        release = read_bin(sys.argv[4])
        for a in range(0xE11B, 0xF740):
            key = cpu_to_flash(a)
            orig[key] = release[key]
        for off in range(PAGE):
            orig[(0x3C, off)] = release[(0x3C, off)]
    pages = {p: bytearray([0xFF] * PAGE) for p in (0x3C, 0x3D, 0x3E, 0x3F)}
    for (page, off), b in app.items():
        a = cpu_addr(page, off) if page != 0x3C else None
        if a is None or not any(lo <= a < hi for lo, hi in APP_ALLOWED):
            sys.exit(f"{app_path}: application byte at {page:02X}:{off:04X} outside "
                     f"0x4000-0xE11A and the trampolines")
        pages[page][off] = b
    for off in range(PAGE):
        if (0x3C, off) not in orig:
            sys.exit(f"{orig_path}: page $3C incomplete")
        pages[0x3C][off] = orig[(0x3C, off)]
    for lo, hi in COPY_FROM_ORIGINAL:
        for a in range(lo, hi):
            page, off = cpu_to_flash(a)
            if (page, off) not in orig:
                sys.exit(f"{orig_path}: no byte at {a:#06x}")
            pages[page][off] = orig[(page, off)]
    comp = solve_compensation(pages)
    page, off = cpu_to_flash(COMP)
    pages[page][off], pages[page][off + 1] = comp >> 8, comp & 0xFF
    used = sum(1 for (p, o) in app if p != 0x3C)
    print(f"{out_path}: {used} application bytes, CRC compensation 0x{comp:04X} at 0x{COMP:04X}")
    write_srec(out_path, pages)
    if len(sys.argv) == 6:
        image = bytearray()
        for page in (0x3C, 0x3E, 0x3D, 0x3F):
            image += pages[page]
        image[3 * PAGE + 0xF800 - 0xC000:] = bytes(release[(0x3F, off)] for off in range(0xF800 - 0xC000, PAGE))
        open(sys.argv[5], "wb").write(image)


main()
