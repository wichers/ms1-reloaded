# CLAUDE.md — MS1 firmware rebuild

C source (inline assembly for the interrupt handlers) of the Märklin Mobile Station 1 firmware (MC9S12D64), built with GCC to behave like the original, with DCC and a device library added. `README.md` covers building, the source layout, the image and RAM layout, and verification.

## Rules

1. **The original binary is the reference** for the behaviour it has. `../ms1-ghidra` (`out/ms1_4011_decomp.c`, `out/listing.tsv`) for what the code does, `../ms1-emu` (the emulator running the original) for how it behaves. Comments in `src/` are hints; where they disagree with the binary, the binary wins. Behaviour the original lacks (DCC, the device library, the programming items) follows the RCN standards and is pinned by `# expect` runs (README: *Expected output*).
2. **Fix `src/` in place**; build with `tools/hc12-gcc/run make` (and `RELEASE=4010`). The `Makefile`'s `SOURCES` and `PAGE3C_SOURCES` list what is built. Fixed flash is nearly full: new code goes into page $3C (`src/include/page3c.h`: entry table, far declarations), whose files define no RAM variables (`src/globals.c` does, without an address).
3. **Verify behaviour changes** with `python3 tools/regress.py` and `--release 4010` (README: *Verification*); both must exit 0. After a deliberate change to new behaviour, rewrite its expected files with `--update RUN` and read the diff before committing. For a change meant to keep the code as it is (renames, types, comments), compare the objects function by function before and after.
4. **Where a function lives:** `tools/functions.tsv` maps each original entry address to its function here; README *Source Layout* maps files to subsystems. Names say what the code does, with a subsystem prefix (`lok_`, `mcan_`, `mfx_`, `mm_`, `dcc_`, `track_`, `lcd_`, `ui_`, `prog_`, `library_`, `str_`).
5. **Slot data** goes through `lok_record_t *` / `lok_state_t *` and the constants in `src/include/lokstack.h`, with `HI8`/`LO8` where the original touches one byte of a word (README: *Struct Typing Policy*).
6. Compute number base conversions with a tool, not by hand.
