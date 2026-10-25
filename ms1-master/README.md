# ms1-master — CAN masters for Märklin MS1 and CS2 devices

Two C99 CAN master libraries on one platform abstraction:

- **MS1 master** (M-CAN): controls Märklin MS1 (Mobile Station 1) handhelds as slaves. It
  implements the master side of the ECoS/CS1/MS1 CAN protocol as documented in
  `CAN_Doku_V101_de.pdf` (reverse-engineered by Knut Schwichtenberg).
- **CS2 master** (CS2 CAN protocol 2.0): MS2 throttles, Gleisboxes, accessories, as in
  Märklin's `cs2CAN-Protokoll-2_0.pdf`.

Both can share one CAN bus. The rest of this file covers the MS1 master; the CS2 master's API
is in `include/cs2_master.h`.

## Overview

This library acts as a **master** on the M-CAN bus, allowing real MS1 handheld controllers to connect as slaves. It handles the full protocol lifecycle:

- **Bus occupation** — Periodic probe frames signaling an active master
- **Registration** — 8-stage handshake exchanging UIDs, hardware/software info, and node assignment
- **Setup** — Handle allocation (SD/SY), system status, lokstack exchange, per-lok data queries
- **Operational** — Heartbeat monitoring, speed/function control, stop/go, lok assignment
- **Flashing** — Firmware update protocol (handle allocation, ID queries, data transfer)

The implementation is **microcontroller agnostic** — it runs on top of a platform abstraction layer that you implement for your hardware.

## Protocol

- CAN 2.0B with 29-bit extended IDs at 250 kBaud
- CS1/ECoS style: node base 42/43, SD-Handle `0x0008`, SY-Handle `0x0003`
- Three CAN ID formats: Registration (node 126), Operational, Flash (node 127)
- 8 priority levels (0 = highest, 7 = lowest)
- API-driven locomotive database via `ms1_master_set_lok()`

## Files

```
include/
  ms1_platform.h    Platform abstraction (CAN send/receive, millis, log), shared by both masters
  ms1_types.h       MS1 protocol constants, CAN ID encode/decode, data helpers
  ms1_master.h      MS1 master API, types, callbacks, internal state
  cs2_types.h       CS2 protocol constants, CAN ID, hash, CRC, Loc-ID
  cs2_master.h      CS2 master API, device/lok/accessory types, callbacks
src/
  ms1_types.c       MS1 CAN ID encode/decode implementations
  ms1_master.c      MS1 master state machine
  cs2_types.c       CS2 CAN ID encode/decode, hash, CRC-16, Loc-ID helpers
  cs2_master.c      CS2 master state machine
test/
  ms1_test_trace.c  MS1 tests with the CAN trace of the protocol document's chapter 11
  cs2_test.c        CS2 tests (types, master, protocol)
```

**Protocol documents** (not included; third-party): `CAN_Doku_V101_de.pdf` (M-CAN, 56 pages)
and `cs2CAN-Protokoll-2_0.pdf` (CS2). In the workspace this project belongs to they are in
`../doc/`, with page images in `../doc/pages/`.

## Building

### Library

Include `ms1_types.c` and `ms1_master.c` in your build, with `include/` on the include path. No external dependencies beyond `<stdint.h>`, `<stdbool.h>`, `<string.h>`, and `<stddef.h>`.

### Tests

```bash
make test        # builds build/ms1_test_trace and build/cs2_test and runs both
```

Each prints `Results: N/N tests passed`.

## Quick Start

```c
#include "ms1_master.h"

/* 1. Implement platform functions for your hardware */
ms1_platform_t platform = {
    .can_send    = my_can_send,
    .can_receive = my_can_receive,
    .millis      = my_millis,
    .log         = NULL,
};

/* 2. Configure the master */
ms1_master_config_t config = {
    .uid          = {0x00, 0x11, 0x22, 0x33},
    .hw_version   = 0x4010,
    .sw_version   = 0x0301,
    .vendor_id    = MS1_VENDOR_ESU,
    .master_type  = MS1_MASTER_TYPE_ECOS,
    .node_base    = 42,
};

/* 3. Set up callbacks */
ms1_master_callbacks_t callbacks = {0};
callbacks.on_slave_connected = my_on_connected;
callbacks.on_lok_speed       = my_on_speed;
callbacks.on_lok_function    = my_on_function;

/* 4. Initialize */
ms1_master_t master;
ms1_master_init(&master, &platform, &config, &callbacks);

/* 5. Add locomotives */
ms1_lok_t lok = {0};
strncpy(lok.name, "BR 44", MS1_LOK_NAME_LEN);
lok.type = MS1_LOK_TYPE_STEAM;
lok.format = MS1_RAIL_MFX;
lok.func_count = 2;
lok.funcs[0] = (ms1_lok_func_t){.number = 1, .activation = MS1_FUNC_SPEED};
lok.funcs[1] = (ms1_lok_func_t){.number = 2, .activation = MS1_FUNC_MAINTAINED, .icon = MS1_ICON_01};
ms1_master_set_lok(&master, 0, &lok);

/* 6. In your on_connected callback, assign loks to the slave */
void my_on_connected(void *ctx, uint8_t slave_id) {
    ms1_master_assign_lok(&master, slave_id, 0);
}

/* 7. Poll in your main loop */
while (1) {
    ms1_master_poll(&master);
}
```

## Configuration

Compile-time limits (override by defining before including headers):

| Define | Default | Description |
|--------|---------|-------------|
| `MS1_MAX_SLAVES` | 4 | Maximum simultaneous MS1 slaves |
| `MS1_MAX_LOKS` | 20 | Locomotive database size |
| `MS1_MAX_LOK_FUNCS` | 16 | Functions per locomotive |
| `MS1_LOKSTACK_SIZE` | 10 | Loks per slave lokstack |
| `MS1_LOK_NAME_LEN` | 16 | Max lok name length |

## API Reference

### Master Lifecycle
- `ms1_master_init()` — Initialize master context
- `ms1_master_poll()` — Process CAN frames and timeouts (call repeatedly)

### Locomotive Management
- `ms1_master_set_lok()` — Add/update a lok in the database
- `ms1_master_remove_lok()` — Remove a lok from the database
- `ms1_master_assign_lok()` — Push a lok to a slave's lokstack
- `ms1_master_unassign_lok()` — Remove a lok from a slave's lokstack

### Control
- `ms1_master_drive_lok()` — Set lok speed/direction from master side
- `ms1_master_drive_function()` — Set a lok function from master side
- `ms1_master_set_stop()` — Set system stop/go state
- `ms1_master_set_overcurrent()` — Set overcurrent/short-circuit state

### Utilities
- `ms1_master_rename_slave()` — Send a new name to a connected slave
- `ms1_master_get_slave_info()` — Read slave state, UID, lokstack
- `ms1_master_load_firmware()` — Load firmware data for flash operations

### Callbacks
| Callback | When |
|----------|------|
| `on_slave_connected` | Slave completed registration and setup |
| `on_slave_disconnected` | Slave heartbeat timed out |
| `on_lok_speed` | Slave changed speed/direction |
| `on_lok_function` | Slave toggled a function |
| `on_stop` / `on_go` | Slave pressed Stop or Go |
| `on_lok_selected` | Slave selected/deselected a lok |
| `on_flash_request` | Slave requested firmware update |

## Test Data

Tests use real CAN trace data captured from an ECoS (FW 3.0.1) communicating with an MS1 slave controlling Lok 003 131-0:

- Master node: 42, Slave node: 43
- Slave UID: `{0x00, 0x04, 0x84, 0xD3}`
- Verified CAN IDs: bus-occupied (`0x380`), heartbeat (`0x0C0007AA`/`0x0C0007AB`), SD-handle request (`0x18000A2B`)

## License

Apache License 2.0, as the whole repository (`../LICENSE`, `../NOTICE`).
