# ms1-ghidra — Ghidra analysis of the MS1 application firmware

Ghidra workspace for the Märklin Mobile Station 1 application image `4011_1-8_mobile-station-2_A` (MC9S12D64 / HCS12), seeded with the annotations from the IDA attempt in `../ms1-reverse`. It is the analysis base for an open-source DCC controller derived from this firmware.

- **Program:** `ms1_4011` in the Ghidra project `project/ms1.gpr`
- **Pseudocode:** `out/ms1_4011_decomp.c` — every function in one file, headed `/* ===== <name> @ <address> ===== */`; failures are marked in place
- **Ghidra language issues:** [`docs/ghidra-language-issues.md`](docs/ghidra-language-issues.md) — the HCS12 SLEIGH defects this project patches, written as upstream reports
- **Track signal:** [`docs/track-signal.md`](docs/track-signal.md) — timer/pin setup, staging-entry format, MM and mfx encodings, what a DCC generator can reuse
- **Firmware map:** [`docs/firmware-map.md`](docs/firmware-map.md) — memory map, interrupt vectors → handlers, peripheral registers → functions, shared state; per-function detail in [`docs/function-index.md`](docs/function-index.md)
- **Boundary report:** `out/boundaries.tsv` — every function merged, kept, promoted or turned into a tail call, with the evidence
- **IDA export:** `ida_export/` — portable copy of everything the IDA database knows

REA (the `rea` CLI/MCP) is not used: its target admission rejects HCS12 in every form (S-record, raw image, ELF with `EM_68HC12`) before any engine runs. Ghidra is driven directly instead.

## Provenance and trust

| Content | Source |
|---|---|
| Bytes `0x4000–0xF7FF` (pages 3E, 3D, 3F) and `FLASH_3C` | `../original-firmware/4011_1-8_mobile-station-2_A.bin` (decoded from the `.bci` by `../tools/bci.py`) |
| Bytes `0xF800–0xFFFF` (bootloader, vector table) | the device dump in the IDA database — a BCI image carries no bootloader |
| Names, comments, structs, data definitions | the firmware rebuild (`./ghidra.sh names`: names checked against this program by difftest) where it has one; elsewhere the IDA attempt — **hints, not ground truth** |
| Memory map, function boundaries, pointer tables | derived in Ghidra from the code (see below); IDA's segments and splits are not kept |

- IDA-derived symbols have source type **Imported** (`MarkImported.java`); names and signatures set in Ghidra — in the GUI or by the curated tables in `ApplySignatures.java` — are **User Defined**, so verified work stays distinguishable; decompiler-derived signatures are **Analysis**. Labels of merged fragments keep their IDA name.
- IDA's per-segment register defaults (`ppage`/`ds` = 0) are placeholders, not observations; `compose_4011.py` drops them so `PPAGE` is not pinned to 0.
- IDA mistakes corrected on import, each listed with its evidence in the script that fixes it: data kept as code (`DefinePointerTables.java` `DATA_REGIONS`), code-pointer tables disassembled as code or unbounded (`TABLES`), the cross-page instruction at `0xBFFF` (`ForceDisassembly.java`), IDA's RAM/EEPROM segments (`SetupMemory.java`), and its paged-code segment and empty window segment, both of which become page 3D at `0x8000` (`compose_4011.py`).
- Where the code contradicts an imported name, `ApplyTypes.java` and `AnnotateFunctions.java` replace it and record the evidence (the IDA name stays as a secondary label), e.g. the CAN rings' read/write pointers, imported with RX and TX swapped, and the CAN0 transmit handler, imported as `can_rx_overflow_and_version`.
- IDA names are not evidence of behaviour, e.g. `p3d_lok_db_page_switch` (0xAC8E, `lcd_mfx_icon`) switches no page (it sets the mfx icon); `p3d_lok_db_parse_record`, `…_decode_address`, `…_decode_name`, `…_ptr_lookup` are EEPROM write helpers (sector-aligned compare, `eeprom_erase_sector`, `eeprom_program_word`); `lok_db_search_index` is a ramp state word; IDA's own comments call `0xE0A4` the default interrupt handler, but it is the string "End of .text" that the 56 unused vectors' trampolines jump to (its IDA name was right).
- `compose_4011.py` prints, per memory block, how many image bytes differ from the device dump. The application differs only in release metadata in `FLASH_3F` (build-date string, article number, the product/CRC words at `0xF700–0xF73F`).

## Memory model

The program is the 16-bit address space as the application sees it. The application maps only page 3D into the `PPAGE` window, so page 3D sits at plain `0x8000–0xBFFF`, contiguous with the fixed pages; page 3C, which only the bootloader maps, is a data-only overlay.

| Ghidra block | Address | Content | Permissions | IDA address |
|---|---|---|---|---|
| `FSR` | `0x0000–0x03FF` | I/O registers (`INITRG = $00`) | rw, volatile | same |
| `EEPROM` | `0x0800–0x0BFF` | EEPROM, 1 KB (`INITEE = $09`) | rw | inside IDA's `RAM_` |
| `RAM` | `0x1000–0x1FFF` | RAM, 4 KB (`INITRM = $10`; stack top `0x2000`) | rw | inside IDA's `RESERVED` |
| `UNMAPPED_*` | `0x0400–0x07FF`, `0x0C00–0x0FFF`, `0x2000–0x3FFF` | nothing mapped | rw | IDA's `EEPROM_`, parts of `RAM_`/`RESERVED` |
| `FLASH_3E` | `0x4000–0x7FFF` | fixed flash page 3E | r-x | same |
| `FLASH_3D` | `0x8000–0xBFFF` | flash page 3D, mapped by the application's `PPAGE = $3D` | r-x | `0x3D8000–0x3DBFFF`, and IDA's empty window segment `0x8000–0xBFFF` |
| `FLASH_3C` (overlay) | `FLASH_3C::8000–bfff` | page 3C — locomotive database and its two accessors | r-- (data) | `0x3C8000–0x3CBFFF` |
| `FLASH_3F` | `0xC000–0xFF7F` | fixed flash page 3F, bootloader from `0xF800` | r-x | same |
| `VECTORS` | `0xFF80–0xFFFF` | interrupt vectors | r-- | same |

IDA `0x3D9144` is Ghidra `0x9144`.

**Which page the window shows.** `PPAGE` is written by `MOVB #$3D,PPAGE` in the application's CRT init (`0x401F`) and the bootloader's init (`0xF81F`), by `#$3C` / `#$3D` around the bootloader's CRC scan (`0xF895`, `0xF8A6`), and by the application's only two `CALL`s, `CALL $BB08,$38` and `CALL $BB0C,$38` (`0xDD01`, `0xDD0A`): the MC9S12D64 has four flash pages, so `PPAGE` bits 1-0 select the page and `$38` shows page 3C; the routine's `RTC` restores `$3D`. So the window shows page 3D except while one of the two locomotive-database accessors in page 3C runs.

**Page 3C (locomotive database).** It holds a 778-entry table (`$030A` at `FLASH_3C::8000`, then 778 record pointers) and the two accessors the application calls: `FLASH_3C::BB08` (number of records) and `FLASH_3C::BB0C` (copy a record). The `FLASH_3C` overlay is defined as data, so those two routines are not disassembled here (see *Known limitations*).

**Cross-page code.** Because page 3D and page 3F are contiguous here, as they are for the CPU, routines that cross `0xC000` need no special handling: page 3D's last routine, `str_format`, runs on past `0xBFFF` into `0xC000–0xC23D` (page 3D's last byte is the opcode of `JSR $BBEE`, whose operand is at `0xC000`), and fixed-page code branches into page 3D directly.

## The `HCS-12:BE:16:flat` language

Neither stock Ghidra HCS12 language fits this firmware:

- **`HC-12:BE:16:default`** exports indexed operands as addresses instead of memory: `STD 0,SP` becomes `SP = D`, and every indexed load/store loses its memory access.
- **`HCS-12:BE:24:default`** maps low and paged addresses to a global layout that does not match the MC9S12D64 map — RAM accesses alias onto page 3D's global range, and in-window jumps depend on a `PPAGE` value the decompiler cannot know.

`ghidra_lang/build.sh` builds a third variant from the installed Ghidra's own HCS12 sources and packages it as the Ghidra extension `HCS12_16`:

- the HCS12 SLEIGH compiled with a 16-bit address size, so the paging macros reduce to plain addresses;
- the `CALL` operands, which only the 24-bit branch defines, added;
- `BRSET` corrected: it branches when every mask bit is set, `(~M & mask) == 0`; the stock semantics branch on `!= 0`, inverting every `BRSET` condition in the decompiler output;
- extended and direct `JMP`/`JSR` given a direct target — stock they are indirect flows whose targets only analysis inside an existing function recovers, so calls from code outside a function stayed invisible;
- the HC12 processor spec (no 24-bit `segmentop`) and the HC12 `JSR`/`RTS` calling model (`extrapop` 2; the HCS12 default assumes `CALL`/`RTC`, which this firmware uses only for the two database accessors in page 3C), plus an `__interrupt` model (`stackshift`/`extrapop` 9: CCR, B, A, X, Y and the return PC are stacked on entry and pulled by `RTI`).

The language has two compiler specs: `default` (generic) and **`ms1`**, which the import uses. `ms1` adds one fact about this firmware: its RAM frame pointer `ui_context_ptr` (`0x1BBD`) is preserved by every call. Functions with a large frame save it, point it at their frame (`MOVW 0x1BBD,2,-SP; LEAS -n,SP; STS 0x1BBD`), address their locals through it and restore it before returning — the only stores to `0x1BBD` are those nine `STS` and nine restoring `MOVW`. Declared unaffected in the default prototype, the decompiler keeps `ctx == SP` across calls and prints those frame accesses as locals.

## Import pipeline

`./ghidra.sh import` runs these scripts from `ghidra_scripts/`:

| Phase | Script | What it does |
|---|---|---|
| before analysis | `AnalysisOptions.java` | disables Ghidra's *Shared Return Calls* analyzer (it re-splits functions at shared jump targets, also in the re-analysis after post-scripts) and *HCS12 Calling Convention* (needs the `CALL`/`RTC` model this language omits) |
| | `SetupMemory.java` | splits and renames the blocks below `0x4000` to the map the CRT init configures, sets the permissions from the table above, clears any code the import placed in data page `FLASH_3C` |
| | `MarkImported.java` | IDA symbols → source type Imported |
| | `DefinePointerTables.java` | clears the instructions IDA left in data regions (keeping its data definitions; remaining undefined runs become byte arrays so no analyzer disassembles them); defines code-pointer tables tied to their dispatch: jump tables get exact `COMPUTED_JUMP` references plus a decompiler switch override (entries pointing back into the table are skipped as unused indices), call and plain pointer tables get a function per target; one dispatch whose override makes the decompiler die (the rotary-encoder transition at `0xBAFC`) gets the references only. Regions and tables, with their evidence, are listed in the script. |
| | `ForceDisassembly.java` | disassembles listed instructions the import gets wrong (the cross-page `JSR $BBEE` at `0xBFFF`) |
| after analysis | `RedrawBoundaries.java` | function boundaries from control flow; writes `out/boundaries.tsv` |
| | `MarkInterruptHandlers.java` | gives each vector's handler the `__interrupt` convention (9 bytes stacked, left by `RTI`); an unused vector's trampoline, whose `JMP` lands on non-code, gets it itself and its jump becomes a call |
| | `ApplySignatures.java` | verified prototypes for functions whose parameters the decompiler misreads (listed with their evidence), applied as User Defined |
| | `ApplyTypes.java` | verified structures (`mscan_frame_t`, `can_msg_t`), the 16-bit registers the firmware accesses as words (`TCn`, `TCNT`, `MCCNT`, `ATD0DRn`, MSCAN ID/filter pairs) under their MC9S12D64 names, and verified types and names for global variables the import left as plain words — each listed with its evidence |
| | `CommitSignatures.java` | commits every other function's decompiler-derived parameters and return, in two passes, so callers show the arguments they pass |
| | `AnnotateFunctions.java` | names functions and parameters where the code shows what they are, keeping the committed parameter storage; listed with their evidence |

`./ghidra.sh names` runs `rebuild_names.py` (the firmware rebuild's `tools/functions.tsv`, `asm/*.json` names and `src/globals.c` RAM variables) and `ApplyNames.java`, which makes those names primary and User Defined, keeping an imported name as a secondary label; the listing export does not depend on names.

`./ghidra.sh decompile` runs `DecompileAll.java`, `ExportFacts.java` and `ExportListing.java` on the saved program; the summary line in `out/decompile.log` gives the decompiled/failed counts. `ExportFacts.java` writes `out/facts.json`: per function its calls, thunk target, and every load/store at a constant address (folded from the instruction's p-code, so direct-page register accesses count even where Ghidra creates no reference; register-indexed accesses are not resolved). `python3 -I firmware_map.py` turns it into `docs/`. `ExportListing.java` writes `out/listing.tsv`: every instruction outside the page-3C overlay (address, bytes, mnemonic, text), the reference the emulator's decoder check (`ms1-emu`) compares against.

## Function boundaries

`RedrawBoundaries.java` decides boundaries from evidence and records each decision in `out/boundaries.tsv` (`action`, `address`, `name`, `detail`):

- **True entry:** called by an instruction, address taken (a `DATA`/`PARAM` reference from an instruction, or from data other than a jump table — a `READ`/`WRITE` is a memory access, not an address taken), an external entry point, or the target of a thunk — each interrupt vector points at a one-`JMP` trampoline (`0xF740 + 3·n` for vector `0xFF80 + 2·n`), which Ghidra models as a thunk of its handler. Thunks get no region of their own.
- **Region** of a true function: everything reachable from its entry through non-call flow, stopping at other true entries.
- A jump already turned into a tail call (`CALL_RETURN` override) still counts as a jump: its call reference is no evidence of a call.

| Action | Meaning |
|---|---|
| `merged` | imported function start inside exactly one region — a fragment; removed as function, its name kept as label |
| `promoted` | entered only by unconditional jumps from several regions — a tail-called function (e.g. `0xE0A4`, the handler most ISR trampolines jump to) |
| `tailcall` | unconditional jump into another true entry; set to `CALL_RETURN` so the decompiler shows a call |
| `shared` | code inside several regions (entered by branches or fall-through) — kept, and inlined by the decompiler into each |
| `orphan` | no call, pointer or flow from any true entry — kept; reached only indirectly, or unused |
| `branch-into`, `fall-into` | conditional branch or fall-through into another true entry — cannot be overridden; the decompiler inlines the target |
| `untailcall` | a tail-call override whose target turned out to be a fragment — removed, the jump restored |

## Setup

Requirements: Ghidra 12.1.x, a full JDK 21, Python 3. Re-exporting from IDA additionally needs IDA 9.x with idalib.

```sh
export GHIDRA_INSTALL_DIR=<Ghidra 12.1.x install>   # e.g. the snap's .../ghidra
export JAVA_HOME=<JDK 21>

ghidra_lang/build.sh                       # build + install the HCS12_16 extension
python3 -I compose_4011.py ida_export ../original-firmware/4011_1-8_mobile-station-2_A.bin build
./ghidra.sh import                         # create project/ms1.gpr : ms1_4011, language HCS-12:BE:16:flat, compiler spec ms1
./ghidra.sh names                          # the firmware rebuild's names (FIRMWARE, default ../ms1-firmware)
./ghidra.sh decompile                      # write out/ms1_4011_decomp.c, out/facts.json, out/listing.tsv
python3 -I firmware_map.py                 # write docs/firmware-map.md and docs/function-index.md
```

- `ghidra_lang/build.sh [settings dir ...]` installs into `~/.config/ghidra/ghidra_<version>_<release>` by default (Ghidra run outside the snap, including headless). The snap's GUI keeps its settings inside the snap's user data: install `build/ext/ghidra_<version>_HCS12_16.zip` there through **File → Install Extensions**, then restart Ghidra. The project cannot be opened without the extension.
- `./ghidra.sh import` refuses when `ms1_4011` already exists. Rebuilding means deleting `project/`, which discards all work done in Ghidra.

### Re-exporting the IDA database

```sh
cp ../ms1-reverse/firmware_20260317_192757_usbdm.s19.i64 <scratch dir>/ms1_firmware.i64
<idalib python> -I ida_export/export_ida.py <scratch dir>/ms1_firmware.i64 ida_export \
    "$GHIDRA_INSTALL_DIR/Extensions/IDAPro/Python/9xx/python"
```

Always pass a copy: idalib unpacks the database next to the file it opens. `export_ida.py` writes:

| File | Content |
|---|---|
| `ms1_ida.json` | segments, entry points, functions, names, comments, bookmarks, local types with members |
| `ms1_types.h` | all local types as C declarations |
| `ms1_firmware.xml` + `ms1_firmware.bytes` | Ghidra XML `<PROGRAM>` document (Ghidra's IDA exporter) and the memory it references |
| `ms1_firmware.asm` | full IDA listing |
| `ms1_firmware.idc` | IDC script that recreates the annotations in IDA |

## Layout

```
ms1-ghidra/
├── compose_4011.py          IDA XML + 4011 image -> build/ms1_4011.{xml,bytes}
├── firmware_map.py          out/facts.json -> docs/firmware-map.md, docs/function-index.md
├── docs/                    analyses (track-signal.md, ghidra-language-issues.md); firmware-map.md, function-index.md (generated)
├── ghidra.sh                headless import / decompile
├── ghidra_lang/             HCS-12:BE:16:flat sources + build.sh
├── ghidra_scripts/          import pipeline scripts + DecompileAll.java
├── ida_export/              IDA database export + export_ida.py
├── out/                     ms1_4011_decomp.c, boundaries.tsv, facts.json, listing.tsv (tracked); *.log ignored
├── build/                   generated XML/bytes and the extension (ignored)
└── project/                 Ghidra project (ignored)
```

## Known limitations

- **`FUN_8000`:** page 3D's first bytes (`13 B4 CC 00 01`) decode as `EMUL; ANDA $CC00; MEM` and are followed by `JSR $9277; JMP $7733`; nothing references `0x8000–0x800A` (the bootloader's CRC scan only reads it). It stays an orphan, likely a page header plus a stub, and its decompilation runs on into `mcan_lokstack_sync`.
- **Frame-pointer functions with loops:** in five of the nine functions that keep their frame in `ui_context_ptr` (0x1BBD, GCC's `_.frame` soft register: `mcan_lokstack_sync`, `mcan_process`, `mcan_master_request`, `mcan_send_status`, `mcan_send_device_name`), locals still appear as `*(type *)(ui_context_ptr + n)`: inside a loop the decompiler cannot prove that stores through those pointers miss `0x1BBD` itself, so it reloads it. The other four decompile with proper locals.
- **Page 3C code:** the database accessors at `FLASH_3C::BB08` and `FLASH_3C::BB0C` lie in the data-only overlay and are not disassembled; the two `CALL`s at `0xDD01`/`0xDD0A` have no target function.
- **Orphans** in `out/boundaries.tsv` have no visible caller: indirect calls the analysis cannot resolve, or unused code.
- **Analyzer error during import:** function purge analysis reports "Unsupported operation" for this language; it does not stop the analysis.
- **`project/` is not tracked.** Work done in the Ghidra GUI lives only there; export it (e.g. File → Export Program → XML) to version it.
