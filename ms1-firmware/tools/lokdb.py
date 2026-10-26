"""The locomotive database of page $3C: its readable form and the page's binary format.

The database is a list of entries; the MS1's "new locomotive from the database" menu steps
through them. An entry holds one locomotive, or several (a set) added one after the other.
Its text form, third_party/lokdb.tsv, has one line per locomotive; the lines of an
entry share its number, in order:

  entry      the entry's number (0 up, every number used)
  article    the article number (0-131071)
  brand      M (Märklin) or T (Trix)
  symbol     none, electric, diesel or steam
  protocol   MM, DCC, or the protocol code (4, 5)
  flags      LOKP_* names of src/include/lokstack.h without the prefix (MM1_REVERSE, MM2,
             F5_F8, PROGRAMMABLE, VOLUME), 0x08 and 0x20 for the unnamed bits, NO_SPEED for a
             locomotive that cannot be driven (speed_object 0); comma-separated, - for none
  address    the MM or DCC address (0-127)
  functions  the function symbols, F0 first: numbers, m for momentary (bit 7); - for none
  name       in double quotes, with \\" and \\\\; Ä, Ö, Ü and ß are the display font's a, o, u
             and d, other characters printable ASCII

The binary format in page $3C, as the accessors (src/lok/lokdb.c) read it: the number of
entries (word), a pointer per entry, the entries, a zero word. An entry is its locomotives
one after the other, each:
  word      article bits 0-15
  byte      bits 0-3 number of function symbols, 4-5 symbol, 6 Trix, 7 article bit 16
  byte      bits 0-1 protocol - 2, bits 2-7 protocol flags (LOKP_* bits 0-5)
  byte      bits 0-6 address, bit 7 another locomotive follows
  bytes     the function symbols
  bytes     the name, 0-terminated; bit 7 of its first character is LOKP_VOLUME, of the
            second NO_SPEED
lokdb_p3c_read copies 56 bytes from an entry's start, whatever its length.

Usage: python3 tools/lokdb.py extract <release.bin> <lokdb.tsv>
       python3 tools/lokdb.py asm <lokdb.tsv> <lokdb.s>
"""
import sys

PAGE = 0x8000
SYMBOLS = ["none", "electric", "diesel", "steam"]
PROTOCOLS = {0: "DCC", 1: "MM"}
FLAG_BITS = ["MM1_REVERSE", "MM2", "F5_F8", "0x08", "PROGRAMMABLE", "0x20"]
UMLAUTS = {"a": "Ä", "o": "Ö", "u": "Ü", "d": "ß"}
COLUMNS = ["entry", "article", "brand", "symbol", "protocol", "flags", "address", "functions", "name"]


def to_text(raw):
    s = "".join(UMLAUTS.get(chr(c), chr(c)) for c in raw)
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def from_text(field, where):
    if len(field) < 2 or field[0] != '"' or field[-1] != '"':
        sys.exit(f"{where}: name not in double quotes")
    s, out, i = field[1:-1], bytearray(), 0
    back = {v: k for k, v in UMLAUTS.items()}
    while i < len(s):
        c = s[i]
        if c == "\\":
            i += 1
            c = s[i] if i < len(s) else ""
            if c not in '"\\':
                sys.exit(f"{where}: bad escape in the name")
        elif c in back:
            c = back[c]
        elif not " " <= c <= "~":
            sys.exit(f"{where}: {c!r} has no glyph")
        out.append(ord(c))
        i += 1
    return out


def extract(image):
    page = image[:0x4000]               # the update file's page order: $3C first
    count = page[0] << 8 | page[1]
    lines = []
    for e in range(count):
        q = (page[2 + 2 * e] << 8 | page[3 + 2 * e]) - PAGE
        while True:
            art, info, proto, addr = page[q] << 8 | page[q + 1], page[q + 2], page[q + 3], page[q + 4]
            n = info & 0x0F
            funcs = page[q + 5:q + 5 + n]
            z = page.index(0, q + 5 + n)
            name = bytearray(page[q + 5 + n:z])
            flags = [FLAG_BITS[b] for b in range(6) if proto >> 2 & 1 << b]
            for i, flag in ((0, "VOLUME"), (1, "NO_SPEED")):
                if len(name) > i and name[i] & 0x80:
                    flags.append(flag)
                    name[i] &= 0x7F
            lines.append([str(e), str((info >> 7) << 16 | art), "T" if info & 0x40 else "M",
                          SYMBOLS[info >> 4 & 3], PROTOCOLS.get(proto & 3, str((proto & 3) + 2)),
                          ",".join(flags) or "-", str(addr & 0x7F),
                          " ".join(f"{f & 0x7F}{'m' if f & 0x80 else ''}" for f in funcs) or "-",
                          to_text(name)])
            q = z + 1
            if not addr & 0x80:
                break
    return lines


def encode(fields, more, where):
    _, article, brand, symbol, protocol, flags, address, functions, name = fields
    article, address = int(article), int(address)
    if not 0 <= article < 1 << 17 or not 0 <= address < 0x80:
        sys.exit(f"{where}: article or address out of range")
    if brand not in ("M", "T") or symbol not in SYMBOLS:
        sys.exit(f"{where}: brand M or T, symbol one of {', '.join(SYMBOLS)}")
    code = {v: k for k, v in PROTOCOLS.items()}.get(protocol)
    if code is None:
        code = int(protocol) - 2 if protocol in ("4", "5") else sys.exit(f"{where}: unknown protocol")
    funcs = [] if functions == "-" else functions.split()
    if len(funcs) > 15:
        sys.exit(f"{where}: more than 15 functions")
    raw = from_text(name, where)
    bits = 0
    for f in ([] if flags == "-" else flags.split(",")):
        if f in FLAG_BITS:
            bits |= 1 << FLAG_BITS.index(f)
        elif f in ("VOLUME", "NO_SPEED"):
            i = 0 if f == "VOLUME" else 1
            if len(raw) <= i:
                sys.exit(f"{where}: {f} needs a name of {i + 1} characters or more")
            raw[i] |= 0x80
        else:
            sys.exit(f"{where}: unknown flag {f}")
    out = bytearray([article >> 8 & 0xFF, article & 0xFF,
                     (article >> 16) << 7 | (brand == "T") << 6 | SYMBOLS.index(symbol) << 4 | len(funcs),
                     bits << 2 | code, address | more << 7])
    for f in funcs:
        v = int(f.rstrip("m"))
        if not 0 <= v < 0x80:
            sys.exit(f"{where}: function symbol {f} out of range")
        out.append(v | (0x80 if f.endswith("m") else 0))
    return out + raw + b"\0"


def read_tsv(path):
    entries = []
    for no, line in enumerate(open(path, encoding="utf-8"), 1):
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.rstrip("\n").split("\t")
        where = f"{path}:{no}"
        if len(fields) != len(COLUMNS):
            sys.exit(f"{where}: {len(COLUMNS)} tab-separated columns expected")
        e = int(fields[0])
        if e == len(entries):
            entries.append([])
        elif e != len(entries) - 1:
            sys.exit(f"{where}: entry {e} out of order")
        entries[-1].append((fields, where))
    return entries


def asm(entries, src):
    out = [f"; The locomotive database, generated by tools/lokdb.py from {src}: edit that file.",
           "\t.sect\t.rodata",
           "\t.globl\tlokdb_entry_count, lokdb_entries",
           "lokdb_entry_count:",
           f"\t.word\t{len(entries)}",
           "lokdb_entries:"]
    out += [f"\t.word\tlokdb_{e}" for e in range(len(entries))]
    for e, locos in enumerate(entries):
        out.append(f"lokdb_{e}:")
        for i, (fields, where) in enumerate(locos):
            data = encode(fields, i + 1 < len(locos), where)
            out.append(f"\t; {fields[1]} {fields[-1]}")
            for k in range(0, len(data), 16):
                out.append("\t.byte\t" + ",".join(f"0x{b:02X}" for b in data[k:k + 16]))
    out.append("\t.word\t0\t\t\t; after the last entry")
    return "\n".join(out) + "\n"


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("extract", "asm"):
        sys.exit(__doc__)
    if sys.argv[1] == "extract":
        lines = extract(open(sys.argv[2], "rb").read())
        with open(sys.argv[3], "w", encoding="utf-8") as f:
            f.write("# The locomotive database of page $3C (tools/lokdb.py describes the columns)\n")
            f.write("# " + "\t".join(COLUMNS) + "\n")
            for fields in lines:
                f.write("\t".join(fields) + "\n")
    else:
        with open(sys.argv[3], "w") as f:
            f.write(asm(read_tsv(sys.argv[2]), sys.argv[2]))


main()
