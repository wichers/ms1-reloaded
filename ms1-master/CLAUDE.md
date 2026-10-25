# CLAUDE.md — Project Instructions

## What This Is

CAN protocol master implementations for Märklin model railway devices:
- **MS1 master** (M-CAN protocol) — controls MS1 (Mobile Station 1) slaves. Ground truth is the MS1's own firmware (run in `../ms1-emu`); `../doc/CAN_Doku_V101_de.pdf` documents the protocol but is wrong in places (README, *Protocol*).
- **CS2 master** (CS2 CAN protocol v2.0) — controls MS2 throttles, Gleisboxes, and other CS2-compatible devices. Based on `../doc/cs2CAN-Protokoll-2_0.pdf`.

Both masters share the same platform abstraction (`ms1_platform.h`) and can coexist on the same CAN bus.

## Project Structure

```
include/ms1_platform.h   — Shared platform abstraction (CAN + timing)
include/ms1_types.h      — MS1 protocol constants, handles, timing, CAN ID encode/decode, data helpers
include/ms1_master.h     — MS1 master API, state machine types, callbacks
include/cs2_types.h      — CS2 protocol constants, CAN ID, hash, CRC, Loc-ID
include/cs2_master.h     — CS2 master API, device/lok/accessory types, callbacks
src/ms1_types.c          — MS1 CAN ID encode/decode implementations
src/ms1_master.c         — MS1 master state machine
src/cs2_types.c          — CS2 CAN ID encode/decode, hash, CRC-16, Loc-ID helpers
src/cs2_master.c         — CS2 master state machine
test/ms1_test_trace.c    — MS1 tests: ID layouts, the document's ECoS trace, master integration
test/ms1_test_reference.c, test/ms1_reference.can — replay of the emulated original MS1 master
test/cs2_test.c          — CS2 tests (types, master, protocol)
```

## Building and Testing

```bash
make test    # all suites; each must report N/N passed
```

No external dependencies. Pure C99 with `<stdint.h>`, `<stdbool.h>`, `<string.h>`, `<stddef.h>`.

## Naming Conventions

- MS1 files use the `ms1_` prefix, CS2 files use the `cs2_` prefix
- All public types, functions, macros, and enums use `ms1_`/`MS1_` or `cs2_`/`CS2_` prefix
- Internal (static) functions use `master_` prefix without namespace
- Do NOT use `mcan_` or `ms2_` prefixes — `ms1_` and `cs2_` are the chosen prefixes

## Protocol Reference

- The README's *Protocol* section is the frame reference; check a change against the MS1 firmware's CAN code (`../ms1-firmware/src/can/`) and the emulator, not only the PDF
- An MS1 slave dispatches by command field and handle, ignores priority, and restarts (registration or the whole firmware) on unexpected registration answers, monitor data or remove frames: a wrong frame can reset it, not just be ignored
- Never set bit 0 of stage-7 data[7]: it starts the slave's bootloader

## Key Design Decisions

1. **Frames as the MS1 master's**, one node pair per slave as an ECoS keeps them (`node_base`, 42 = ECoS numbering, 0 = MS1's)
2. **API-driven lok database** — loks via `ms1_master_set_lok()`, per-slave lokstacks via `ms1_master_assign_lok()`
3. **Reactive setup** — the slave drives the setup (requests), the master answers
4. **Platform agnostic** — all hardware access through `ms1_platform_t` function pointers

## Test Data Source

- `test/ms1_reference.can`: emulated original MS1 master + MS1 slave (header: how it was captured); a protocol change that alters the library's frames must keep `ms1_test_reference` passing or extend its list of differences with the reason
- `test/ms1_test_trace.c`: frames of the PDF's chapter 11 ECoS trace (slave UID `00 02 04 B3`, ECoS UID `00 04 84 D3`)

## When Modifying

- Run `make test` after changes
- CAN ID encode/decode in both `ms1_types.c` and `cs2_types.c` is critical
- CS2: Hash from UID must fix bits 7-9 to 0b110; ping interval is 5000ms; device timeout is 15000ms
- CS2: Config data streaming uses CRC-16/CCITT (poly 0x1021, init 0xFFFF)
