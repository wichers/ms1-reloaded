# CLAUDE.md — Project Instructions

## What This Is

CAN protocol master implementations for Märklin model railway devices:
- **MS1 master** (M-CAN protocol) — controls MS1 (Mobile Station 1) slaves. Based on `../doc/CAN_Doku_V101_de.pdf`.
- **CS2 master** (CS2 CAN protocol v2.0) — controls MS2 throttles, Gleisboxes, and other CS2-compatible devices. Based on `../doc/cs2CAN-Protokoll-2_0.pdf`.

Both masters share the same platform abstraction (`ms1_platform.h`) and can coexist on the same CAN bus.

## Project Structure

```
include/ms1_platform.h   — Shared platform abstraction (CAN + timing)
include/ms1_types.h      — MS1 protocol constants, CAN ID encode/decode, data helpers
include/ms1_master.h     — MS1 master API, state machine types, callbacks
include/cs2_types.h      — CS2 protocol constants, CAN ID, hash, CRC, Loc-ID
include/cs2_master.h     — CS2 master API, device/lok/accessory types, callbacks
src/ms1_types.c          — MS1 CAN ID encode/decode implementations
src/ms1_master.c         — MS1 master state machine (~1600 lines)
src/cs2_types.c          — CS2 CAN ID encode/decode, hash, CRC-16, Loc-ID helpers
src/cs2_master.c         — CS2 master state machine
test/ms1_test_trace.c    — 20 MS1 tests with real CAN trace data
test/cs2_test.c          — 23 CS2 tests (types, master, protocol)
```

## Building and Testing

```bash
make test    # both suites; each must report N/N passed
```

No external dependencies. Pure C99 with `<stdint.h>`, `<stdbool.h>`, `<string.h>`, `<stddef.h>`.

## Naming Conventions

- MS1 files use the `ms1_` prefix, CS2 files use the `cs2_` prefix
- All public types, functions, macros, and enums use `ms1_`/`MS1_` or `cs2_`/`CS2_` prefix
- Internal (static) functions use `master_` prefix without namespace
- Do NOT use `mcan_` or `ms2_` prefixes — `ms1_` and `cs2_` are the chosen prefixes

## Protocol Reference

- CAN 2.0B, 29-bit extended IDs, 250 kBaud
- CS1/ECoS style master (not MS1-Master style)
- Node base: 42 (master=42, first slave=43)
- SD-Handle: `0x0008`, SY-Handle: `0x0003`
- Bus-occupied CAN ID: `0x380`
- Three CAN ID bit layouts: Registration (node=126), Operational, Flash (node=127)
- Full protocol details in `../doc/` (not part of this repository) — page images of both PDFs in `../doc/pages/`

## Key Design Decisions

1. **CS1/ECoS style** — Uses node numbers starting at 42/43 (not MS1-Master which uses different addressing)
2. **API-driven lok database** — Loks are added via `ms1_master_set_lok()` calls, not compiled-in
3. **Reactive setup** — The slave drives the setup sequence (sends queries), the master responds
4. **Flash support included** — Full firmware update protocol with handle allocation and data transfer
5. **Platform agnostic** — All hardware access goes through `ms1_platform_t` function pointers

## Test Data Source

Tests in `test/ms1_test_trace.c` use real captured CAN frames from Chapter 11 of the protocol PDF (pages 53-56). The trace shows an ECoS FW 3.0.1 communicating with an MS1 slave that has Lok 003 131-0 in its lokstack:

- Slave UID: `{0x00, 0x04, 0x84, 0xD3}`
- Heartbeat CAN IDs: `0x0C0007AA` (master), `0x0C0007AB` (slave)
- SD-Handle request: `0x18000A2B`

## When Modifying

- Always run both test suites after changes: MS1 (20/20) and CS2 (23/23)
- CAN ID encode/decode in both `ms1_types.c` and `cs2_types.c` is critical — bit positions must match PDFs
- MS1: Registration handler follows 8-stage handshake; setup is slave-driven
- MS1: Heartbeat timeout is 7500ms; heartbeat interval is ~250ms
- CS2: Hash from UID must fix bits 7-9 to 0b110; ping interval is 5000ms; device timeout is 15000ms
- CS2: Config data streaming uses CRC-16/CCITT (poly 0x1021, init 0xFFFF)
