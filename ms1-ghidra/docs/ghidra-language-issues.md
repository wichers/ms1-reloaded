# Ghidra HCS12 language issues

Defects in Ghidra's `HCS12` processor module (`Ghidra/Processors/HCS12/data/languages/HCS_HC12.sinc`, Ghidra 12.1.x) found while analysing this firmware, written as upstream issue reports. `ghidra_lang/build.sh` patches the first two and works around the others in the `HCS-12:BE:16:flat` variant.

## 1. `BRSET` branches on the inverted condition (all HCS12 variants)

**Affects:** `HC-12:BE:16:default`, `HCS-12:BE:24:default`, `HCS-12X:BE:24:default` — the constructors are shared.

**Expected:** CPU12 `BRSET`: branch if `(¬M) · mm = 0`, i.e. when every mask bit is set in the operand.

**Actual:** all three `BRSET` constructors (`opr8a_8`, `opr16a_8`, `indexed1_3`) compute `result = ~operand & msk8` and branch on `result != 0` — when at least one mask bit is clear.

```
:BRSET opr8a_8, msk8, rel8    is Prefix18=0 & op8=0x4E; opr8a_8; msk8; rel8
{
	result:1 = ~opr8a_8 & msk8;
	if (result != 0) goto rel8;     # should be: if (result == 0) goto rel8;
}
```

**Repro:** `0E 5E 10 10` (`BRSET -2,Y,#$10,*+$14`, at `0xC678` in this firmware): with bit 4 of `-2,Y` set, the CPU branches; the decompiler shows the fall-through code executed in that case. `BRCLR` (branch on `operand & msk8 == 0`) is correct.

**Fix:** `if (result == 0) goto rel8;` in the three `BRSET` constructors.

## 2. `HC-12:BE:16`: indexed operands are exported as addresses, not memory

**Affects:** `HC-12:BE:16:default` (the `@else` branch of the `@ifdef HCS12` blocks).

**Actual:** in the non-HCS12 branch the indexed operand tables export the computed address itself, e.g.

```
indexed2_5: indexed5 is UseGPAGE=0 & indexed5 {
	export indexed5;          # the HCS12 branch exports *:2 paddr
}
```

Loads and stores through indexed operands therefore read or write the address value instead of memory. With a zero offset the operand is the index register itself: `6C 80` (`STD 0,SP`) becomes p-code `SP = COPY D`, which corrupts the decompiler's stack tracking for every function that touches the top of its frame.

**Expected:** `export *:2 indexed5;` (and the corresponding sizes for the other indexed tables), as the HCS12 branch does through `GetPagedAddr`.

## 3. The HCS12 SLEIGH does not compile at 16-bit address size

**Affects:** anyone building an HCS12 (not HCS12X) variant with `@define SIZE "2"` — the natural model for MC9S12 parts used without global addressing, whose firmware sees only the 16-bit CPU address space.

**Actual:** `page` and `PageDest`, used by `CallDest` (`CALL`), are defined only in the `@if SIZE=="3"` branch; compiling with `HCS12` and `SIZE 2` fails with `No matching global symbol 'PageDest'` / `'page'`.

**Fix:** define both in the `@elif SIZE=="2"` branch as well:

```
page:		    imm8     is imm8       { export *[const]:1 imm8; }
PageDest: opr16a   is opr16a; page  { export opr16a; }
```

## 4. `HCS-12:BE:24` maps low addresses into page 3D's global range

**Affects:** `HCS-12:BE:24:default`.

**Actual:** for `SIZE 3`, `setHCSphysPage` maps CPU addresses `0x0000–0x3FFF` to the global range of flash page `$3D` (`0xF4000 + addr`), so on an MC9S12D64 — registers at `0x0000`, RAM at `0x1000` — every RAM access aliases onto the global addresses of paged flash page 3D, and jumps into the `0x8000–0xBFFF` window depend on a `PPAGE` value the decompiler cannot know (reported as reads of `RAM:0030`). Whether this mapping suits other MC9S12 members was not checked; for this one it makes the 24-bit language unusable.

## 5. `HC-12`/`HCS-12` at 16-bit: extended `JSR`/`JMP` are indirect flows

**Affects:** the `SIZE 2` branch.

**Actual:** `opr16a` and `opr8a` export a computed value, so `JSR opr16a` becomes `call [opr16a]` (`COMPUTED_CALL`) and `JMP opr16a` becomes `goto [opr16a]` (`COMPUTED_JUMP`). Their targets exist only once constant propagation runs inside an existing function; calls from code not yet in a function produce no reference, so their targets look uncalled.

**Suggestion:** a direct-target operand for these forms, e.g. `dest16: imm16 is imm16 { export *:1 imm16; }` with `goto dest16;` / `call dest16;`.
