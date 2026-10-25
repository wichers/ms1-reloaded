"""Compose the "4011 as flashed" Ghidra XML: the IDA annotation XML with its memory
contents replaced by the BCI-decoded application image.

A BCI image carries no bootloader, so FLASH_3F from BOOTLOADER_START up (bootloader and
vector table) is taken from the IDA database's own bytes (the USBDM dump of a device).

The program is the address space the application sees. The application maps only page 3D
into the 0x8000-0xBFFF PPAGE window (its CRT init writes PPAGE = $3D once; nothing else in
the application writes PPAGE), so page 3D sits at plain 0x8000-0xBFFF, contiguous with the
fixed pages: IDA 0x3D9144 is 0x9144, and IDA's window-segment annotations land on the same
addresses. Page 3C, which only the bootloader's CRC scan maps, is the data-only overlay
FLASH_3C over the window.

Page 3D's last byte is the opcode of JSR $BBEE, whose operand is at 0xC000. IDA could not
decode it: it defined the byte as data and decoded 0xC000 as an instruction with a function
there; those three are dropped.

Usage: python3 -I compose_4011.py <ida_export dir> <4011 .bin> <out dir>
Writes <out dir>/ms1_4011.xml and <out dir>/ms1_4011.bytes.
"""
import os, re, sys

BOOTLOADER_START = 0xF800
PAGE_BOUNDARY = 0xC000  # page 3D ends, fixed page 3F begins
CROSS_TAIL_END = 0xC23E  # IDA code decoded from PAGE_BOUNDARY runs to here (isr_spi0_lcd)
STEM = "ms1_4011"


def bin_offset(addr):
    """Map an IDA address to its offset in the 64 KB BCI image (page order 3C, 3E, 3D, 3F)."""
    if 0x4000 <= addr < 0x8000 or 0xC000 <= addr < 0x10000:
        return addr
    if 0x3C8000 <= addr < 0x3CC000:
        return addr - 0x3C8000
    if 0x3D8000 <= addr < 0x3DC000:
        return 0x8000 + addr - 0x3D8000
    raise ValueError(f"address {addr:#x} is not in the BCI image")


def once(pattern, repl, text, what, flags=0):
    out, n = re.subn(pattern, repl, text, flags=flags)
    if n != 1:
        sys.exit(f"{what}: expected one match, found {n}")
    return out


ida_dir, bin_path, out_dir = sys.argv[1:4]
xml = open(os.path.join(ida_dir, "ms1_firmware.xml"), encoding="utf-8").read()
dump = open(os.path.join(ida_dir, "ms1_firmware.bytes"), "rb").read()
image = open(bin_path, "rb").read()
composed = bytearray(dump)

for m in re.finditer(r'<MEMORY_CONTENTS START_ADDR="0x([0-9A-Fa-f]+)" FILE_NAME="ms1_firmware\.bytes" '
                     r'FILE_OFFSET="0x([0-9A-Fa-f]+)" LENGTH="0x([0-9A-Fa-f]+)"', xml):
    start, off, length = (int(g, 16) for g in m.groups())
    replaced = differ = 0
    for i in range(length):
        addr = start + i
        if BOOTLOADER_START <= addr < 0x10000:
            continue
        composed[off + i] = image[bin_offset(addr)]
        replaced += 1
        differ += composed[off + i] != dump[off + i]
    print(f"{start:#08x}+{length:#06x}: {replaced} bytes from the image ({differ} differ from the dump), "
          f"{length - replaced} kept from the dump")

# Memory map: page 3D replaces the uninitialized window block, page 3C becomes an overlay.
xml = once(r'\n *<MEMORY_SECTION NAME="FLASH_PAGE" START_ADDR="0x8000" LENGTH="0x4000" />', "", xml,
           "IDA window section FLASH_PAGE")
xml = once(r'(<MEMORY_SECTION NAME="FLASH_3D" START_ADDR=)"0x3D8000"', r'\1"0x8000"', xml, "FLASH_3D section")
xml = once(r'(<MEMORY_CONTENTS START_ADDR=)"0x3D8000"', r'\1"0x8000"', xml, "FLASH_3D contents")


def split_code_block(m):
    """Drop the part of an IDA code block decoded from mid-instruction at PAGE_BOUNDARY."""
    start, end = int(m[2], 16), int(m[3], 16)
    if start != PAGE_BOUNDARY:
        return m[0]
    return f'{m[1]}<CODE_BLOCK START="0x{CROSS_TAIL_END:X}" END="0x{end:X}" />' if end >= CROSS_TAIL_END else ""


xml = re.sub(r'( *)<CODE_BLOCK START="0x([0-9A-F]{4})" END="0x([0-9A-F]{4})" />', split_code_block, xml)
xml, n_funcs = re.subn(rf'\n *<FUNCTION ENTRY_POINT="0x{PAGE_BOUNDARY:X}" [^>]*>.*?</FUNCTION>', "", xml, flags=re.DOTALL)
print(f"dropped {n_funcs} IDA function(s) starting mid-instruction at {PAGE_BOUNDARY:#06x}")

# IDA's segment-register defaults (ppage=0, ds=0 per segment) are placeholders, not observations;
# imported, they pin PPAGE to 0 at every function entry.
xml = re.sub(r"\n\s*<REGISTER_VALUES>.*?</REGISTER_VALUES>", "", xml, count=1, flags=re.DOTALL)
xml, n3d = re.subn(r'="0x3D([89AB][0-9A-F]{3})"', r'="0x\1"', xml, flags=re.IGNORECASE)
xml, n3c = re.subn(r'="0x3C([89AB][0-9A-F]{3})"', r'="FLASH_3C::\1"', xml, flags=re.IGNORECASE)
print(f"{n3d} page-3D addresses moved to 0x8000-0xBFFF, {n3c} page-3C addresses into the FLASH_3C overlay")

page_end_data = []


def drop_page_end_data(m):
    """Data IDA defined up to the last byte of page 3D, the opcode of the cross-page JSR."""
    addr, size = int(m[1], 16), int(m[2], 16)
    if addr < PAGE_BOUNDARY <= addr + size:
        page_end_data.append(f"{addr:#06x}")
        return ""
    return m[0]


xml = re.sub(r'\n *<DEFINED_DATA ADDRESS="0x([0-9A-F]{4})" [^>]*SIZE="0x([0-9A-F]+)"[^>]*/>', drop_page_end_data, xml)
print("dropped IDA data across the page-3D end:", page_end_data)
xml = xml.replace('FILE_NAME="ms1_firmware.bytes"', f'FILE_NAME="{STEM}.bytes"')
xml = re.sub(r'<PROGRAM NAME="[^"]*" EXE_PATH="[^"]*"',
             f'<PROGRAM NAME="{STEM}" EXE_PATH="{os.path.basename(bin_path)}"', xml, count=1)
os.makedirs(out_dir, exist_ok=True)
open(os.path.join(out_dir, f"{STEM}.bytes"), "wb").write(composed)
open(os.path.join(out_dir, f"{STEM}.xml"), "w", encoding="utf-8").write(xml)
print("wrote", os.path.join(out_dir, f"{STEM}.xml"))
