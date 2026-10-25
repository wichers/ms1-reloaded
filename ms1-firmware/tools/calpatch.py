"""Override words of an S-record image, for experiments in the emulator: appends S1 records
with the given values (hex), which ms1emu and difftest load over the image's own. The
bootloader's calibration words (0xFF60-0xFF7F: flash_adc*_scale, flash_current_limit in
gcc/ms1.ld) are the usual target:

  python3 tools/calpatch.py IN.s19 OUT.s19 ADDR=VALUE...
"""
import sys


def s1(addr, data):
    b = [len(data) + 3, addr >> 8, addr & 0xFF] + data
    return "S1" + "".join("%02X" % x for x in b) + "%02X" % (~sum(b) & 0xFF)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    words = {}
    for arg in sys.argv[3:]:
        k, v = arg.split("=")
        words[int(k, 16)] = int(v, 16)
    lines = open(sys.argv[1]).read().split()
    out = [l for l in lines if not l.startswith("S9")]
    out += [s1(a, [v >> 8, v & 0xFF]) for a, v in sorted(words.items())]
    out += [l for l in lines if l.startswith("S9")]
    open(sys.argv[2], "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
