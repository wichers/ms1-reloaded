"""Compose a complete flash image for the emulator from a BCI application image and a device
dump that supplies the bootloader.

A BCI-decoded image (../tools/bci.py) holds the four 16 KB flash pages in the order
3C, 3E, 3D, 3F but carries no bootloader: page 3F from 0xF800 (bootloader and vectors) is
taken from the dump, an S-record file in the banked layout USBDM writes (S1 records for the
fixed pages at 0x4000-0x7FFF and 0xC000-0xFFFF, S2 records PPAGE:addr for the windowed pages).

Writes the result in the same banked S-record layout, which the emulator loads.

Usage: python3 -I tools/compose_image.py <app.bin> <dump.s19> <out.s19>
"""
import sys

BOOTLOADER_START = 0xF800
PAGE = 0x4000
BIN_ORDER = (0x3C, 0x3E, 0x3D, 0x3F)  # page at each 16 KB offset of a BCI image


def read_s19(path):
    """{(page, offset): byte}; fixed 0x4000/0xC000 windows are pages 3E/3F."""
    mem = {}
    for line in open(path):
        line = line.strip()
        if line[:2] not in ("S1", "S2"):
            continue
        alen = 2 if line[1] == "1" else 3
        count = int(line[2:4], 16)
        raw = bytes.fromhex(line[4:4 + 2 * count])
        addr = int.from_bytes(raw[:alen], "big")
        for i, b in enumerate(raw[alen:-1]):
            a = addr + i
            if alen == 3:
                page, off = a >> 16, (a & 0xFFFF) - 0x8000
            elif 0x4000 <= a < 0x8000:
                page, off = 0x3E, a - 0x4000
            elif a >= 0xC000:
                page, off = 0x3F, a - 0xC000
            else:
                sys.exit(f"{path}: S1 address {a:#06x} outside the fixed flash pages")
            mem[(page, off)] = b
    return mem


def write_s19(path, pages):
    def rec(kind, addr, data, alen):
        body = addr.to_bytes(alen, "big") + data
        n = len(body) + 1
        csum = (~(n + sum(body))) & 0xFF
        return f"S{kind}{n:02X}{body.hex().upper()}{csum:02X}\n"

    with open(path, "w") as out:
        for page in (0x3E, 0x3F, 0x3C, 0x3D):
            data = pages[page]
            for off in range(0, PAGE, 16):
                if page == 0x3E:
                    out.write(rec(1, 0x4000 + off, data[off:off + 16], 2))
                elif page == 0x3F:
                    out.write(rec(1, 0xC000 + off, data[off:off + 16], 2))
                else:
                    out.write(rec(2, page << 16 | (0x8000 + off), data[off:off + 16], 3))
        out.write("S9030000FC\n")


def main():
    app_path, dump_path, out_path = sys.argv[1:4]
    app = open(app_path, "rb").read()
    if len(app) != 4 * PAGE:
        sys.exit(f"{app_path}: expected a 64 KB image, got {len(app)} bytes")
    dump = read_s19(dump_path)
    pages = {p: bytearray(app[i * PAGE:(i + 1) * PAGE]) for i, p in enumerate(BIN_ORDER)}
    boot = BOOTLOADER_START - 0xC000
    missing = [o for o in range(boot, PAGE) if (0x3F, o) not in dump]
    if missing:
        sys.exit(f"{dump_path}: no bootloader bytes at 0x{0xC000 + missing[0]:04X} and {len(missing) - 1} more")
    for o in range(boot, PAGE):
        pages[0x3F][o] = dump[(0x3F, o)]
    differ = sum(1 for (p, o), b in dump.items() if not (p == 0x3F and o >= boot) and pages[p][o] != b)
    print(f"{out_path}: bootloader 0x{BOOTLOADER_START:04X}-0xFFFF from the dump; "
          f"{differ} application bytes differ from the dump")
    write_s19(out_path, pages)


main()
