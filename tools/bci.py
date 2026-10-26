#!/usr/bin/env python3
"""
BCI firmware update files of the Märklin Mobile Station 1 (MS1): decode and encode
Based on reverse-engineering of the MC9S12D64 bootloader at 0xF877-0xFCDB,
plus the ECoS/MCS userspace updater `hc12admin` (sub_BE70 / sub_8904).

Decode: .bci -> 64 KB flash image (decode_bci_file).
Encode: 64 KB flash image + a loader -> .bci (encode_bci): the header the release files have,
from the image (bci_header), then the loader's blocks and the image's, each with parameters of
its own (block_params). Decoding the result gives the image back; on the four release
images, with their version's loader, the header is the release file's. Not tested on a device.
Loader: an update file's loader -> S-records at 0x1000 (extract_loader, write_srec).

The blocks, in file order, carry the records of the records table (below):
  Loader (8)       the first blocks (loader_block_count), dest_keys 0x00-0x06: code that the
                   bootloader decodes into RAM 0x1000-0x17FF and runs (bootloader 0xFC59-0xFCB6:
                   once all four dest_keys have arrived, the CRC-16 of stage 2 over
                   0x1000-0x17FF must come out 0; then JSR 0x1000 with the file offset of the
                   next block and the transfer handle on the stack). Not part of the image.
  Application (1)  the image: a 0x200-byte slice at dest_key * 0x100 per block, the pages in
                   the order $3C, $3E, $3D, $3F, without 0xF800-0xFFFF (the bootloader).

The release files' loader (ms1-firmware/third_party/loader/) takes the application's blocks as
the bootloader takes its own (stages below; dest_key * 0x100 must be below 0xF800), in any
order. It first erases the flash sector 0xF600-0xF7FF, which holds the application version
word, and the EEPROM's first sector, 0x0800-0x0803. For each block it erases the block's
sector and programs it, except the version word 0xF73C. Once every one of the 0x7C slots has
arrived, it programs the version word and restarts through the reset vector.

File header (hc12admin sub_BE70):
  Bytes 0..3:   Magic 'BCI\0' (BE 0x42434900). Hard-required.
  Bytes 4..7:   Header version, BE u32. Updater rejects > 0x00030000 (>3.0).
  Bytes 0x10-0x13: BE u32, file offset of the records table.
  Bytes 0x14-0x17: BE u32, "Header size" (informational).
  Bytes 0x18-0x1B: BE u32, file offset where the 0x226 firmware blocks start.
  Bytes 0x1C-0x1F: BE u32, total firmware size in bytes (0 in v0 files;
                   then take file_size - image_offset).
  Updater enforces 0x20 <= file_size <= 0x249F0.

Records table (16-byte header at table[0]):
  +0..3 BE: offset of first record (relative to table)
  +4..7 BE: record count
  +16..19 BE: offset of targets list (relative to table)
  +20..23 BE: target count

Per record:
  +0..3 BE: type (1=Application, 2=Bootloader, 3=Code, 8=Loader; others rejected).
  +4..7 BE: total record size in bytes (walk stride; 0 == skip).
  +8..11 BE: payload size (printed as "Size: %d").
  Type 1/2 metadata starts at +16; Type 3 at +64 (header<3.0) or +80 (>=3.0).
  Subrecord: +0..3 ProductInfo, +4..7 AppVersion, +8..11 compile time,
             +28..31 XMagicnumber.

Each target entry is 16 bytes: addr (BE u32) + 2x BE u16 + 8 HW-range bytes.

BCI block format (0x226 = 550 bytes per block):
  Byte 0:      Pre-descramble initial seed
  Byte 1:      Pre-descramble state (constant during pass)
  Byte 2:      Offset into descrambled payload buffer for header start
  Bytes 3-4:   Reserved (probably CAN transport)
  Bytes 5-545: Scrambled payload (541 bytes = 0x21D)
  Bytes 546-549: Transport-layer bytes, not decoded

Stage 1 (pre-descramble, bootloader 0xFB7E-0xFB9C):
  seed = block[0]; state = block[1]  (state stays constant)
  for i in 5..545:
      orig      = buf[i]                          # scrambled
      buf[i]    = orig XOR seed                   # descrambled
      temp      = (orig + state) & 0xFF
      carry     = bit7 of temp
      seed      = ((temp << 1) + 0x0D + carry) & 0xFF

Stage 2 (strided CRC reader, bootloader 0xFB04):
  Start pointer in descrambled buffer: pos = 5 + block[2]
  Stride = desc[pos] + 3  (constant for the block)
  Each read: fetch desc[pos], CRC it, advance pos by stride, wrap mod 0x21D
  541 total CRC bytes: 14 header/skip + 0x209 payload + 6 trailing.
  Final CRC-16 CCITT (poly 0x1021, seed 0xFFFF) must be 0.

Stage 3 (header parse, bootloader 0xFBB7-0xFC10):
  Call  1: header[0]  -> (re-read of stride seed; just CRC'd)
  Call  2: header[1]  -> dest_stride = header[1] + 7
  Call  3: magic1 (== 0x09)
  Call  4: magic2 (== 0x10)
  Call  5: dest_key  (must be even; dest_ram = 0x1000 + dest_key*0x100)
  Call  6: dest_check (must be 0)
  Call  7: scatter_pos (initial Y register)
  Call  8: skipped
  Call  9: xor_state
  Call 10: xor_key
  Call 11-14: 4 padding bytes

Stage 4 (XOR stream + scatter write, bootloader 0xFC12-0xFC49):
  for 0x209 iterations:
      scrambled  = read()
      plain      = scrambled XOR xor_state
      xor_state  = ((scrambled << 1) + xor_key + bit7(scrambled)) & 0xFF
      if scatter_pos < 0x200:
          payload[scatter_pos] = plain
      scatter_pos = (scatter_pos + dest_stride) mod 0x209

Stage 5 (trailing bytes):
  6 more bytes CRC'd. Final CRC must be 0.
"""

import os
import random
import sys


BLOCK_SIZE = 0x226          # 550 bytes per block (full transport unit)
PAYLOAD_LEN = 0x21D         # 541 bytes, CRC'd region in descrambled buffer
PAYLOAD_START = 5           # first descrambled byte (block-relative)
PAYLOAD_END = PAYLOAD_START + PAYLOAD_LEN  # 546
XOR_LOOP = 0x209            # 521 iterations of XOR stream cipher
FLASH_BYTES = 0x200         # 512 bytes of plaintext per block

# File-level constants from hc12admin sub_BE70 (0xBE70).
BCI_MAGIC = b"BCI\x00"
BCI_HEADER_MIN_SIZE = 0x20      # sub_BE70: file <= 0x1F bytes -> "didn't seem to have a header"
BCI_FILE_MAX_SIZE = 0x249F0     # sub_BE70: file > 0x249EF -> "Image too large"
BCI_HEADER_MAX_VERSION = 0x00030000  # sub_BE70: > 0x30000 -> "wrong header version"
BCI_HEADER_V3 = 0x00030000      # subrecord layout switches at this boundary

RECORD_TYPES = {
    1: "Application",
    2: "Bootloader",
    3: "Code",
    8: "Loader",
}
# hc12admin's main also has a dead "Bootloader + Application" label for any
# type !=1,!=2,!=3 -- but sub_BE70 refuses to store anything outside {1,2,3,8},
# so that branch never fires. Likely an older-format leftover.


def crc16_ccitt_update(crc: int, byte: int) -> int:
    """CRC-16 CCITT (poly 0x1021), matching bootloader 0xF8BF."""
    for _ in range(8):
        msb = (crc >> 15) & 1
        crc = (crc << 1) & 0xFFFF
        if (byte >> 7) & 1:
            crc ^= 0x0001
        byte = (byte << 1) & 0xFF
        if msb:
            crc ^= 0x1021
    return crc


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc = crc16_ccitt_update(crc, b)
    return crc


def pre_descramble(block: bytes) -> bytearray:
    """Stage 1: descramble bytes [5, 546) using byte-stream cipher with
    seed = block[0] and constant state = block[1]."""
    buf = bytearray(block)
    seed = buf[0]
    state = buf[1]
    for i in range(PAYLOAD_START, PAYLOAD_END):
        orig = buf[i]
        buf[i] = orig ^ seed
        temp = (orig + state) & 0xFF
        carry = (temp >> 7) & 1
        temp = (temp << 1) & 0xFF
        seed = (temp + 0x0D + carry) & 0xFF
    return buf


class StridedReader:
    """Walks the descrambled payload buffer with a fixed stride,
    updating CRC-16 CCITT. Mirrors bootloader_flash_crc_byte (0xFB04)."""

    def __init__(self, buf: bytearray, start_pos: int, stride: int):
        self.buf = buf
        self.pos = start_pos
        self.stride = stride
        self.crc = 0xFFFF

    def read(self) -> int:
        b = self.buf[self.pos]
        self.crc = crc16_ccitt_update(self.crc, b)
        self.pos += self.stride
        # Absolute wrap: if abs_pos >= 0x1A32, abs_pos -= 0x21D.
        # Buffer-relative: wrap when pos >= PAYLOAD_END (546).
        if self.pos >= PAYLOAD_END:
            self.pos -= PAYLOAD_LEN
        return b


def decode_bci_block(block: bytes) -> dict:
    """Decode a single 0x226-byte BCI block."""
    if len(block) < BLOCK_SIZE:
        return {"valid": False, "error": f"block too short: {len(block)}"}

    # Stage 1
    desc = pre_descramble(block)

    # Stage 2: strided CRC reader
    header_offset = desc[2]
    start_pos = PAYLOAD_START + header_offset
    if start_pos >= PAYLOAD_END:
        start_pos -= PAYLOAD_LEN
    stride = (desc[start_pos] + 3) & 0xFFFF
    reader = StridedReader(desc, start_pos, stride)

    # Header reads
    reader.read()                       # header[0] (same byte that seeded stride)
    hb1 = reader.read()                 # header[1] -> dest_stride
    dest_stride = (hb1 + 7) & 0xFFFF

    magic1 = reader.read()
    if magic1 != 0x09:
        return {"valid": False, "error": f"bad magic1: {magic1:#x}"}
    magic2 = reader.read()
    if magic2 != 0x10:
        return {"valid": False, "error": f"bad magic2: {magic2:#x}"}

    dest_key = reader.read()
    dest_check = reader.read()
    if dest_check != 0:
        return {"valid": False, "error": f"bad dest_check: {dest_check:#x}"}
    if dest_key & 1:
        return {"valid": False, "error": f"odd dest_key: {dest_key:#x}"}

    scatter_pos = reader.read()         # initial Y (position index)
    reader.read()                       # skipped
    xor_state = reader.read()
    xor_key = reader.read()
    for _ in range(4):                  # 4 padding bytes
        reader.read()

    # Stage 4: XOR stream cipher + scatter write, 0x209 iterations
    payload = bytearray(FLASH_BYTES)
    for _ in range(XOR_LOOP):
        scrambled = reader.read()
        plain = scrambled ^ xor_state
        # state update uses the SCRAMBLED byte (re-derived via plain XOR state)
        carry = (scrambled >> 7) & 1
        xor_state = (((scrambled << 1) & 0xFF) + xor_key + carry) & 0xFF

        if scatter_pos < FLASH_BYTES:
            payload[scatter_pos] = plain
        scatter_pos += dest_stride
        if scatter_pos >= XOR_LOOP:
            scatter_pos -= XOR_LOOP

    # Stage 5: 6 trailing CRC bytes
    for _ in range(6):
        reader.read()

    crc_valid = reader.crc == 0

    return {
        "valid": True,
        "crc_valid": crc_valid,
        "crc_state": reader.crc,
        "dest_key": dest_key,
        "dest_addr": 0x1000 + (dest_key << 8),
        "stride": stride,
        "dest_stride": dest_stride,
        "xor_key": xor_key,
        "payload": bytes(payload),
    }


def _be32(buf: bytes, off: int) -> int:
    return int.from_bytes(buf[off:off + 4], "big")


def _be16(buf: bytes, off: int) -> int:
    return int.from_bytes(buf[off:off + 2], "big")


def parse_bci_records(data: bytes, table_off: int) -> dict:
    """Walk the records table at file offset `table_off`, mirroring sub_BE70.
    Returns dict with keys 'records', 'targets', plus 'app'/'bootloader'/'code'/'loader'
    pointing at the first record of each type (None if absent)."""
    if table_off == 0 or table_off + 24 > len(data):
        return {"records": [], "targets": [], "app": None, "bootloader": None,
                "code": None, "loader": None, "first_rec_off": 0, "rec_count": 0,
                "tgt_list_off": 0, "tgt_count": 0}

    first_off = _be32(data, table_off)              # table[+0..3] BE
    rec_count = _be32(data, table_off + 4)          # table[+4..7] BE
    tgt_off = _be32(data, table_off + 16)           # table[+16..19] BE
    tgt_count = _be32(data, table_off + 20)         # table[+20..23] BE

    records: list[dict] = []
    pos = table_off + first_off
    by_type: dict[int, dict] = {}
    for _ in range(rec_count):
        if pos + 12 > len(data):
            break
        rtype = _be32(data, pos)
        rsize = _be32(data, pos + 4)
        psize = _be32(data, pos + 8)
        rec = {"offset": pos, "type": rtype, "rec_size": rsize,
               "payload_size": psize, "name": RECORD_TYPES.get(rtype, f"unknown({rtype})")}
        records.append(rec)
        by_type.setdefault(rtype, rec)
        if rsize == 0:
            break
        pos += rsize

    targets: list[dict] = []
    if tgt_off and tgt_count:
        tpos = table_off + tgt_off
        for _ in range(tgt_count):
            if tpos + 16 > len(data):
                break
            targets.append({
                "offset": tpos,
                "addr": _be32(data, tpos),
                "key1": _be16(data, tpos + 4),
                "key2": _be16(data, tpos + 6),
                "hw_range": tuple(data[tpos + 8:tpos + 16]),
            })
            tpos += 16

    return {
        "records": records,
        "targets": targets,
        "first_rec_off": first_off,
        "rec_count": rec_count,
        "tgt_list_off": tgt_off,
        "tgt_count": tgt_count,
        "app": by_type.get(1),
        "bootloader": by_type.get(2),
        "code": by_type.get(3),
        "loader": by_type.get(8),
    }


def parse_bci_header(data: bytes) -> dict:
    """Parse the BCI file header per hc12admin sub_BE70. Raises ValueError on
    hard failures (magic, size limits, version cap) -- same checks as the updater."""
    n = len(data)
    if n < BCI_HEADER_MIN_SIZE:
        raise ValueError(f"file too short ({n} bytes); header requires >= {BCI_HEADER_MIN_SIZE}")
    if n > BCI_FILE_MAX_SIZE:
        raise ValueError(f"file too large ({n} bytes); updater rejects > {BCI_FILE_MAX_SIZE:#x}")
    if data[:4] != BCI_MAGIC:
        raise ValueError(f"bad magic {data[:4]!r}, expected {BCI_MAGIC!r}")

    hdr_version = _be32(data, 4)
    if hdr_version > BCI_HEADER_MAX_VERSION:
        raise ValueError(f"header version {hdr_version:#x} > {BCI_HEADER_MAX_VERSION:#x}")

    rec_table_off = _be32(data, 0x10)
    hdr_size = _be32(data, 0x14)
    img_data_off = _be32(data, 0x18)
    img_data_size = _be32(data, 0x1C)

    # Image-size field is 0 in v0 files. Updater would refuse to send blocks
    # then, but the on-disk firmware still extends to EOF -- fall back to that.
    img_data_size_eff = img_data_size if img_data_size else (n - img_data_off)

    if img_data_off == 0 or img_data_off >= n:
        raise ValueError(f"invalid image-data offset {img_data_off:#x} (file {n:#x})")
    if img_data_off + img_data_size_eff > n:
        raise ValueError(
            f"image extends past EOF: off={img_data_off:#x} size={img_data_size_eff:#x} file={n:#x}"
        )
    if img_data_size_eff % BLOCK_SIZE != 0:
        # Not fatal for the updater (which streams arbitrary chunks), but the
        # block decoder needs whole 0x226 blocks. Warn via the result.
        pass

    return {
        "magic": data[:4],
        "header_version": hdr_version,
        "header_version_str": f"{(hdr_version >> 24) & 0xFF}.{(hdr_version >> 16) & 0xFF}",
        "header_size": hdr_size,
        "rec_table_off": rec_table_off,
        "image_data_off": img_data_off,
        "image_data_size": img_data_size,
        "image_data_size_eff": img_data_size_eff,
        "block_count": img_data_size_eff // BLOCK_SIZE,
        "tail_bytes": img_data_size_eff % BLOCK_SIZE,
        "records": parse_bci_records(data, rec_table_off),
    }


def find_binary_start(data: bytes) -> int:
    """Back-compat: return the file offset where the 0x226 blocks start.
    Prefers the header field; falls back to the historical `len % 0x226` trick
    for files that lack a parseable BCI header."""
    try:
        return parse_bci_header(data)["image_data_off"]
    except ValueError:
        return len(data) % BLOCK_SIZE


def _print_header_info(filepath: str, data: bytes, header: dict) -> None:
    print(f"File: {filepath}")
    print(f"Size: {len(data)} bytes ({len(data):#x})")
    print(f"Header version: {header['header_version_str']} ({header['header_version']:#010x})")
    print(f"Header size:    {header['header_size']} ({header['header_size']:#x})")
    print(f"Image offset:   {header['image_data_off']:#x}")
    img_size = header['image_data_size']
    if img_size:
        print(f"Image size:     {img_size} ({img_size:#x})")
    else:
        eff = header['image_data_size_eff']
        print(f"Image size:     0 in header; using {eff} ({eff:#x}) from file_size - offset")
    print(f"Blocks:         {header['block_count']} x {BLOCK_SIZE:#x}"
          + (f" (+ {header['tail_bytes']} tail bytes)" if header['tail_bytes'] else ""))

    rec = header["records"]
    if rec["records"]:
        print(f"Records ({rec['rec_count']}):")
        for r in rec["records"]:
            print(f"  @{r['offset']:#06x} type={r['type']} ({r['name']}) "
                  f"rec_size={r['rec_size']} payload={r['payload_size']}")
    if rec["targets"]:
        print(f"Targets ({rec['tgt_count']}):")
        for t in rec["targets"]:
            hw = ".".join(str(b) for b in t["hw_range"])
            print(f"  @{t['offset']:#06x} addr={t['addr']:#010x} "
                  f"keys={t['key1']:#06x}/{t['key2']:#06x} hw={hw}")


FLASH_IMAGE_SIZE = 0x10000  # 64 KB MC9S12D64 staged flash image
APP_SIZE = 0xF800           # the image an update file carries: up to the bootloader
LOADER_ADDR = 0x1000        # the loader's RAM, dest_keys 0x00-0x06
LOADER_SIZE = 0x800

# The application's identification area (ms1-firmware src/sys/ident.c), in page $3F: the image
# offset is the CPU address
IDENT = 0xF700              # its identification words
IDENT_NAME = 0xF720         # the pointer to its product name
VERSION_WORD = 0xF73C       # the application version word

# The release files' target: the manufacturer (ESU, a device's 0xFF14), and per application
# version word the device's hardware version range (major.minor from, to: its 0xFF18), then
# the bootloader version range (0xFF24)
TARGET_MANUFACTURER = 0x97
TARGET_HW_VERSIONS = {0x4011: b"\x02\x00\x02\xff", 0x4010: b"\x01\x00\x01\xff"}
TARGET_BOOT_VERSIONS = b"\x01\x00\x01\xff"


def loader_block_count(header: dict) -> int:
    """The blocks, from the first, that carry the Loader record (module docstring)."""
    loader = header["records"]["loader"]
    return -(-loader["payload_size"] // FLASH_BYTES) if loader else 0


def decode_bci_file(filepath: str, output_path: str = None) -> bytes:
    """Decode all blocks in a .bci file into a single 64 KB image.

    Each block's decoded 0x200 payload is placed at offset (dest_key * 0x100)
    in the output image. dest_key always even, so dest_key in 0x00..0xFE covers
    128 contiguous 0x200 chunks = 0x10000 bytes. The loader's blocks
    (loader_block_count) are decoded and checked but left out of the image.
    """
    with open(filepath, "rb") as f:
        data = f.read()

    try:
        header = parse_bci_header(data)
    except ValueError as e:
        print(f"File: {filepath}")
        print(f"Size: {len(data)} bytes ({len(data):#x})")
        print(f"Warning: {e}; falling back to len%0x226 layout.")
        header = None

    loader_blocks = 0
    if header is not None:
        _print_header_info(filepath, data, header)
        binary_start = header["image_data_off"]
        block_count = header["block_count"]
        loader_blocks = loader_block_count(header)
    else:
        binary_start = len(data) % BLOCK_SIZE
        block_count = (len(data) - binary_start) // BLOCK_SIZE
        print(f"Header: {binary_start} bytes ({binary_start:#x})")
        print(f"Blocks: {block_count} x {BLOCK_SIZE:#x}")
    print()

    image = bytearray(FLASH_IMAGE_SIZE)
    written = bytearray(FLASH_IMAGE_SIZE // FLASH_BYTES)  # write count per slot
    loader = bytearray(LOADER_SIZE)

    valid = 0
    crc_ok = 0
    failed = 0

    for i in range(block_count):
        offset = binary_start + i * BLOCK_SIZE
        block = data[offset:offset + BLOCK_SIZE]
        result = decode_bci_block(block)

        if not result["valid"]:
            failed += 1
            if failed <= 5:
                print(f"  Block {i:4d} @ {offset:#08x}: FAILED - {result['error']}")
            continue

        valid += 1
        if result["crc_valid"]:
            crc_ok += 1

        slot = result["dest_key"] >> 1                 # 0..0x7F
        dst = result["dest_key"] * 0x100               # == slot * FLASH_BYTES
        if i < loader_blocks:
            if dst < LOADER_SIZE:
                loader[dst:dst + FLASH_BYTES] = result["payload"]
            where = f"loader RAM {LOADER_ADDR + dst:#06x}"
        else:
            image[dst:dst + FLASH_BYTES] = result["payload"]
            written[slot] += 1
            where = f"img[{dst:#06x}]"

        if i < 8 or i % 32 == 0:
            status = "OK" if result["crc_valid"] else f"CRC={result['crc_state']:#06x}"
            print(
                f"  Block {i:4d} @ {offset:#08x}: "
                f"key={result['dest_key']:#04x} -> {where} "
                f"stride={result['stride']:3d} dstride={result['dest_stride']:3d} "
                f"xor_key={result['xor_key']:#04x} {status}"
            )

    print()
    print(f"Results: {valid}/{block_count} valid, {crc_ok}/{block_count} CRC OK, {failed} failed")
    if loader_blocks:
        crc = crc16_ccitt(loader)
        print(f"Loader:  RAM {LOADER_ADDR:#06x}-{LOADER_ADDR + LOADER_SIZE - 1:#06x}, CRC-16 "
              + ("0: the bootloader runs it" if crc == 0 else f"{crc:#06x}: the bootloader refuses it"))

    empty = [i for i, c in enumerate(written) if c == 0]
    overwritten = [i for i, c in enumerate(written) if c > 1]
    if empty:
        print(f"Empty slots ({len(empty)}): " +
              ", ".join(f"{i:#04x}" for i in empty))
    if overwritten:
        print(f"Slots written more than once (the last counts): " +
              ", ".join(f"{i:#04x}(x{written[i]})" for i in overwritten))

    if output_path is None:
        base = os.path.splitext(os.path.basename(filepath))[0]
        output_path = f"{base}.bin"
    parent = os.path.dirname(output_path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(output_path, "wb") as f:
        f.write(image)
    print(f"Saved {output_path} ({FLASH_IMAGE_SIZE} bytes)")

    return bytes(image)


def encode_bci_block(payload: bytes, dest_key: int, *,
                     stride: int = 7,
                     dest_stride: int = 8,
                     header_offset: int = 0,
                     scatter_pos: int = 0,
                     xor_state: int = 0,
                     xor_key: int = 0,
                     init_seed: int = 0,
                     init_state: int = 0,
                     skipped: int = 0,
                     padding: bytes = b"\x00\x00\x00\x00",
                     trailing: bytes = b"\x00\x00\x00\x00",
                     reserved: bytes = b"\x00\x00",
                     transport: bytes = b"\x00\x00\x00\x00") -> bytes:
    """Inverse of decode_bci_block. Produces a 0x226-byte transport block.

    Stages:
      1. Build the 541-byte strided sequence: 14-byte header + 521-byte XOR
         stream over `payload` + 6 trailing bytes (4 free + 2 CRC closure).
      2. Scatter the sequence into the descrambled buffer along `stride`.
      3. Forward-scramble bytes [5,546) using the byte-stream cipher with
         block[0]=init_seed and block[1]=init_state (state stays constant).
    """
    if len(payload) != FLASH_BYTES:
        raise ValueError(f"payload must be {FLASH_BYTES} bytes, got {len(payload)}")
    if dest_key & 1:
        raise ValueError(f"dest_key must be even, got {dest_key:#x}")
    if not (3 <= stride <= 258):
        # stride byte = stride-3 must fit in a u8.
        raise ValueError(f"stride must be 3..258, got {stride}")
    if dest_stride % XOR_LOOP == 0:
        raise ValueError(f"dest_stride {dest_stride} not coprime to {XOR_LOOP}")
    if len(padding) != 4 or len(trailing) != 4:
        raise ValueError("padding and trailing must be 4 bytes each")
    if len(reserved) != 2 or len(transport) != 4:
        raise ValueError("reserved must be 2 bytes, transport 4")

    # Step 1: 14-byte header.
    seq = bytearray(PAYLOAD_LEN)
    seq[0] = (stride - 3) & 0xFF                  # re-read of stride seed
    seq[1] = (dest_stride - 7) & 0xFF             # hb1
    seq[2] = 0x09                                 # magic1
    seq[3] = 0x10                                 # magic2
    seq[4] = dest_key & 0xFF
    seq[5] = 0                                    # dest_check
    seq[6] = scatter_pos & 0xFF
    seq[7] = skipped & 0xFF
    seq[8] = xor_state & 0xFF
    seq[9] = xor_key & 0xFF
    seq[10:14] = padding

    # Step 2: XOR stream cipher forward, 521 iterations.
    sp = scatter_pos & 0xFF
    xs = xor_state & 0xFF
    for k in range(XOR_LOOP):
        # plain=0 for the 9 scatter slots that fall outside the 0x200 payload;
        # decoder discards their value but still updates state from scrambled.
        plain = payload[sp] if sp < FLASH_BYTES else 0
        scrambled = (plain ^ xs) & 0xFF
        seq[14 + k] = scrambled
        carry = (scrambled >> 7) & 1
        xs = (((scrambled << 1) & 0xFF) + xor_key + carry) & 0xFF
        sp += dest_stride
        if sp >= XOR_LOOP:
            sp -= XOR_LOOP

    # Step 3a: 4 free trailing bytes (positions 535..538).
    seq[535:539] = trailing

    # Step 3b: CRC closure into seq[539..540].
    # The bootloader's CRC-16 algorithm is the "augmented" form: feeding the
    # message followed by its raw (M||0x0000) CRC drives the register to 0.
    # And because no bit is shifted out of the 16-bit register during the
    # final 16 closing bits when starting from 0, the closure value equals
    # the CRC of (real_539_bytes || 0 || 0) -- big-endian.
    crc = 0xFFFF
    for i in range(539):
        crc = crc16_ccitt_update(crc, seq[i])
    crc = crc16_ccitt_update(crc, 0)
    crc = crc16_ccitt_update(crc, 0)
    seq[539] = (crc >> 8) & 0xFF
    seq[540] = crc & 0xFF

    # Step 4: scatter the strided sequence into the descrambled buffer.
    desc = bytearray(BLOCK_SIZE)
    desc[2] = header_offset & 0xFF
    desc[3:PAYLOAD_START] = reserved
    pos = PAYLOAD_START + (header_offset & 0xFF)
    if pos >= PAYLOAD_END:
        pos -= PAYLOAD_LEN
    for k in range(PAYLOAD_LEN):
        desc[pos] = seq[k]
        pos += stride
        if pos >= PAYLOAD_END:
            pos -= PAYLOAD_LEN

    # Step 5: forward-scramble. Mirrors pre_descramble; encrypts plain->scrambled.
    block = bytearray(BLOCK_SIZE)
    block[0] = init_seed & 0xFF
    block[1] = init_state & 0xFF
    block[2] = header_offset & 0xFF
    block[3:PAYLOAD_START] = reserved
    seed = init_seed & 0xFF
    state = init_state & 0xFF
    for i in range(PAYLOAD_START, PAYLOAD_END):
        plain = desc[i]
        scrambled = (plain ^ seed) & 0xFF
        block[i] = scrambled
        temp = (scrambled + state) & 0xFF
        carry = (temp >> 7) & 1
        temp = (temp << 1) & 0xFF
        seed = (temp + 0x0D + carry) & 0xFF
    block[PAYLOAD_END:BLOCK_SIZE] = transport
    return bytes(block)


def bci_header(image: bytes, block_count: int) -> bytes:
    """The header of the release files, for `image` followed by `block_count` blocks: the records
    table, the Loader and Application records, the latter with the image's identification
    words, version word and product name, and the target for the version word."""
    version = _be16(image, VERSION_WORD)
    if version not in TARGET_HW_VERSIONS:
        raise ValueError(f"no target for the application version word {version:#06x}")
    name_at = _be16(image, IDENT_NAME)
    name = image[name_at:image.index(b"\0", name_at)]
    if len(name) >= 0x20:
        raise ValueError(f"product name {name!r} longer than 31 bytes")

    h = bytearray(0xC0)

    def put32(off: int, value: int) -> None:
        h[off:off + 4] = value.to_bytes(4, "big")

    h[0:4] = BCI_MAGIC
    put32(0x04, 0x00020000)                 # header version 0.2
    put32(0x10, 0x20)                       # the records table
    put32(0x14, len(h) - 0x20)              # header size: the records table's
    put32(0x18, len(h))                     # the blocks
    put32(0x1C, block_count * BLOCK_SIZE)
    put32(0x20, 0x20)                       # table: the first record, from the table
    put32(0x24, 2)                          #   records
    put32(0x30, 0x90)                       #   the targets, from the table
    put32(0x34, 1)                          #   targets
    put32(0x40, 8)                          # Loader: type, record size, payload size
    put32(0x44, 0x10)
    put32(0x48, LOADER_SIZE)
    put32(0x50, 1)                          # Application: type, record size, payload size
    put32(0x54, 0x60)
    put32(0x58, APP_SIZE)
    h[0x60:0x70] = image[IDENT:IDENT + 0x10]
    h[0x7C:0x7E] = image[VERSION_WORD:VERSION_WORD + 2]
    put32(0x80, 0x40)                       # the product name, from the record
    h[0x90:0x90 + len(name)] = name
    put32(0xB0, TARGET_MANUFACTURER)        # the target
    h[0xB4:0xB6] = image[VERSION_WORD:VERSION_WORD + 2]
    h[0xB6:0xB8] = b"\x4f\xff"
    h[0xB8:0xBC] = TARGET_HW_VERSIONS[version]
    h[0xBC:0xC0] = TARGET_BOOT_VERSIONS
    return bytes(h)


def block_params(rng: random.Random) -> dict:
    """encode_bci_block's parameters: any value in these ranges decodes (the strides are
    coprime to the prime lengths they walk); the release files' look random."""
    r = rng.randrange
    return {
        "stride": r(3, 259), "dest_stride": r(7, 263), "header_offset": r(256),
        "scatter_pos": r(256), "xor_state": r(256), "xor_key": r(256), "init_seed": r(256),
        "init_state": r(256), "skipped": r(256), "padding": rng.randbytes(4),
        "trailing": rng.randbytes(4), "reserved": rng.randbytes(2), "transport": rng.randbytes(4),
    }


def encode_bci(image: bytes, loader: bytes) -> bytes:
    """An update file for `image` with `loader`: bci_header, then the loader's blocks and every
    slot of the image below APP_SIZE, in dest_key order, with parameters from a fixed seed."""
    if len(image) != FLASH_IMAGE_SIZE:
        raise ValueError(
            f"image must be {FLASH_IMAGE_SIZE} bytes ({FLASH_IMAGE_SIZE:#x}), got {len(image)}"
        )
    if len(loader) != LOADER_SIZE or crc16_ccitt(loader) != 0:
        raise ValueError(f"the loader must be {LOADER_SIZE:#x} bytes with a CRC-16 of 0")
    parts = [(dk, loader) for dk in range(0, LOADER_SIZE // 0x100, 2)]
    parts += [(dk, image) for dk in range(0, APP_SIZE // 0x100, 2)]
    rng = random.Random(0)
    out = bytearray(bci_header(image, len(parts)))
    for dk, src in parts:
        out += encode_bci_block(src[dk * 0x100:dk * 0x100 + FLASH_BYTES], dk, **block_params(rng))
    return bytes(out)


def extract_loader(data: bytes) -> bytes:
    """The loader of the update file `data`, as the bootloader puts it in RAM."""
    header = parse_bci_header(data)
    start = header["image_data_off"]
    loader = bytearray(LOADER_SIZE)
    for i in range(loader_block_count(header)):
        r = decode_bci_block(data[start + i * BLOCK_SIZE:start + (i + 1) * BLOCK_SIZE])
        if not r["valid"] or not r["crc_valid"] or r["dest_key"] * 0x100 >= LOADER_SIZE:
            raise ValueError(f"block {i}: not a loader block")
        loader[r["dest_key"] * 0x100:r["dest_key"] * 0x100 + FLASH_BYTES] = r["payload"]
    if crc16_ccitt(loader) != 0:
        raise ValueError("the loader's CRC-16 is not 0")
    return bytes(loader)


def _srec(kind: str, addr: int, data: bytes) -> str:
    rec = bytes([len(data) + 3]) + addr.to_bytes(2, "big") + data
    return f"{kind}{rec.hex().upper()}{~sum(rec) & 0xFF:02X}\n"


def write_srec(path: str, addr: int, data: bytes) -> None:
    """S1 records of 32 bytes for `data` at `addr`."""
    with open(path, "w") as f:
        f.write(_srec("S0", 0, b"loader"))
        for off in range(0, len(data), 32):
            f.write(_srec("S1", addr + off, data[off:off + 32]))
        f.write(_srec("S9", 0, b""))


def read_loader(path: str) -> bytes:
    """A loader from S1 records covering LOADER_ADDR..+LOADER_SIZE."""
    loader = bytearray(LOADER_SIZE)
    covered = 0
    with open(path) as f:
        for n, line in enumerate(f, 1):
            if not line.startswith("S1"):
                continue
            rec = bytes.fromhex(line[2:].strip())
            addr, data = _be16(rec, 1), rec[3:-1]
            if (rec[0] != len(rec) - 1 or sum(rec) & 0xFF != 0xFF or addr < LOADER_ADDR
                    or addr + len(data) > LOADER_ADDR + LOADER_SIZE):
                raise ValueError(f"{path}:{n}: bad S1 record for {LOADER_ADDR:#06x}-"
                                 f"{LOADER_ADDR + LOADER_SIZE - 1:#06x}")
            loader[addr - LOADER_ADDR:addr - LOADER_ADDR + len(data)] = data
            covered += len(data)
    if covered != LOADER_SIZE:
        raise ValueError(f"{path}: {covered} of {LOADER_SIZE} loader bytes")
    return bytes(loader)


def encode_bci_file(bin_path: str, loader_path: str, output_path: str = None) -> bytes:
    """encode_bci from files: the image, the loader as S-records (read_loader)."""
    with open(bin_path, "rb") as f:
        image = f.read()
    out = encode_bci(image, read_loader(loader_path))
    if output_path is None:
        base = os.path.splitext(os.path.basename(bin_path))[0]
        output_path = f"{base}.bci"

    print(f"Image:  {bin_path}")
    print(f"Loader: {loader_path}")
    _print_header_info(output_path, out, parse_bci_header(out))

    parent = os.path.dirname(output_path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(output_path, "wb") as f:
        f.write(out)
    print(f"Saved {output_path} ({len(out)} bytes)")

    return out


_USAGE = (
    "Usage:\n"
    "  bci.py decode <file.bci> [output.bin]\n"
    "  bci.py encode <file.bin> <loader.s19> [output.bci]\n"
    "  bci.py loader <file.bci> <loader.s19>             # the update file's loader\n"
    "  bci.py <file.bci> [output.bin]                    # decode\n"
)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(_USAGE, end="")
        sys.exit(1)

    cmd = sys.argv[1]
    if cmd == "decode":
        if len(sys.argv) < 3:
            print(_USAGE, end="")
            sys.exit(1)
        out = sys.argv[3] if len(sys.argv) > 3 else None
        decode_bci_file(sys.argv[2], out)
    elif cmd == "encode":
        if len(sys.argv) < 4:
            print(_USAGE, end="")
            sys.exit(1)
        out = sys.argv[4] if len(sys.argv) > 4 else None
        encode_bci_file(sys.argv[2], sys.argv[3], out)
    elif cmd == "loader" and len(sys.argv) == 4:
        with open(sys.argv[2], "rb") as f:
            write_srec(sys.argv[3], LOADER_ADDR, extract_loader(f.read()))
    elif os.path.exists(cmd):
        # positional `<file.bci> [output.bin]` decodes
        out = sys.argv[2] if len(sys.argv) > 2 else None
        decode_bci_file(cmd, out)
    else:
        print(f"unknown command or missing file: {cmd!r}\n", file=sys.stderr)
        print(_USAGE, end="")
        sys.exit(1)
