"""Decode the track signal logged by `ms1emu -T`: Märklin Motorola (MM) packets and the mfx
bit stream.

The track level is PT4 (PT5 drives the other side of the bridge). From the edges come the
pulse widths:
- MM: 18 bits, each 208 us at locomotive rate (104 us for solenoid packets): '1' = long high
  (182 us) + short low (26 us), '0' = short high + long low. A packet is sent twice with a
  short gap (about 1.5 ms); a longer gap separates packets.
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


def decode(segs):
    i = 0
    n = len(segs)
    while i < n:
        lvl, d, t0 = segs[i]
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
