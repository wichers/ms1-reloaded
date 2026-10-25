# CLAUDE.md — MS1 firmware rebuild

C (and the original's hand-written assembly) source of the Märklin Mobile Station 1 firmware (MC9S12D64), built with GCC to behave like the original. `README.md` covers building, the source layout, the image and RAM layout, and verification.

## Rules

1. **The original binary is the reference.** `../ms1-ghidra` (`out/ms1_4011_decomp.c`, `out/listing.tsv`) for what the code does, `../ms1-emu` (the emulator running the original) for how it behaves. Comments in `src/` are hints; where they disagree with the binary, the binary wins.
2. **Fix `src/` in place**; build with `tools/hc12-gcc/run make`. The `Makefile`'s `SOURCES` lists what is built.
3. **Verify behaviour changes** with `python3 tools/regress.py` (README: *Verification*); it must exit 0. For a change meant to keep the code as it is (renames, types, comments), compare the objects function by function before and after.
4. **Where a function lives:** `tools/functions.tsv` maps each original entry address to its function here; README *Source Layout* maps files to subsystems. Names say what the code does, with a subsystem prefix (`lok_`, `mcan_`, `mfx_`, `mm_`, `track_`, `lcd_`, `ui_`, `str_`).
5. **Slot data** goes through `lok_record_t *` / `lok_state_t *` and the constants in `src/include/lokstack.h`, with `HI8`/`LO8` where the original touches one byte of a word (README: *Struct Typing Policy*).
6. Compute number base conversions with a tool, not by hand.
