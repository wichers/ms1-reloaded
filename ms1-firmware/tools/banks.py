"""The fixed-flash code of the application, and the addresses of the page-$3C entry table.

Page $3D sits in the window 0x8000-0xBFFF except while the application runs code in page $3C
(include/page3c.h): a CALL maps page $3C there, with interrupts on. Code that may run then
must lie in the fixed pages ($3E 0x4000-0x7FFF, $3F from 0xC000): the interrupt handlers (the
targets of the trampolines, section .tramp, but for the reset's: the start-up code, .crt0,
runs before any page switch), the application's functions that page-$3C code calls, and
everything these call or read, followed through the objects' relocations.

place    reads the application's and page $3C's objects (objdump -h -r -t) and writes the
         linker script template (from tools/ramlayout.py) with @FIXED_TEXT@ filled in: those
         functions' sections, kept and placed first from 0x4000 on (gcc/ms1.ld checks that
         they end below the window and that read-only data starts above it).
entries  writes the application's symbols of the page-$3C entry table: each name's JMP at
         0x8000 + offset in the linker's banked form 0x100000 + offset, so GCC's far call
         carries page $3C (src/sys/page3c_entries.c).

Usage: python3 tools/banks.py place OBJDUMP TEMPLATE --page3c OBJ... --app OBJ... > build/ms1.ld
       python3 tools/banks.py entries OBJDUMP build/obj/sys/page3c_entries.o > build/page3c_symbols.ld
"""
import re
import subprocess
import sys

MARKER = "/* @FIXED_TEXT@ */"
BANKED_PAGE3C = 0x100000        # the linker's banked address of page $3C, 0x8000
ENTRY_SECTION = ".page3c_entry"


class Obj:
    def __init__(self, path):
        self.path = path
        self.sections = {}      # name -> size
        self.defined = {}       # symbol -> section
        self.globals = set()    # defined global symbols
        self.relocs = {}        # section -> [(offset, type, symbol)]


def objdump(tool, paths, opts):
    """the objects of paths with their sections, symbols and relocations"""
    text = subprocess.run([tool] + opts + paths, check=True, capture_output=True, text=True).stdout
    objs, obj, relocs = {}, None, None
    for line in text.splitlines():
        m = re.match(r"(\S+):\s+file format", line)
        if m:
            obj = objs[m.group(1)] = Obj(m.group(1))
            relocs = None
            continue
        m = re.match(r"RELOCATION RECORDS FOR \[(\S+)\]:", line)
        if m:
            relocs = obj.relocs.setdefault(m.group(1), [])
            continue
        if relocs is not None:
            m = re.match(r"([0-9a-f]{8}) (\S+)\s+(\S+)$", line)
            if m:
                relocs.append((int(m.group(1), 16), m.group(2), m.group(3).split("+")[0]))
            continue
        m = re.match(r"\s*\d+ (\S+)\s+([0-9a-f]{8}) ", line)
        if m:
            obj.sections[m.group(1)] = int(m.group(2), 16)
            continue
        m = re.match(r"[0-9a-f]{8} (.)(.{6}) (\S+)\t[0-9a-f]+ (\S+)$", line)
        if m and m.group(3) not in ("*UND*", "*ABS*", "*COM*") and "d" not in m.group(2):
            obj.defined[m.group(4)] = m.group(3)
            if m.group(1) == "g":
                obj.globals.add(m.group(4))
    return [objs[p] for p in paths]


def resolve(obj, symbol, glob):
    """(object, section) a relocation in obj refers to, or None outside these objects"""
    if symbol in obj.sections:
        return obj, symbol
    if symbol in obj.defined:
        return obj, obj.defined[symbol]
    return glob.get(symbol)


def global_table(objs):
    table = {}
    for o in objs:
        for s in o.globals:
            table[s] = (o, o.defined[s])
    return table


def place(tool, template_path, page3c, app):
    apps = objdump(tool, app, ["-h", "-r", "-t"])
    p3cs = objdump(tool, page3c, ["-h", "-r", "-t"])
    app_glob, p3c_glob = global_table(apps), global_table(p3cs)

    # the roots: the trampolines' targets, and what page-$3C code calls in the application
    roots = []
    for o in apps:
        for _, _, sym in o.relocs.get(".tramp", []):
            node = resolve(o, sym, app_glob)
            if node and node[1] != ".crt0":
                roots.append((node, "interrupt"))
    for o in p3cs:
        for sec, rel in o.relocs.items():
            if sec.startswith(".debug"):
                continue
            for _, _, sym in rel:
                if resolve(o, sym, p3c_glob) is None and sym in app_glob:
                    roots.append((app_glob[sym], "page $3C"))

    reason, todo = {}, list(roots)
    while todo:
        (o, sec), why = todo.pop()
        if (o.path, sec) in reason:
            continue
        reason[(o.path, sec)] = why
        for _, _, sym in o.relocs.get(sec, []):
            node = resolve(o, sym, app_glob)
            if node:
                todo.append((node, why))

    lines, size = [], 0
    for o in apps:
        for sec in o.sections:
            why = reason.get((o.path, sec))
            if why and sec.startswith(".text.") and o.sections[sec]:
                lines.append(f"    KEEP ({o.path}({sec}))   /* {why} */")
                size += o.sections[sec]
    lines.append(f"    /* {len(lines)} sections, {size} bytes */")

    template = open(template_path).read()
    if MARKER not in template:
        sys.exit(f"banks: {MARKER} missing from {template_path}")
    sys.stdout.write(template.replace(MARKER, "\n".join(lines).lstrip()))


def entries(tool, path):
    obj = objdump(tool, [path], ["-r"])[0]
    out = []
    for offset, kind, sym in obj.relocs.get(ENTRY_SECTION, []):
        if kind == "R_M68HC12_16":
            if (offset - 1) % 3:
                sys.exit(f"banks: {sym}: not a 3-byte JMP in {ENTRY_SECTION}")
            out.append(f"{sym} = {BANKED_PAGE3C + offset - 1:#x};")
    print("/* The page-$3C entry table (tools/banks.py entries) */")
    print("\n".join(out))


def main():
    args = sys.argv[1:]
    if args[:1] == ["entries"] and len(args) == 3:
        entries(args[1], args[2])
    elif args[:1] == ["place"] and len(args) >= 4 and "--page3c" in args and "--app" in args:
        i, j = args.index("--page3c"), args.index("--app")
        page3c = args[i + 1:j] if i < j else args[i + 1:]
        app = args[j + 1:i] if j < i else args[j + 1:]
        place(args[1], args[2], page3c, app)
    else:
        sys.exit(__doc__)


main()
