"""Decode the track signal logged by `ms1emu -T`: Märklin Motorola (MM) packets, DCC packets
and the mfx bit stream.

The track level is PT4 (PT5 drives the other side of the bridge). From the edges come the
pulse widths:
- MM: 18 bits, each 208 us at locomotive rate (104 us for solenoid packets): '1' = long high
  (182 us) + short low (26 us), '0' = short high + long low. A packet is sent twice with a
  short gap (about 1.5 ms); a longer gap separates packets.
- DCC (RCN-210, RCN-211): each level is half a bit, 52-64 us for a 1, 90-10000 us for a 0; a
  packet is a preamble of at least 10 ones, then each byte after a 0, a 1 after the last byte.
  Each packet is printed with its preamble length, bytes, checksum and meaning (RCN-212/213/
  214/216). Tested before mfx: DCC's halves fall inside mfx's looser cell windows.
- mfx: 100 us bit cells with a level change at every cell boundary; a '1' has another change
  in the middle (two 50 us half cells), a '0' none. Each burst of cells is printed as its bit
  string (sync patterns, bit stuffing and CRC are not interpreted).

Usage: python3 -I tools/track_decode.py <track.log>
"""
import sys


def load(path):
    edges = []
    for line in open(path):
        p = line.split()
        if len(p) == 4:
            edges.append((float(p[0]), int(p[2])))   # time, PT4 level
    return edges


def segments(edges):
    """(level, duration_us, start) of each constant-level run of PT4."""
    out = []
    last_t, last_l = None, None
    for t, lvl in edges:
        if last_l is not None and lvl != last_l:
            out.append((last_l, (t - last_t) * 1e6, last_t))
            last_t, last_l = t, lvl
        elif last_l is None:
            last_t, last_l = t, lvl
    return out


def near(v, target, tol=0.25):
    return abs(v - target) <= target * tol


TRIT = {"00": "0", "11": "1", "10": "2", "01": "?"}   # MM: open trit is '10'


def mm_packet(bits):
    """Interpret 18 MM bits (as '0'/'1' string): address trits, function, speed."""
    trits = "".join(TRIT.get(bits[i:i + 2], "?") for i in range(0, 8, 2))
    # address: trits valued 0, 1 (=1) and 2 (open) in base 3, first trit least significant
    addr = 0
    for k, tr in enumerate(trits):
        addr += {"0": 0, "1": 1, "2": 2}.get(tr, 0) * 3 ** k
    func = bits[8:10]
    data = bits[10:18]
    speed = sum(int(data[2 * i]) << i for i in range(4))
    mm2 = data[1::2] != data[0::2]
    return f"MM addr trits {trits} (={addr}) F={func} speed={speed}{' (MM2 bits ' + data[1::2] + ')' if mm2 else ''}"


def dcc_half(us):
    """1 or 0 for a DCC half bit (RCN-210 receiver limits), None for anything else"""
    if 52 - 0.01 <= us <= 64 + 0.01:
        return 1
    if 90 - 0.01 <= us <= 10000:
        return 0
    return None


def dcc_meaning(b):
    """what a DCC packet (without its XOR byte) commands"""
    if b[0] == 0xFF:
        return "idle"
    if b[:2] == [0, 0] and len(b) == 2:
        return "reset"
    if (b[0] & 0xF0) == 0x70 and len(b) == 3:
        cv = ((b[0] & 3) << 8 | b[1]) + 1
        cc = b[0] >> 2 & 3
        if cc == 2:
            return f"service: {'write' if b[2] & 0x10 else 'verify'} CV {cv} bit {b[2] & 7} = {b[2] >> 3 & 1}"
        return f"service: {'verify' if cc == 1 else 'write'} CV {cv} = {b[2]}"
    if 0x80 <= b[0] <= 0xBF and len(b) >= 2:
        dec = (b[0] & 0x3F) | ((~b[1] >> 4) & 7) << 6
        port = b[1] >> 1 & 3
        if b[1] & 0x80:
            return (f"accessory {dec} port {port} (output {(dec - 1) * 4 + port + 1}) "
                    f"{b[1] & 1} {'on' if b[1] & 8 else 'off'}")
        return f"accessory {dec} port {port} aspect {b[2] if len(b) > 2 else '?'}"
    if b[0] <= 127:
        who, i = ("broadcast" if b[0] == 0 else f"loco {b[0]}"), 1
    elif b[0] <= 231 and len(b) >= 2:
        who, i = f"loco {(b[0] & 0x3F) << 8 | b[1]} (long)", 2
    else:
        return "reserved address"
    ins = b[i:]
    if not ins:
        return who
    c = ins[0]
    if c == 0x3F and len(ins) > 1:
        s = ins[1] & 0x7F
        d = "+" if ins[1] & 0x80 else "-"
        return f"{who}: speed {d}{'estop' if s == 1 else max(s - 1, 0)}/126"
    if (c & 0xC0) == 0x40:
        s = (c & 0x0F) << 1 | (c >> 4 & 1)
        return f"{who}: speed {'+' if c & 0x20 else '-'}{'estop' if s in (2, 3) else max(s - 3, 0)}/28"
    if (c & 0xE0) == 0x80:
        return f"{who}: F0 {c >> 4 & 1} F1-F4 {''.join(str(c >> k & 1) for k in range(4))}"
    if (c & 0xF0) in (0xB0, 0xA0):
        first = 5 if (c & 0xF0) == 0xB0 else 9
        return f"{who}: F{first}-F{first + 3} {''.join(str(c >> k & 1) for k in range(4))}"
    if c in (0xDE, 0xDF) and len(ins) > 1:
        first = 13 if c == 0xDE else 21
        return f"{who}: F{first}-F{first + 7} {''.join(str(ins[1] >> k & 1) for k in range(8))}"
    if (c & 0xF0) == 0xE0 and len(ins) >= 3:
        cv = ((c & 3) << 8 | ins[1]) + 1
        cc = c >> 2 & 3
        if cc == 2:
            return f"{who}: POM {'write' if ins[2] & 0x10 else 'verify'} CV {cv} bit {ins[2] & 7} = {ins[2] >> 3 & 1}"
        return f"{who}: POM {'verify' if cc == 1 else 'write'} CV {cv} = {ins[2]}"
    return f"{who}: instruction {c:02X}"


def dcc_packet(segs, i):
    """the DCC packet whose preamble starts at segs[i]: (text, next index), or None"""
    n = len(segs)
    j = i
    while j < n and dcc_half(segs[j][1]) == 1:
        j += 1
    ones = (j - i) // 2
    if ones < 10:
        return None
    halves = []
    k = j
    while k < n and dcc_half(segs[k][1]) is not None:
        halves.append(dcc_half(segs[k][1]))
        k += 1
    bits = []
    for h in range(0, len(halves) - 1, 2):
        if halves[h] != halves[h + 1]:
            break
        bits.append(halves[h])
    out, pos = [], 0
    while pos + 9 <= len(bits) and bits[pos] == 0:
        out.append(int("".join(map(str, bits[pos + 1:pos + 9])), 2))
        pos += 9
        if pos < len(bits) and bits[pos] == 1:
            break
    if len(out) < 3 or pos >= len(bits):
        return None
    x = 0
    for v in out:
        x ^= v
    text = (f"DCC preamble {ones}: {' '.join(f'{v:02X}' for v in out)} "
            f"({'xor ok' if x == 0 else 'BAD XOR'}) {dcc_meaning(out[:-1])}")
    return text, j + 2 * (pos + 1)


def decode(segs):
    i = 0
    n = len(segs)
    while i < n:
        lvl, d, t0 = segs[i]
        if dcc_half(d) == 1:
            p = dcc_packet(segs, i)
            if p:
                print(f"{t0:12.6f} {p[0]}")
                i = p[1]
                continue
        # MM bit: high then low summing to ~208 (or ~104) us
        if lvl == 1 and i + 1 < n and (near(d, 182) or near(d, 26)) and segs[i + 1][0] == 0:
            bits = ""
            j = i
            while j + 1 < n and len(bits) < 18:
                h, l = segs[j][1], segs[j + 1][1]
                if segs[j][0] != 1 or not (near(h + l, 208) or near(h + l, 104)):
                    # the last bit's low phase merges into the gap
                    if len(bits) == 17 and segs[j][0] == 1 and (near(h, 182) or near(h, 26)):
                        bits += "1" if h > 100 else "0"
                    break
                bits += "1" if h > l else "0"
                j += 2
            if len(bits) == 18:
                print(f"{t0:12.6f} {mm_packet(bits)}  [{bits}]")
                i = j + 1
                continue
        # mfx: runs of 100/50 us cells
        if near(d, 100) or near(d, 50):
            j = i
            cells = []
            while j < n and (near(segs[j][1], 100) or near(segs[j][1], 50)):
                cells.append(segs[j][1])
                j += 1
            if len(cells) >= 16:
                bits = ""
                k = 0
                while k < len(cells):
                    if near(cells[k], 50) and k + 1 < len(cells) and near(cells[k + 1], 50):
                        bits += "1"
                        k += 2
                    else:
                        bits += "0"
                        k += 1
                print(f"{t0:12.6f} mfx {len(cells)} cells: {bits}")
            i = j
            continue
        i += 1


def main():
    edges = load(sys.argv[1])
    try:
        decode(segments(edges))
    except BrokenPipeError:
        pass


main()
