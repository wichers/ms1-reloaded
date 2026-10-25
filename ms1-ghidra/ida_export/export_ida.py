"""Export the analysed knowledge in an IDA database (opened headless via idalib).

Usage (pass a COPY of the .i64; the copy is opened read-only-in-spirit and closed without saving):
  <idalib python> -I export_ida.py <copy of .i64> <out dir> [ghidra IDAPro/Python/9xx/python dir]

Writes to <out dir>:
  ms1_ida.json       segments, entry points, functions, names, comments, bookmarks, local types
  ms1_types.h        all local types as C declarations
  ms1_firmware.asm   full IDA listing
  ms1_firmware.idc   IDC script that recreates the database annotations
  ms1_firmware.xml   Ghidra XML <PROGRAM> document (only if the Ghidra exporter dir is given)
  ms1_firmware.bytes memory contents referenced by the XML; keep it next to the XML
"""
import json, os, shutil, sys
import idapro

db, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
ghidra_py = sys.argv[3] if len(sys.argv) > 3 else None
if idapro.open_database(db, run_auto_analysis=False) != 0:
    sys.exit("open failed")
import idautils, idc, ida_bytes, ida_funcs, ida_ida, ida_idp, ida_moves, ida_name, ida_segment, ida_typeinf

os.makedirs(out, exist_ok=True)


def extra_lines(ea, base):
    lines, i = [], 0
    while (line := idc.get_extra_cmt(ea, base + i)) is not None:
        lines.append(line)
        i += 1
    return lines


segments = []
for s in idautils.Segments():
    seg = ida_segment.getseg(s)
    segments.append(dict(name=ida_segment.get_segm_name(seg), start=seg.start_ea, end=seg.end_ea,
                         cls=ida_segment.get_segm_class(seg), bitness=seg.bitness, perm=seg.perm,
                         comment=ida_segment.get_segment_cmt(seg, False) or ""))

functions = []
for ea in idautils.Functions():
    f = ida_funcs.get_func(ea)
    functions.append(dict(
        ea=ea, end=f.end_ea, name=ida_funcs.get_func_name(ea),
        user_named=ida_bytes.has_user_name(ida_bytes.get_flags(ea)),
        prototype=idc.get_type(ea) or "",
        noreturn=bool(f.flags & ida_funcs.FUNC_NORET), library=bool(f.flags & ida_funcs.FUNC_LIB),
        thunk=bool(f.flags & ida_funcs.FUNC_THUNK),
        chunks=[list(c) for c in idautils.Chunks(ea)],
        comment=ida_funcs.get_func_cmt(f, False) or "", repeatable_comment=ida_funcs.get_func_cmt(f, True) or ""))
func_starts = {f["ea"] for f in functions}

names = []
for ea, name in idautils.Names():
    if ea in func_starts:
        continue
    fl = ida_bytes.get_flags(ea)
    names.append(dict(ea=ea, name=name, user_named=ida_bytes.has_user_name(fl), code=ida_bytes.is_code(fl),
                      size=ida_bytes.get_item_size(ea), type=idc.get_type(ea) or ""))

comments = []
for seg in segments:
    ea = seg["start"]
    while ea != idc.BADADDR and ea < seg["end"]:
        entry = dict(ea=ea, regular=idc.get_cmt(ea, False) or "", repeatable=idc.get_cmt(ea, True) or "",
                     anterior=extra_lines(ea, idc.E_PREV), posterior=extra_lines(ea, idc.E_NEXT))
        if entry["regular"] or entry["repeatable"] or entry["anterior"] or entry["posterior"]:
            comments.append(entry)
        ea = idc.next_head(ea, seg["end"])

bookmarks = []
for slot in range(1, 1025):
    ea = idc.get_bookmark(slot)
    if ea not in (None, idc.BADADDR):
        bookmarks.append(dict(slot=slot, ea=ea, description=idc.get_bookmark_desc(slot) or ""))

til = ida_typeinf.get_idati()
types = []
for ordinal in range(1, ida_typeinf.get_ordinal_limit(til)):
    tif = ida_typeinf.tinfo_t()
    if not tif.get_numbered_type(til, ordinal):
        continue
    entry = dict(ordinal=ordinal, name=tif.get_type_name() or "", size=tif.get_size() if tif.get_size() != idc.BADADDR else None,
                 kind="struct" if tif.is_struct() else "union" if tif.is_union() else "enum" if tif.is_enum()
                 else "typedef" if tif.is_typedef() else "func" if tif.is_func() else "other",
                 decl=tif._print(None, ida_typeinf.PRTYPE_DEF | ida_typeinf.PRTYPE_MULTI | ida_typeinf.PRTYPE_TYPE))
    if tif.is_udt():
        udt = ida_typeinf.udt_type_data_t()
        tif.get_udt_details(udt)
        entry["members"] = [dict(name=m.name, offset=m.offset // 8, bit_size=m.size, type=str(m.type), comment=m.cmt or "")
                            for m in udt]
    elif tif.is_enum():
        edt = ida_typeinf.enum_type_data_t()
        tif.get_enum_details(edt)
        entry["members"] = [dict(name=m.name, value=m.value, comment=m.cmt or "") for m in edt]
    types.append(entry)

summary = dict(functions=len(functions), user_named_functions=sum(f["user_named"] for f in functions),
               functions_with_prototype=sum(bool(f["prototype"]) for f in functions),
               names=len(names), user_named_names=sum(n["user_named"] for n in names),
               commented_addresses=len(comments), bookmarks=len(bookmarks), local_types=len(types),
               structs=sum(t["kind"] == "struct" for t in types), enums=sum(t["kind"] == "enum" for t in types))
json.dump(dict(source_idb=os.path.basename(db), processor=ida_idp.get_idp_name(),
               min_ea=ida_ida.inf_get_min_ea(), max_ea=ida_ida.inf_get_max_ea(),
               entry_points=[dict(ordinal=o, ea=ea, name=n) for _, o, ea, n in idautils.Entries()],
               summary=summary, segments=segments, functions=functions, names=names, comments=comments,
               bookmarks=bookmarks, types=types),
          open(os.path.join(out, "ms1_ida.json"), "w"), indent=1)

with open(os.path.join(out, "ms1_types.h"), "w") as h:
    h.write("/* Local types exported from %s */\n\n" % os.path.basename(db))
    h.write(idc.print_decls(",".join(str(t["ordinal"]) for t in types),
                            idc.PDF_INCL_DEPS | idc.PDF_DEF_FWD | idc.PDF_DEF_BASE))

idc.gen_file(idc.OFILE_ASM, os.path.join(out, "ms1_firmware.asm"), ida_ida.inf_get_min_ea(), ida_ida.inf_get_max_ea(), 0)
idc.gen_file(idc.OFILE_IDC, os.path.join(out, "ms1_firmware.idc"), ida_ida.inf_get_min_ea(), ida_ida.inf_get_max_ea(), 0)

if ghidra_py:
    sys.path.insert(0, ghidra_py)
    import idaxml
    # The version banner insists idaxml.py lives in IDA's own python dir; it only prints, so skip it.
    idaxml.XmlExporter.display_version = lambda self, what: None
    idaxml.XmlExporter(1).export_xml()  # autorun: writes <idb without ext>.xml plus the .bytes it references
    stem = os.path.splitext(db)[0]
    shutil.move(stem + ".xml", os.path.join(out, "ms1_firmware.xml"))
    shutil.move(stem + ".bytes", os.path.join(out, os.path.basename(stem) + ".bytes"))

print(json.dumps(summary))
idapro.close_database(save=False)
