#!/usr/bin/env python3
"""Compare the objects of two builds function by function, for changes meant to leave the code
as it is (renames, types, comments, moves between files): each .text.* section's bytes and
relocations. A relocation against a symbol the linked build places in RAM or the register
block (below 0x4000) counts by its address (+ addend), so renaming a variable or reaching it
through a struct is no change; other relocations count by symbol name + addend.

  cp -a build before                        # the build before the change
  tools/hc12-gcc/run make                   # the build after it
  python3 tools/objcmp.py before build [RENAMES.tsv]

RENAMES.tsv: old<TAB>new function names, for a rename. Prints the sections that differ or are
missing, then the counts."""
import os, struct, sys


def elf(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'\x7fELF'
    be = d[5] == 2
    e = '>' if be else '<'
    shoff, = struct.unpack_from(e + 'I', d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from(e + 'HHH', d, 0x2E)
    secs = []
    for i in range(shnum):
        name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(e + 'IIIIIIIIII', d, shoff + i * shentsize)
        secs.append(dict(name=name, type=typ, off=off, size=size, link=link, info=info, entsize=entsize))
    shstr = secs[shstrndx]
    def cstr(tab, o):
        s = tab['off'] + o
        return d[s:d.index(b'\0', s)].decode()
    for s in secs:
        s['n'] = cstr(shstr, s['name'])
    syms = []
    for s in secs:
        if s['type'] == 2:      # SYMTAB
            strtab = secs[s['link']]
            for k in range(s['size'] // 16):
                nm, val, size, info, other, shndx = struct.unpack_from(e + 'IIIBBH', d, s['off'] + k * 16)
                syms.append((cstr(strtab, nm), val, shndx))
    out = {}
    for i, s in enumerate(secs):
        if s['type'] == 1 and s['n'].startswith('.text'):
            out[s['n']] = [d[s['off']:s['off'] + s['size']], [], i]
    for s in secs:
        if s['type'] in (4, 9):  # RELA, REL
            tgt = secs[s['info']]['n']
            if tgt not in out:
                continue
            ent = 12 if s['type'] == 4 else 8
            for k in range(s['size'] // ent):
                if s['type'] == 4:
                    off, info, add = struct.unpack_from(e + 'IIi', d, s['off'] + k * ent)
                else:
                    off, info = struct.unpack_from(e + 'II', d, s['off'] + k * ent)
                    add = 0
                sym = syms[info >> 8]
                name = sym[0] or secs[sym[2]]['n'] if sym[2] < len(secs) else sym[0]
                out[tgt][1].append((off, info & 0xFF, name, add, sym))
    return out


def ram_addrs(symfile):
    m = {}
    for line in open(symfile):
        w = line.split()
        if len(w) == 3:
            a = int(w[0], 16)
            if a < 0x4000:
                m[w[2]] = a
    return m


def objs(build):
    res = {}
    for root, _, files in os.walk(os.path.join(build, 'obj')):
        for f in files:
            if f.endswith('.o'):
                for sec, v in elf(os.path.join(root, f)).items():
                    if not v[0] and not v[1]:
                        continue            # an empty section (a file's own .text)
                    res[sec if sec.startswith('.text.') else os.path.relpath(os.path.join(root, f), build) + ':' + sec] = v
    return res


if len(sys.argv) < 3:
    sys.exit(__doc__)
base, new = sys.argv[1], sys.argv[2]
ren = {}
if len(sys.argv) > 3:
    for line in open(sys.argv[3]):
        if line.strip() and not line.startswith('#'):
            o, n = line.split()[:2]
            ren[o] = n
ra, rb = ram_addrs(os.path.join(base, 'ms1.sym')), ram_addrs(os.path.join(new, 'ms1.sym'))
A, Bn = objs(base), objs(new)


# field size of an m68hc11 relocation type (REL: the addend is in the field)
SIZE = {1: 1, 2: 1, 3: 1, 4: 1, 5: 2, 6: 4, 7: 1, 8: 2, 11: 3, 12: 2, 13: 1}


def norm(v, ram, rename):
    data, rels, _ = v
    data = bytearray(data)
    out = []
    for off, typ, name, add, sym in sorted(rels):
        n = SIZE.get(typ, 0)
        val = int.from_bytes(data[off:off + n], 'big') if n else 0
        data[off:off + n] = bytes(n)
        if typ in (20, 21):              # relaxation markers: nothing patched
            continue
        if name in ram:
            out.append((off, typ, 'ram', ram[name] + val if typ in (5, 12, 6, 11) else (ram[name], val)))
        else:
            out.append((off, typ, rename.get(name, name), val))
    return bytes(data), out


same = diff = 0
for sec in sorted(A):
    nsec = '.text.' + ren.get(sec[6:], sec[6:]) if sec.startswith('.text.') else sec
    if nsec not in Bn:
        print('missing in new:', sec)
        diff += 1
        continue
    if norm(A[sec], ra, ren) == norm(Bn[nsec], rb, {}):
        same += 1
    else:
        diff += 1
        print('differs:', sec)
print(f'{same} sections identical, {diff} differ')
