#!/bin/bash
# Build the HCS-12:BE:16:flat language as a Ghidra extension from the installed Ghidra's
# HCS12 sources, and install it into Ghidra settings directories.
#   ./build.sh [settings dir ...]
# Default target: the settings dir that Ghidra run outside the snap uses
# (~/.config/ghidra/ghidra_<version>_<release>). The zip in build/ext installs through the
# GUI's File > Install Extensions.
set -eu
here=$(dirname "$(readlink -f "$0")")
: "${GHIDRA_INSTALL_DIR:?set GHIDRA_INSTALL_DIR to a Ghidra 12.1.x install}"
: "${JAVA_HOME:?set JAVA_HOME to a full JDK 21}"
export JAVA_HOME
props="$GHIDRA_INSTALL_DIR/Ghidra/application.properties"
version=$(sed -n 's/^application.version=//p' "$props")
release=$(sed -n 's/^application.release.name=//p' "$props")
src="$GHIDRA_INSTALL_DIR/Ghidra/Processors/HCS12/data/languages"
out="$here/../build/ext"
ext="$out/HCS12_16"
lang="$ext/data/languages"

rm -rf "$ext"
mkdir -p "$lang"
cp "$src/HCS_HC12.sinc" "$src/XGATE.sinc" "$lang/"
cp "$src/HC12.pspec" "$lang/HCS12_16.pspec"   # HC12's pspec: no 24-bit segmentop
cp "$src/HC12.cspec" "$lang/HCS12_16.cspec"   # HC12's cspec: JSR/RTS model (extrapop 2); HCS12's
                                              # defaults to the CALL/RTC one (extrapop 3)
cp "$here/HCS12_16.slaspec" "$here/HCS12_16.ldefs" "$lang/"

# SIZE 2 patches:
# - define the CALL operands that only the SIZE 3 branch defines;
# - give extended/direct JMP and JSR a direct target. opr16a/opr8a export a computed value, so
#   these became indirect flows whose targets only analysis inside a function could recover.
python3 -I - "$lang/HCS_HC12.sinc" <<'EOF'
import re, sys
path = sys.argv[1]
src = open(path).read()

def once(pattern, repl, text):
    out, n = re.subn(pattern, repl, text)
    if n != 1:
        sys.exit(f"HCS_HC12.sinc: expected one match for {pattern!r}, found {n}; this Ghidra's sources differ")
    return out

anchor = re.escape('@elif SIZE=="2"\nopr16a:          imm16    is  imm16     {  local addr:$(SIZE) = imm16; export addr; }\n')
src = once(anchor, lambda m: m[0] +
           "page:\t\t    imm8     is imm8       { export *[const]:1 imm8; }\n"
           "PageDest: opr16a   is opr16a; page  { export opr16a; }\n"
           "dest16:          imm16    is  imm16     { export *:1 imm16; }\n"
           "dest8:           imm8     is  imm8      { export *:1 imm8; }\n", src)
src = once(r":JMP opr16a(\s+)is Prefix18=0 & \(op8=0x06\); opr16a\s*\{\s*goto \[opr16a\];\s*\}",
           r":JMP dest16\1is Prefix18=0 & (op8=0x06); dest16\n{\n\tgoto dest16;\n}", src)
for op, operand, dest in (("0x16", "opr16a", "dest16"), ("0x17", "opr8a", "dest8")):
    src = once(rf":JSR {operand}(\s+)is Prefix18=0 & \(op8={op}\); {operand}(\s*\{{[^}}]*?)call \[{operand}\];",
               rf":JSR {dest}\1is Prefix18=0 & (op8={op}); {dest}\2call {dest};", src)
# BRSET branches when every mask bit is set, i.e. (~M & mask) == 0; the stock semantics branch
# on != 0, inverting every BRSET.
src, n = re.subn(r"(:BRSET [^\n]*\n\{\s*result:1 = ~[^;]*& msk8;\s*if \(result )!=( 0\) goto rel8;)", r"\1==\2", src)
if n != 3:
    sys.exit(f"HCS_HC12.sinc: expected 3 BRSET constructors, patched {n}; this Ghidra's sources differ")
open(path, "w").write(src)
EOF

# An interrupt handler is entered with CCR, B, A, X, Y and the return PC (9 bytes) stacked and
# leaves by RTI; without its own model the decompiler reads those as the caller's stack.
python3 -I - "$lang/HCS12_16.cspec" <<'EOF'
import sys
path = sys.argv[1]
src = open(path).read()
anchor = "  </default_proto>\n"
if src.count(anchor) != 1:
    sys.exit("HCS12_16.cspec: default_proto anchor not found; this Ghidra's sources differ")
proto = ('  <prototype name="__interrupt" extrapop="9" stackshift="9">\n'
         '    <input/>\n    <output/>\n'
         '    <unaffected>\n      <register name="SP"/>\n    </unaffected>\n'
         '  </prototype>\n')
open(path, "w").write(src.replace(anchor, anchor + proto))
EOF

# Compiler spec "ms1": this firmware keeps a frame pointer in RAM, ui_context_ptr at 0x1BBD
# (MOVW 0x1BBD,2,-SP; LEAS -n,SP; STS 0x1BBD on entry, MOVW 2,SP+,0x1BBD before returning),
# so every call preserves it: listing it as unaffected in the default prototype lets the
# decompiler carry ctx == SP across calls and print frame accesses as locals. In functions that
# loop over pointer stores it still cannot prove the stores miss 0x1BBD and keeps reloading it.
python3 -I - "$lang/HCS12_16.cspec" "$lang/HCS12_16_ms1.cspec" <<'EOF'
import sys
src = open(sys.argv[1]).read()
def once(old, new):
    global src
    if src.count(old) != 1:
        sys.exit(f"HCS12_16.cspec: {old!r} not found once; this Ghidra's sources differ")
    src = src.replace(old, new)
once('      <unaffected>\n        <register name="SP"/>\n      </unaffected>\n    </prototype>\n  </default_proto>',
     '      <unaffected>\n        <register name="SP"/>\n        <varnode space="RAM" offset="0x1bbd" size="2"/>\n'
     '      </unaffected>\n    </prototype>\n  </default_proto>')
open(sys.argv[2], "w").write(src)
EOF

"$GHIDRA_INSTALL_DIR/support/sleigh" "$lang/HCS12_16.slaspec" >/dev/null
touch "$ext/Module.manifest"
cat > "$ext/extension.properties" <<EOF
name=HCS12_16
description=HCS-12:BE:16:flat - HCS12 (MC9S12) with 16-bit CPU-visible addresses
author=ms1-ghidra
createdOn=
version=$version
EOF
(cd "$out" && rm -f "ghidra_${version}_HCS12_16.zip" && python3 -I -m zipfile -c "ghidra_${version}_HCS12_16.zip" HCS12_16)

targets=("$@")
[ ${#targets[@]} -gt 0 ] || targets=("$HOME/.config/ghidra/ghidra_${version}_${release}")
for settings in "${targets[@]}"; do
    rm -rf "$settings/Extensions/HCS12_16"
    mkdir -p "$settings/Extensions"
    cp -r "$ext" "$settings/Extensions/HCS12_16"
    echo "installed into $settings/Extensions/HCS12_16"
done
