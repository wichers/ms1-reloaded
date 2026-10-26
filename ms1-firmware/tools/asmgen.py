"""Assembly source for the hand-written assembly parts of the original firmware.

Some of the original is hand-written assembly (the display driver and track signal
generator, interrupt handlers, low-level drivers): code with jump tables into the middle of
routines, shared tails and register conventions that C does not express. Its source here is
GNU as source made from the original's instructions, with labels for branch targets, names
for functions, RAM variables and I/O registers, and data tables as .byte/.word. The result
is checked by assembling it at the original addresses (other symbols at their original
addresses) and comparing the bytes with the original image.

  asmgen.py regs                     write gcc/mc9s12d64.inc from gcc/mc9s12d64.h
  asmgen.py gen MODULE.json OUT.s    generate a module (description below)
  asmgen.py verify MODULE.json SRC.s assemble SRC.s at the original addresses, compare bytes

MODULE.json: {"ranges": [[start, end], ...] (hex strings, end exclusive),
              "names": {"c4ed": "isr_timer_ch4_signal_gen", ...}  entry labels in the module,
              "globals": [...]  names to export (default: all "names"),
              "tables": [[address, words], ...], "code": [address, ...]  code-pointer
                  tables and code nothing else leads to, for ms1-ghidra's module listing
                  (./ghidra.sh listing); not used here}
Names for addresses outside the module: the version's function table (tools/functions.tsv:
original entry -> function here), the entry points of the other modules (asm/*.json) and the
RAM variables defined in src/globals.c (they are at the original's addresses).
Inputs from the original (environment variables, defaults next to this project):
  RELEASE    the application version (tools/releases.py, default 4011): 4010 takes its
             function table, its module descriptions (asm/4010) and its RAM addresses
  LISTING    every instruction of the original, ms1-ghidra's out/listing.tsv (for 4010 the
             modules' instructions, ./ghidra.sh listing)
  APP_IMAGE  the application image decoded from the firmware update file (CPU address =
             offset + 0x4000)
"""
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # releases.py, also under python3 -I
import releases

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LISTING = os.environ.get("LISTING") or os.path.join(ROOT, "..", "ms1-ghidra", "out", "listing.tsv")
IMAGE = os.environ.get("APP_IMAGE") or os.path.join(ROOT, "..", "original-firmware", "4011_1-8_mobile-station-2_A.bin")
REG_HEADER = os.path.join(ROOT, "gcc", "mc9s12d64.h")
REG_INC = os.path.join(ROOT, "gcc", "mc9s12d64.inc")
RELEASE = releases.get()
FUNCTIONS = RELEASE.functions
GLOBALS = os.path.join(ROOT, "src", "globals.c")

REG = {"IX": "x", "IY": "y", "SP": "sp", "PC": "pc", "D": "d", "A": "a", "B": "b", "CCR": "ccr"}


def regs_inc():
    out = ["; MC9S12D64 register addresses (INITRG = $00) for assembly sources, generated from",
           "; gcc/mc9s12d64.h by tools/asmgen.py regs.", ""]
    for line in open(REG_HEADER):
        m = re.match(r"#define (\w+)\s+_REG(8|16)\((0x[0-9A-F]+)\)", line)
        if m:
            out.append(f"\t.equ\t{m.group(1)},{m.group(3)}")
    open(REG_INC, "w").write("\n".join(out) + "\n")


def load_listing():
    insns = {}
    for line in open(LISTING):
        f = line.rstrip("\n").split("\t")
        if len(f) >= 4:
            insns[int(f[0], 16)] = (bytes.fromhex(f[1]), f[2], f[3])
    return insns


def ram_variables():
    """name -> address of the RAM variables (RAM(addr) / RAM_DATA(addr) in src/globals.c, at
    4011's addresses), at their address in RELEASE; those the version lacks are left out"""
    found = {}
    for line in open(GLOBALS):
        m = re.match(r"\s*RAM(?:_DATA)?\((0x[0-9A-Fa-f]+)\)\s+[^;=]*?(\w+)\s*(?:\[[^\]]*\])*\s*(?:=[^;]*)?;", line)
        if m:
            a = RELEASE.ram(int(m.group(1), 16))
            if a is not None:
                found[m.group(2)] = a
    return found


def load_names():
    """address -> name for things outside a module"""
    names = {}
    for line in open(FUNCTIONS):
        if line.startswith("#") or not line.strip():
            continue
        f = line.split()
        names[int(f[0], 16)] = f[1]
    # entry points of the assembly modules
    asm_dir = RELEASE.asm_dir
    for fn in sorted(os.listdir(asm_dir)) if os.path.isdir(asm_dir) else []:
        if fn.endswith(".json"):
            for a, n in json.load(open(os.path.join(asm_dir, fn))).get("names", {}).items():
                names.setdefault(int(a, 16), n)
    ram = {a: n for n, a in ram_variables().items()}
    regs = {}
    for line in open(REG_HEADER):
        m = re.match(r"#define (\w+)\s+_REG(8|16)\((0x[0-9A-F]+)\)", line)
        if m:
            regs.setdefault(int(m.group(3), 16), m.group(1))
    return names, ram, regs


def num(v):
    return str(v) if -16 <= v < 16 else (f"-0x{-v:x}" if v < 0 else f"0x{v:x}")


class Module:
    def __init__(self, desc):
        self.ranges = [(int(a, 16), int(b, 16)) for a, b in desc["ranges"]]
        self.entry_names = {int(a, 16): n for a, n in desc.get("names", {}).items()}
        self.globals = desc.get("globals", list(self.entry_names.values()))
        self.insns = load_listing()
        self.names, self.ram, self.regs = load_names()
        self.image = open(IMAGE, "rb").read()
        self.labels = dict(self.entry_names)
        self.externals = {}          # name -> original address

    def inside(self, a):
        return any(lo <= a < hi for lo, hi in self.ranges)

    def code_addrs(self):
        return sorted(a for a in self.insns if self.inside(a))

    def label(self, a):
        if a in self.labels:
            return self.labels[a]
        if self.inside(a):
            self.labels[a] = f".L{a:04x}"
            return self.labels[a]
        if a in self.names:
            self.externals[self.names[a]] = a
            return self.names[a]
        raise SystemExit(f"asmgen: no name for {a:#06x} (add it to tools/functions.tsv)")

    def data_ref(self, a, size=1):
        """Symbol text for a memory operand address."""
        if self.inside(a) and a in self.labels:
            return self.labels[a]
        if a < 0x400:
            if a in self.regs:
                return self.regs[a]
            if size == 2 and a - 1 in self.regs:
                return self.regs[a - 1] + "+1"
            return num(a)
        if 0x1000 <= a < RELEASE.soft_registers:
            for back in range(0, 0x200):
                if a - back in self.ram:
                    s = self.ram[a - back]
                    return s if back == 0 else f"{s}+{back}"
            return num(a)
        return num(a)

    # operand conversion ------------------------------------------------------------------

    def idx(self, text):
        """Ghidra indexed operand -> gas"""
        t = text.strip()
        m = re.fullmatch(r"(-?0x[0-9a-f]+|-?\d+),\s*([-+]?)(IX|IY|SP|PC)(\+|-)?", t)
        if m:
            off = int(m.group(1), 0)
            pre, reg, post = m.group(2), REG[m.group(3)], m.group(4) or ""
            if pre or post:
                return f"{abs(off)},{pre}{reg}{post}"
            if off < 0 and off < -0x100:
                off &= 0xFFFF
            return f"{num(off)},{reg}"
        m = re.fullmatch(r"(A|B|D),\s*(IX|IY|SP|PC)", t)
        if m:
            return f"{REG[m.group(1)]},{REG[m.group(2)]}"
        m = re.fullmatch(r"\[\s*(D|-?0x[0-9a-f]+|-?\d+),\s*(IX|IY|SP|PC)\s*\]", t)
        if m:
            o = m.group(1)
            o = "d" if o == "D" else num(int(o, 0))
            return f"[{o},{REG[m.group(2)]}]"
        return None

    def operand(self, text, size=1, code=False):
        """One operand: immediate, extended address or indexed."""
        t = text.strip()
        if t.startswith("#"):
            v = int(t[1:], 0)
            if code and self.inside(v):
                return "#" + self.label(v)
            return "#" + num(v)
        i = self.idx(t)
        if i:
            return i
        if re.fullmatch(r"-?0x[0-9a-f]+|\d+", t):
            a = int(t, 0)
            return self.label(a) if code else self.data_ref(a, size)
        raise SystemExit(f"asmgen: operand '{text}'")

    def split_operands(self, ops):
        """Split Ghidra's comma-separated operands, keeping 'off, REG' together."""
        parts = [p.strip() for p in ops.split(",")]
        out = []
        for p in parts:
            if out and (re.fullmatch(r"[-+]?(IX|IY|SP|PC)[+-]?", p)) and not out[-1].endswith("]"):
                out[-1] = out[-1] + ", " + p
            else:
                out.append(p)
        return out

    def convert(self, a, raw, mnem, text):
        ops = text[len(mnem):].strip()
        m = mnem.lower()
        word = mnem in ("LDD", "STD", "LDX", "STX", "LDY", "STY", "LDS", "STS", "CPD", "CPX", "CPY",
                        "CPS", "ADDD", "SUBD", "MOVW", "EMACS")
        if not ops:
            return m
        if mnem in ("BRA", "BRN", "BHI", "BLS", "BCC", "BCS", "BNE", "BEQ", "BVC", "BVS", "BPL",
                    "BMI", "BGE", "BLT", "BGT", "BLE", "BSR") or mnem.startswith("LB"):
            return f"{m}\t{self.label(int(ops, 0))}"
        if mnem in ("DBNE", "DBEQ", "IBNE", "IBEQ", "TBNE", "TBEQ"):
            r, t = [x.strip() for x in ops.split(",")]
            return f"{m}\t{REG[r]},{self.label(int(t, 0))}"
        if mnem in ("EXG", "TFR", "SEX"):
            r1, r2 = [x.strip() for x in ops.split(",")]
            return f"{m}\t{REG.get(r1, r1.lower())},{REG.get(r2, r2.lower())}"
        if mnem in ("JSR", "JMP"):
            i = self.idx(ops)
            if i:
                return f"{m}\t{i}"
            return f"{m}\t{self.label(int(ops, 0))}"
        if mnem == "CALL":
            p = [x.strip() for x in ops.split(",")]
            return f"{m}\t{p[0]},{p[1]}"
        if mnem in ("BRSET", "BRCLR"):
            p = self.split_operands(ops)
            return f"{m}\t{self.operand(p[0])},#{num(int(p[1], 0))},{self.label(int(p[2], 0))}"
        if mnem in ("BSET", "BCLR"):
            p = self.split_operands(ops)
            return f"{m}\t{self.operand(p[0])},#{num(int(p[1], 0))}"
        if mnem in ("MOVB", "MOVW"):
            p = self.split_operands(ops)
            if len(p) != 2:
                raise SystemExit(f"asmgen: {a:04x} {text}")
            src = self.operand(p[0], 2 if word else 1, code=False)
            if p[0].startswith("#"):
                v = int(p[0][1:], 0)
                src = "#" + (self.label(v) if self.inside(v) and v in self.labels else num(v))
            return f"{m}\t{src},{self.operand(p[1], 2 if word else 1)}"
        # single operand
        if ops.startswith("#"):
            v = int(ops[1:], 0)
            if self.inside(v):
                return f"{m}\t#{self.label(v)}"
            return f"{m}\t#{num(v)}"
        return f"{m}\t{self.operand(ops, 2 if word else 1)}"

    # module --------------------------------------------------------------------------------

    def collect_labels(self):
        """Branch and jump targets inside the module, and code pointers in data gaps."""
        for a in self.code_addrs():
            raw, mnem, text = self.insns[a]
            for v in re.findall(r"0x[0-9a-f]+", text):
                t = int(v, 16)
                if self.inside(t) and (t in self.insns or mnem in ("LDD", "ADDD", "LDX", "LDY", "CPX", "CPY")):
                    if t in self.insns or self.gap_start(t):
                        self.label(t)
        for lo, hi in self.gaps():
            for p in range(lo, hi - 1, 2):
                v = self.word(p)
                if self.inside(v) and v in self.insns:
                    self.label(v)

    def gap_start(self, a):
        return any(lo == a for lo, _ in self.gaps())

    def word(self, a):
        return self.image[a] << 8 | self.image[a + 1]

    def gaps(self):
        if hasattr(self, "_gaps"):
            return self._gaps
        out = []
        for lo, hi in self.ranges:
            a = lo
            while a < hi:
                if a in self.insns:
                    a += len(self.insns[a][0])
                    continue
                s = a
                while a < hi and a not in self.insns:
                    a += 1
                out.append((s, a))
        self._gaps = out
        return out

    def generate(self, title):
        self.collect_labels()
        lines = [f"; {title}", "; Generated by tools/asmgen.py from the original firmware; see that script.", "",
                 "\t.include\t\"gcc/mc9s12d64.inc\"", ""]
        for g in self.globals:
            lines.append(f"\t.globl\t{g}")
        body = []
        for lo, hi in self.ranges:
            body.append("")
            body.append("\t.sect\t.text")
            a = lo
            while a < hi:
                if a in self.labels:
                    lab = self.labels[a]
                    if not lab.startswith(".L"):
                        body.append("")
                    body.append(f"{lab}:")
                if a in self.insns:
                    raw, mnem, text = self.insns[a]
                    body.append(f"\t{self.convert(a, raw, mnem, text)}")
                    a += len(raw)
                    continue
                # data
                end = a + 1
                while end < hi and end not in self.insns and end not in self.labels:
                    end += 1
                n = end - a
                words = n % 2 == 0 and all(self.inside(self.word(p)) and self.word(p) in self.labels
                                           for p in range(a, end, 2))
                if words:
                    for p in range(a, end, 2):
                        body.append(f"\t.word\t{self.labels[self.word(p)]}")
                else:
                    for p in range(a, end, 8):
                        chunk = self.image[p:min(end, p + 8)]
                        body.append("\t.byte\t" + ",".join(f"0x{b:02x}" for b in chunk))
                a = end
        self.lines = lines + body
        return "\n".join(self.lines) + "\n"

    def defsyms(self):
        return {n: a for n, a in self.externals.items()}


def tool(*args, capture=False):
    """Run a toolchain program, in the container (tools/hc12-gcc/run) unless it is on PATH;
    paths are relative to the project directory."""
    import shutil
    cmd = list(args) if shutil.which(args[0]) else [os.path.join(ROOT, "tools", "hc12-gcc", "run"), *args]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=capture, text=True)
    if r.returncode:
        raise SystemExit(f"asmgen: {' '.join(args)} failed")
    return r.stdout


def verify(desc, src):
    """Assemble src with the externals at their original addresses, one link per range."""
    mod = Module(desc)
    mod.generate("verify")
    work = "build/asmgen"
    os.makedirs(os.path.join(ROOT, work), exist_ok=True)
    obj = f"{work}/m.o"
    tool("m68hc11-as", "-m68hcs12", "-I", ".", "-o", obj, os.path.relpath(os.path.abspath(src), ROOT))
    # externals referenced by the source but not defined in it
    nm = tool("m68hc11-nm", obj, capture=True)
    undefined = [l.split()[-1] for l in nm.splitlines() if l.split()[0] == "U"]
    _, ram, regs = load_names()
    syms = {}
    names = {n: a for a, n in load_names()[0].items()}
    ram_by_name = ram_variables()
    for u in undefined:
        if u in names:
            syms[u] = names[u]
        elif u in ram_by_name:
            syms[u] = ram_by_name[u]
        else:
            raise SystemExit(f"verify: no original address for {u}")
    lo = min(a for a, _ in mod.ranges)
    hi = max(b for _, b in mod.ranges)
    if len(mod.ranges) != 1:
        raise SystemExit("verify: one range per module")
    elf = f"{work}/m.elf"
    args = ["m68hc11-ld", "-m", "m68hc12elf", f"-Ttext=0x{lo:x}", "-o", elf, obj]
    for n, a in syms.items():
        args.append(f"--defsym={n}=0x{a:x}")
    tool(*args)
    binf = f"{work}/m.bin"
    tool("m68hc11-objcopy", "-O", "binary", "-j", ".text", elf, binf)
    got = open(os.path.join(ROOT, binf), "rb").read()
    want = mod.image[lo:hi]
    if got == want:
        print(f"{src}: identical to the original 0x{lo:04X}-0x{hi - 1:04X} ({len(got)} bytes)")
        return 0
    n = min(len(got), len(want))
    first = next((i for i in range(n) if got[i] != want[i]), n)
    print(f"{src}: differs from the original: {len(got)} bytes vs {len(want)}, first difference at "
          f"0x{lo + first:04X} (built {got[first:first + 6].hex()} original {want[first:first + 6].hex()})")
    return 1


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "regs":
        regs_inc()
        return 0
    if len(sys.argv) == 4 and sys.argv[1] == "gen":
        desc = json.load(open(sys.argv[2]))
        mod = Module(desc)
        text = mod.generate(desc.get("title", os.path.basename(sys.argv[3])))
        open(sys.argv[3], "w").write(text)
        return 0
    if len(sys.argv) == 4 and sys.argv[1] == "verify":
        return verify(json.load(open(sys.argv[2])), sys.argv[3])
    print(__doc__)
    return 2


sys.exit(main())
