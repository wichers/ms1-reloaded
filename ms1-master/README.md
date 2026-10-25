# ms1-master — CAN masters for Märklin MS1 and CS2 devices

Two C99 CAN master libraries on one platform abstraction:

- **MS1 master** (M-CAN): controls Märklin MS1 (Mobile Station 1) handhelds as slaves. It
  implements the master side of the ECoS/CS1/MS1 CAN protocol frame for frame as the MS1's own
  master firmware does it, checked against that firmware running in an emulator (see *Test
  data*). `CAN_Doku_V101_de.pdf` (reverse-engineered by Knut Schwichtenberg) documents the
  protocol; where it and the MS1 disagree, the library follows the MS1 (see *Protocol*).
- **CS2 master** (CS2 CAN protocol 2.0): MS2 throttles, Gleisboxes, accessories, as in
  Märklin's `cs2CAN-Protokoll-2_0.pdf`.

Both can share one CAN bus. The rest of this file covers the MS1 master; the CS2 master's API
is in `include/cs2_master.h`.

## Overview

The library acts as the **master** on the M-CAN bus: MS1 handhelds plugged into the bus
register with it, receive the locomotives the application assigns to them, and drive them.

- **Bus occupation** — eight `0x380` frames once at start-up, as the MS1 master sends them
- **Registration** — UID handshake (stages 0-3), node assignment (stage 7); one slave at a time
- **Setup** — the slave fetches its own object, the status object and list, its lokstack and
  every lok's data (symbol, track protocol, functions, parameters, name, controller)
- **Operational** — monitoring both ways, take/release of a lok, speed/direction/functions in
  both directions, Stop/Go in both directions, loks added to and removed from a lokstack,
  slave rename
- **Slave leaving** — a slave silent for `MS1_HEARTBEAT_TIMEOUT_MS` is given up
- **Flashing** — firmware update protocol (handle allocation, ID queries, data transfer); not
  checked against an MS1

Several slaves can be connected at once, each on its own node pair (`MS1_MAX_SLAVES`). The
implementation is **microcontroller agnostic**: it runs on a platform abstraction you implement
(CAN send/receive, a millisecond clock).

## Protocol

CAN 2.0B, 29-bit extended IDs, 250 kBaud. Operational ID: priority (bits 28-26), object handle
(25-10), command (9-7), sender's node (6-0). Registration ID (node 126): handle = UID byte
(15-8), stage (7-5), MID (4-0); command field = direction (0 slave, 1 master). Flash ID: node
127 (`ms1_types.h`).

**Receiving.** The library dispatches a received frame by its command field and handle, as the
MS1 firmware does; the priority is not evaluated.

**Nodes.** Slave context *i* gets master node `node_base + 2i` and slave node one higher. The
stage-7 answer carries the slave node with bit 7 set (master node = slave node − 1), or, for
master node 0, without (the MS1 master's own numbering: master 0, slave 1).

**Handles** the master hands out (`MS1_HANDLE_*` in `ms1_types.h`; the MS1 master's values):

| Handle | Object | Given to the slave in |
|---|---|---|
| `MS1_HANDLE_MONITOR` | cyclic monitoring | stage-0 answer, data[6..7] |
| `MS1_HANDLE_START` | start object: setup requests, lok announcements and removals | stage-7 answer, data[4..5] |
| `MS1_HANDLE_STATUS` | system status (SY handle): status objects 1-4 | answer to `{3, 0}` |
| `MS1_HANDLE_DEVICE` + slave node | the slave's own object (SD handle): controller id, rename | answer to `{0x40, node}` |
| `MS1_HANDLE_LOK` + lok slot | a lok of the master's database | lokstack iteration, announcement |

**Frames** (D = data; M = master, S = slave; all on the slave's node pair after stage 7):

| Purpose | Sender, command, handle | Data |
|---|---|---|
| registration stage k (0-3) | S cmd 0, node 126 | none; UID byte k in the handle |
| answer to stage k | M cmd 1, node 126, same UID byte and stage, master's MID | stage 0: master UID, `00 02`, monitor handle |
| stage 7 (right after the answer to stage 3) | M cmd 1, node 126 | slave UID, start handle, node byte, `00` |
| own object | S 4 / M 4, start | `{40, node}` → `{40, node, 0000, own handle, 0001}` |
| status object | S 3 / M 3, start | `{03, 00}` → `{03, 00, 0000, SY handle, 0001}` |
| status kinds | S 2 / M 2, SY | `{i, 01}` → `{i, 01, kind}` + value `{i, 00, v}` (priority 2); `{5, 01}` → end |
| lokstack iteration | S 3 / M 3, start | `{80, 02, from}` → `{80, 02, from, lok, counter}`; 4 bytes = end; kinds 81, 82 are empty |
| lok data | S 1 / M 1, lok | `{40 03 00 00}` symbol, `{40 01 00 00}` track protocol, `{40 01 01 00}` address, `{40 04 00 00}` volume, `{02 00 00 off}` name |
| lok objects | S 2 / M 2, lok | `{n, 01}` kind: speed `01 01`, function `02 icon` (bit 7 momentary), 2 bytes = end; `{n, 00}` value; `{1, 02, p, 00}` speed parameter |
| controller | S 0 / M 0 (priority 5), lok | `{03 00}` query, `{03 00 own counter}` take, `{03 01 own counter}` release → `{03 00 controller counter 00}` |
| drive | S or M 2 (priority 2), lok | `{n, 00, value}`: speed (bit 7 reverse) or function (bit 0) |
| Stop/Go, overcurrent | S or M 2 (priority 2), SY | `{1, 00, v}` Stop (from a slave bit 1 = emergency stop), `{2, 00, v}` |
| lok added / removed | M 4 / M 5, start | `{80 00 controller lok counter}` / `{0000 0000 lok counter}` |
| monitoring | S or M 7 (priority 3), monitor | S `{0000, master node}` → M `{0000, master node, master UID}`; M `{0000, slave node, slave UID}` → S the same |
| rename | M 1, own | `{02 00 00 off, 4 characters}`, the terminating 0 in the last frame |

**Where the protocol document differs from the MS1** (the library follows the MS1):

| Document (`CAN_Doku_V101_de.pdf`) | MS1 firmware / emulated MS1 |
|---|---|
| registration ID: UID 25..16, stage 15..13, MID 12..8, direction bit 7 | UID 25..18, stage 17..15, MID 14..10 (its own ECoS trace agrees) |
| MID allows 31 parallel registrations | a slave in registration restarts on any answer that is not its own: one at a time |
| status-object answer `{40, Knot, 00 00, SY, 00 01}` (6.5) | `{03, 00, 00 00, SY, 00 01}` (also the document's ECoS trace) |
| lok type answered on the SY handle (6.7.1) | on the lok's handle (also the ECoS trace) |
| function kind `{FKT, 01, BET, ICON}`, BET 00 speed / 01 / 02 (6.7.3) | speed `{n, 01, 01, 01}`, function `{n, 01, 02, icon}`, momentary = icon bit 7 |
| monitoring answer `{03 00 00 Knot, slave UID}` (7.2.1) | `{00 00 00 Knot, master UID}` (also the ECoS trace) |
| slave drives at priority 2, master at 3; Stop/Go at 4 | everything at priority 2 |
| status 4 value 1 = normal operation | 1 = mfx registration running (blinking mfx icon) |
| a missing slave: ~7.5 s, then a probe every second, 30 times (ECoS) | polled after 256 ms of silence, given up after 5.116 s |
| `0x380` "bus occupied": cyclic | once, at start-up; to a slave registered with master node 0 it is a reset |

## Files

```
include/
  ms1_platform.h    Platform abstraction (CAN send/receive, millis, log), shared by both masters
  ms1_types.h       MS1 protocol constants, handles, timing, CAN ID encode/decode, data helpers
  ms1_master.h      MS1 master API, types, callbacks, internal state
  cs2_types.h       CS2 protocol constants, CAN ID, hash, CRC, Loc-ID
  cs2_master.h      CS2 master API, device/lok/accessory types, callbacks
src/
  ms1_types.c       MS1 CAN ID encode/decode implementations
  ms1_master.c      MS1 master state machine
  cs2_types.c       CS2 CAN ID encode/decode, hash, CRC-16, Loc-ID helpers
  cs2_master.c      CS2 master state machine
test/
  ms1_test_trace.c      MS1 tests: ID layouts, the document's ECoS trace, master integration
  ms1_test_reference.c  replays ms1_reference.can against the library (see Test data)
  ms1_reference.can     the original MS1 firmware as master with an MS1 slave (emulated)
  cs2_test.c            CS2 tests (types, master, protocol)
```

**Protocol documents** (not included; third-party): `CAN_Doku_V101_de.pdf` (M-CAN, 56 pages)
and `cs2CAN-Protokoll-2_0.pdf` (CS2). In the workspace this project belongs to they are in
`../doc/`, with page images in `../doc/pages/`.

## Building

### Library

Include `ms1_types.c` and `ms1_master.c` in your build, with `include/` on the include path. No external dependencies beyond `<stdint.h>`, `<stdbool.h>`, `<string.h>`, and `<stddef.h>`.

### Tests

```bash
make test        # builds build/ms1_test_trace, build/ms1_test_reference and build/cs2_test, runs them
```

Each prints `Results: N/N tests passed` and exits with status 1 on a failure.

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

/* 2. Configure the master (uid must not be 0) */
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

/* 5. Add locomotives: object 1 = speed, then functions numbered 2, 3, ... without gaps */
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

/* 7. Poll in your main loop (every few ms: the slave's setup is one request per answer) */
while (1) {
    ms1_master_poll(&master);
}
```

## Configuration

Compile-time limits (override by defining before including headers):

| Define | Default | Description |
|--------|---------|-------------|
| `MS1_MAX_SLAVES` | 4 | Maximum simultaneous MS1 slaves (node pairs) |
| `MS1_MAX_LOKS` | 20 | Locomotive database size |
| `MS1_MAX_LOK_FUNCS` | 16 | Functions per locomotive (an MS1 keeps 9 with an icon) |
| `MS1_LOKSTACK_SIZE` | 10 | Loks per slave lokstack (an MS1 has 10 places) |
| `MS1_LOK_NAME_LEN` | 16 | Max lok name length (an MS1 keeps 15 characters) |

`ms1_master_config_t`: `uid` (sent in the registration answers and monitoring answers; not 0),
`node_base` (42 as an ECoS/CS1, 0 as an MS1 master), `vendor_id`, `master_type`,
`hw_version`, `sw_version`, `build_version` (the registration answers of stages 1-3; an MS1
slave does not evaluate them).

Timing (`ms1_types.h`, the MS1's 4 ms protocol ticks): `MS1_HEARTBEAT_INTERVAL_MS` (poll a
silent slave), `MS1_HEARTBEAT_TIMEOUT_MS` (give it up; also how long a slave waits for the
master before it restarts), `MS1_REG_TIMEOUT_MS` (drop a stalled registration),
`MS1_BUS_OCCUPIED_BURST_COUNT`.

## API Reference

### Master Lifecycle
- `ms1_master_init()` — Initialize master context
- `ms1_master_poll()` — Process CAN frames, polls and timeouts (call repeatedly)

### Locomotive Management
- `ms1_master_set_lok()` — Add/update a lok in the database; an update is announced to the
  slaves that have it (new change counter), which fetch it again
- `ms1_master_remove_lok()` — Remove a lok from the database and every lokstack
- `ms1_master_assign_lok()` — Add a lok to a slave's lokstack (slave in SETUP or OPERATIONAL);
  announced at once
- `ms1_master_unassign_lok()` — Remove a lok from a slave's lokstack

### Control
- `ms1_master_drive_lok()` — Set lok speed/direction from master side (every slave with the lok)
- `ms1_master_drive_function()` — Set a lok function from master side
- `ms1_master_set_stop()` — Set system stop/go state (all slaves)
- `ms1_master_set_overcurrent()` — Set overcurrent/short-circuit state (all slaves)

### Utilities
- `ms1_master_rename_slave()` — Send a new name to a connected slave (it stores it)
- `ms1_master_get_slave_info()` — Read slave state, UID, nodes, lokstack, the lok it controls
- `ms1_master_load_firmware()` — Load firmware data for flash operations

### Callbacks
| Callback | When |
|----------|------|
| `on_slave_connected` | Slave completed registration and setup (fetched its lokstack) |
| `on_slave_disconnected` | A connected slave was silent for `MS1_HEARTBEAT_TIMEOUT_MS`, or registered again |
| `on_lok_selected` | Slave took a lok (lok slot) or released it (-1) |
| `on_lok_speed` | The slave controlling the lok changed speed/direction |
| `on_lok_function` | The slave controlling the lok switched a function |
| `on_stop` / `on_go` | Slave pressed Stop or Go |
| `on_flash_request` | Slave requested firmware update |

### Behaviour
- **Controller.** A lok is free or controlled by one slave. A take of a free lok is granted, a
  take of a lok another slave controls is answered with that slave's object (the MS1 shows the
  lok as not controllable); every slave with the lok learns a change. An MS1 takes the first
  lok of its lokstack by itself after setup.
- **Driving.** Speed and function frames from the controlling slave fire the callbacks and go
  to every slave with the lok, the sender included (as the MS1 master echoes them). From a
  slave that does not control the lok they are answered with the current value.
- **Stop/Go** from a slave goes to the other slaves; from the API to all.
- **Monitoring.** A slave polls when the master has been silent; the master answers. The
  master polls a slave silent for `MS1_HEARTBEAT_INTERVAL_MS` and gives it up after
  `MS1_HEARTBEAT_TIMEOUT_MS` (its controlled lok becomes free). A device registering again
  (same UID) replaces its old connection.

## Known limitations

- A lok's track address and protocol flags are not modelled: answered as 0 (as an ECoS does).
  Speed parameters and volume are answered with the values the MS1 gives a new lok.
- Changes a slave writes to a lok (its edit menu) are answered with the master's values and not
  applied; a slave's request for a slot for a lok it creates is not answered.
- Registrations run one at a time: slaves started together register one per
  `MS1_REG_TIMEOUT_MS`.
- The flash protocol has not been run against an MS1.

## Test Data

- **Reference capture** (`test/ms1_reference.can`): the original MS1 firmware (4011) as master
  with the same firmware as slave, in the emulator `ms1-emu` (its header gives the command and
  the operator inputs): registration, setup, a lok added, take, driving, functions, direction,
  Stop/Go from both sides, the slave unplugged. `ms1_test_reference` feeds the slave's frames
  to the library configured as that master (node 0, its UID, the same lok) and checks the
  library's frames (ID, length, data, order, no later than the MS1's), its callbacks, its polls
  after the unplug (within one 4 ms tick of the MS1's) and the time it gives the slave up. The
  frames the library intentionally sends differently are listed in the test.
- **Document trace** (`CAN_Doku_V101_de.pdf`, chapter 11, pages 53-56): an ECoS (FW 3.0.1)
  with an MS1 slave holding Lok 003 131-0; master node 42, slave node 43, slave UID
  `00 02 04 B3`, ECoS UID `00 04 84 D3`, monitoring handle `0x0001`, start object `0x0002`,
  SD handle `0x0008`, SY handle `0x0003`.
- The library was also run in real time against the emulated MS1 slave (`ms1-emu`'s
  `master_bridge`): registration, both loks, driving with callbacks, Stop/Go both ways,
  take/release, unplug and re-plug, two slaves at once.

## License

Apache License 2.0 (`LICENSE`, `NOTICE`). Märklin, Mobile Station, Central Station and mfx are
trademarks of Gebr. Märklin & Cie. GmbH, ECoS of ESU; this project is not affiliated with
either.
