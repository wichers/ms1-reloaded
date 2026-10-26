#!/bin/sh
# Headless Ghidra steps: the ms1_4011 program in project/ms1.gpr, and module listings.
#   ./ghidra.sh import      create ms1_4011 from build/ms1_4011.xml, auto-analyse it and redraw
#                           function boundaries (report: out/boundaries.tsv); Ghidra refuses
#                           if the program already exists in the project
#   ./ghidra.sh names       name functions and data as the firmware rebuild does (FIRMWARE, default
#                           ../ms1-firmware; ApplyNames.java)
#   ./ghidra.sh decompile   write out/ms1_4011_decomp.c, out/facts.json (calls, memory accesses)
#                           and out/listing.tsv (every instruction)
#                           from the program as it is now in the project
#   ./ghidra.sh listing RELEASE.bin MODULE_DIR OUT.tsv
#                           the instructions of the firmware rebuild's assembly modules
#                           (MODULE_DIR/*.json) in another release image, for its tools/asmgen.py:
#                           a raw import of 0x4000-0xFFFF in a throw-away project, disassembled
#                           from the modules' entries and jump-table targets (module_seeds.py,
#                           DisassembleSeeds.java), exported as out/listing.tsv is
# All need the HCS-12:BE:16:flat language installed by ghidra_lang/build.sh.
set -eu
here=$(dirname "$(readlink -f "$0")")
: "${GHIDRA_INSTALL_DIR:?set GHIDRA_INSTALL_DIR to a Ghidra 12.1.x install}"
: "${JAVA_HOME:?set JAVA_HOME to a full JDK 21}"
export JAVA_HOME
headless="$GHIDRA_INSTALL_DIR/support/analyzeHeadless"
mkdir -p "$here/project" "$here/out"

case "${1:-}" in
import)
    rm -f "$here/out/boundaries.tsv"
    exec "$headless" "$here/project" ms1 \
        -import "$here/build/ms1_4011.xml" -loader XmlLoader -processor HCS-12:BE:16:flat -cspec ms1 \
        -scriptPath "$here/ghidra_scripts" \
        -preScript AnalysisOptions.java -preScript SetupMemory.java -preScript MarkImported.java \
        -preScript DefinePointerTables.java -preScript ForceDisassembly.java \
        -postScript RedrawBoundaries.java "$here/out/boundaries.tsv" \
        -postScript MarkInterruptHandlers.java -postScript ApplyTypes.java \
        -postScript ApplySignatures.java -postScript CommitSignatures.java -postScript AnnotateFunctions.java \
        -log "$here/out/import.log" ;;
names)
    python3 -I "$here/rebuild_names.py" "${FIRMWARE:-$here/../ms1-firmware}" "$here/build/names.tsv"
    exec "$headless" "$here/project" ms1 -process ms1_4011 -noanalysis \
        -scriptPath "$here/ghidra_scripts" -postScript ApplyNames.java "$here/build/names.tsv" \
        -log "$here/out/names.log" ;;
decompile)
    exec "$headless" "$here/project" ms1 -process ms1_4011 -noanalysis -readOnly \
        -scriptPath "$here/ghidra_scripts" -postScript DecompileAll.java "$here/out/ms1_4011_decomp.c" \
        -postScript ExportFacts.java "$here/out/facts.json" \
        -postScript ExportListing.java "$here/out/listing.tsv" \
        -log "$here/out/decompile.log" ;;
listing)
    [ $# -eq 4 ] || { sed -n '2,13p' "$0" >&2; exit 2; }
    bin=$(readlink -f "$2"); out=$(readlink -f "$(dirname "$4")")/$(basename "$4")
    mkdir -p "$here/build/listing"
    python3 -I "$here/module_seeds.py" "$bin" "$3" "$here/build/listing/seeds.txt"
    rm -rf "$here/build/listing/project.gpr" "$here/build/listing/project.rep" "$here/build/listing/listing.log"
    exec "$headless" "$here/build/listing" project \
        -import "$bin" -loader BinaryLoader -loader-baseAddr 0x4000 -loader-fileOffset 0x4000 \
        -loader-length 0xC000 -processor HCS-12:BE:16:flat -cspec ms1 -noanalysis -deleteProject \
        -scriptPath "$here/ghidra_scripts" \
        -postScript DisassembleSeeds.java "$here/build/listing/seeds.txt" \
        -postScript ExportListing.java "$out" \
        -log "$here/build/listing/listing.log" ;;
*)
    sed -n '2,13p' "$0" >&2; exit 2 ;;
esac
