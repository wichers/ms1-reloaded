# ms1-firmware — the Märklin Mobile Station 1 firmware in C

A C (and GNU assembler) source of the Märklin Mobile Station 1 (MS1) application firmware,
built with GCC, that behaves like the original: verified function by function and as a whole
against the original running in the emulator (`../ms1-emu`), and buildable as the official
release images (`make images`).

## Project Goal

Produce a C source of the MS1 firmware that behaves like the original. The original binary is
the reference: `../ms1-ghidra` (decompilation, instruction listing) for what the code does and
`../ms1-emu` (the emulator running the original) for how it behaves; comments in the sources
are hints, not facts.

- [docs/architecture.md](docs/architecture.md): how the parts of the firmware fit together —
  start-up, scheduling, the track signal (MM, mfx, feedback), the locomotive slots, CAN, the
  display, keys and knob, EEPROM, the peripherals
- [docs/bootloader.md](docs/bootloader.md): the bootloader and the application — how it starts
  and updates the application, the flash metadata block, and what a modified firmware must keep

## Building

The firmware is built with the GNU 68HC12 toolchain the original was built with (GCC m68hc1x:
same soft-register frame pointer `_.frame`, start-up code and section markers), GCC 3.3.6 +
m68hc1x 3.1 and binutils 2.18 as Debian 7 packaged them, in a container:

```sh
tools/hc12-gcc/run make          # build/ms1.s19: flashable image, bootloader included
tools/hc12-gcc/run make images   # the release images (RELEASES), each as .s19, .bin and .bci
tools/hc12-gcc/run make DEBUG=1  # with SCI0 debug output (MS1_DEBUG_SERIAL, SCI0 115200 8N1)
tools/hc12-gcc/run make RELEASE=4010 images  # version 4010 (releases 4010, c010) in build/4010/
tools/hc12-gcc/run make clean    # all of build/, every version
../ms1-emu/build/ms1emu -i build/ms1.s19     # run it in the emulator
```

`tools/hc12-gcc/run` builds the `ms1-hc12-gcc` Docker image from `tools/hc12-gcc/Dockerfile` on
first use and runs the command in it, in this directory, with the repository around it mounted
at the same path.

**Inputs from the original** (not part of this repository; the workspace README's *What you
provide*), as `make` variables; the defaults of the last four depend on `RELEASE`:

| Variable | Default | Used for |
|---|---|---|
| `RELEASE` | `4011` | the application version built: `4011` in `build/`, `4010` in `build/4010/` (*Release images*) |
| `FIRMWARE` | `../original-firmware` | the directory of the release images and the device dump |
| `ORIGINAL` | `$(FIRMWARE)/firmware_20260317_192757_usbdm.s19` | a device dump (a 4011 device's): the bootloader, and for 4011's `build/ms1.s19` the original's data |
| `APP_IMAGE` | `$(FIRMWARE)/4011_1-8_mobile-station-2_A.bin`; 4010: `$(FIRMWARE)/4010_1-8_mobile-station_A.bin` | the decoded update of the version: the assembly modules' bytes; for 4010 also the original's data of `build/4010/ms1.s19` |
| `LISTING` | `../ms1-ghidra/out/listing.tsv`; 4010: `../ms1-ghidra/out/listing_4010.tsv` | the original's instructions: the assembly modules. The 4010 one holds the modules only and is made with `../ms1-ghidra/ghidra.sh listing $(APP_IMAGE) asm/4010 $(LISTING)` (Ghidra, see `../ms1-ghidra/README.md`) |
| `RELEASES` | `4011_1-8_mobile-station-2_A c011_1-8_mobile-station-2_A`; 4010: `4010_1-8_mobile-station_A c010_1-8_mobile-station_A` | `make images`: `$(FIRMWARE)/<release>.bin` and `.bci` |

**Release images.** Releases 4011 (article 60652) and c011 (60651) have the same code and
differ only in data the build takes from the original (article number and build strings, the
configuration words, the CRC word), so one application builds both: `build/<release>.s19` is
the application with page $3C and 0xE11B-0xF73F from the release's decoded update file and the
bootloader from `ORIGINAL`; `build/<release>.bin` is the same in the update file's page order;
`build/<release>.bci` is an update file encoded from it with the release's own `.bci` as
template (`../tools/bci.py`; it decodes back to the image, it has not been tried on a device).

Releases 4010 and c010 are application version 4010, for the MS1 hardware without track
current measurement, and `make RELEASE=4010 images` builds them the same way into
`build/4010/`. The version is one source with `#if MS1_RELEASE == 0x4010` points (C) and
`.ifdef MS1_4010` (`gcc/crt0.s`); `docs/architecture.md` section 11 lists what differs:

| | 4011 | 4010 |
|---|---|---|
| code (`MS1_RELEASE`) | the default | `track_current_monitor`, `ui_task` start-up screen and restart paths (`TRACK_OUTPUT_OFF`), INFO item 9 (`OVC:`), `mfx_frame_begin` 9/11-bit address prefixes (not the mfx format, as the original sends them), `crt_restart`/`enter_bootloader` |
| RAM (`tools/releases.py`) | `src/globals.c` | no variables at 0x10F2-0x10FC; from 0x10FD up 0x0B lower, soft registers at 0x1BAC |
| application / original's .rodata | 0x4000-0xE11A / 0xE11B-0xF73F | 0x4000-0xDFA0 / 0xDFA1-0xF73F |
| table symbols | `gcc/4011/release.ld` | `gcc/4010/release.ld` |
| assembly modules | `asm/*.json` | `asm/4010/*.json` |
| function table (difftest) | `tools/functions.tsv` | `tools/functions_4010.tsv`, row for row the same functions |

The bootloader starts an application only if the version word at 0xF73C agrees with its flash
metadata word at 0xFF1A; the device dump is a 4011 device's (0xC011) and refuses 4010. So the
4010 and c010 `.s19` images (and `build/4010/ms1.s19`) carry the dump's bootloader with the low
byte at 0xFF1B set to 0x10 (`tools/mkimage.py` does so when the check fails, and says so); a
4011 image keeps the bootloader as it is. The firmware shows that word in INFO (`HW:`). The
`.bin` and `.bci` carry no bootloader and are unaffected.

| File | Role |
|---|---|
| `Makefile` | sources, flags (`-m68hcs12 -mshort -O1 -fomit-frame-pointer`, per-function sections, `--gc-sections`), `UNCALLED` (functions the original has but nothing calls, kept with `-u`) |
| `gcc/crt0.s` | start-up: the original's 0x4000-0x4076 (MMC, PLL, then `crt_restart` = the original's 0x403C: .data copy, .bss clear, stack, `main(1, {"main"})`) |
| `gcc/vectors.s` | interrupt trampolines 0xF740-0xF7FF: the bootloader's vectors point at 0xF740 + 3n; unused vectors stop in `unexpected_interrupt` (BGND) |
| `gcc/ms1.ld` | linker script template: memory map; symbols for the bootloader's flash metadata words the code reads by name (`docs/bootloader.md`) |
| `gcc/<version>/release.ld` | the version's symbols for the tables in the original's .rodata, and the end of the application; `gcc/ms1.ld` INCLUDEs it through `-L gcc/<version>` |
| `tools/releases.py` | the versions: RAM layout, where the original's .rodata starts, the per-version files |
| `tools/ramlayout.py` | writes `build/ms1.ld` from the template, placing every RAM variable at its address (below) |
| `gcc/mc9s12d64.h` | register definitions, from the MC9S12DJ64 Device User Guide register map |
| `tools/mkimage.py` | final image: application + parts of the original + CRC |
| `tools/regress.py`, `tools/scenarios/`, `tools/functions.tsv`, `tools/functions_4010.tsv`, `tools/calpatch.py` | verification against the original with `../ms1-emu` (*Verification*) |
| `tools/objcmp.py` | two builds compared function by function, for changes meant to keep the code |
| `asm/*.json`, `asm/4010/*.json` | the original's hand-written assembly modules (below), of version 4011 and 4010: ranges, entry names, jump tables; the build generates `build/asm/*.s` (`build/4010/asm/*.s`) from them with `tools/asmgen.py` |
| `gcc/mc9s12d64.inc` | register addresses for assembly sources (`tools/asmgen.py regs`, from `gcc/mc9s12d64.h`) |

**Image layout.** The application (code, its own read-only data, the .data image) takes
0x4000-0xE11A (version 4010: 0x4000-0xDFA0, its original's part starts at 0xDFA1), the space of
the original's code and GCC runtime helpers; page $3D is mapped into
the 0x8000 window for good, as in the original. `tools/mkimage.py` takes from the original image
(`ORIGINAL`, default the device dump): page $3C (the
locomotive database and the accessors reached by `CALL $BB08/$BB0C,$38`), 0xE11B-0xF73F (the
original's .rodata — language tables, display maps, fonts, trit tables the code reads by address
— build information, configuration area) and 0xF800-0xFFFF (bootloader, flash metadata,
vectors). It then sets the word at 0xF73E so that the bootloader's CRC-16 over page $3C and
0x4000-0xF7FF comes out 0, as the original does.

**RAM layout.** Every RAM variable is at its address in the original, so RAM is comparable
byte for byte with the original's (see *Verification*). `src/globals.c` defines
them with `RAM(addr)` (or `RAM_DATA(addr)` for the original's initialised `.data`,
0x1000-0x102F, whose initial values are in the definitions and are copied by `crt_restart`);
the macros put each variable in a section `.bss.ram.<addr>` / `.data.ram.<addr>`, and
`tools/ramlayout.py` places those sections, failing on overlaps and on variables outside the
original's `.data` (0x1000-0x102F) and `.bss` (0x1030-0x1BB6). The addresses in
`src/globals.c` are version 4011's; for 4010 (`--release 4010`) it places each variable from
0x10FD up 0x0B lower and fails on one in 0x10F2-0x10FC, which 4010 does not have
(`tools/releases.py`). The GCC soft registers follow
at 0x1BB7 (`_.tmp`, `_.z`, `_.xy`, `_.frame`), as in the original; variables without an
address go after them, and an initialised one without an address fails the link. Names for
fields inside other variables (e.g. the CAN frame at 0x106F, the display buffer at 0x1108)
are macros at the end of `src/include/globals.h`.

**Assembly modules.** Parts of the original are hand-written assembly — code with jump
tables into the middle of routines, shared tails and register conventions C does not
express. Their source is assembly too, generated by the build (`tools/asmgen.py gen
asm/X.json build/asm/X.s`) from the original's instructions (`LISTING`, `APP_IMAGE`): this
repository holds only the module descriptions (`asm/*.json`: ranges, entry names, and the jump
tables and other code `../ms1-ghidra/ghidra.sh listing` disassembles from) and the generator.
Version 4010's modules (`asm/4010/*.json`, generated with `RELEASE=4010`) are the same code at
other addresses, except the timer channel 4 interrupt and `signal_gen_init` (the booster line,
the over-current threshold) and two routines of their own; immediates that point into RAM or
the original's .rodata are numbers in the generated source, so each version has its own. The
generated source has labels for branch targets, names for functions (the module's `names`, the
version's function table, the other modules' entries), RAM variables (`src/globals.c`, at the
version's addresses) and I/O registers, data as `.byte`/`.word`.
`tools/asmgen.py verify asm/X.json build/asm/X.s` assembles a module at its original address
with every other symbol at its original address and compares the bytes with the original
image; every module is byte-identical, of both versions (4010's with `RELEASE=4010` and its
`LISTING` and `APP_IMAGE` in the environment, as the Makefile runs `gen`).

| Module | Original | Version 4010 | Contents |
|---|---|---|---|
| `siggen` | `0xC23E`-`0xCA7F` | `0xC084`-`0xC905` | PT6547 SPI pump interrupt, display buffer rendering (segments, speed bar, glyphs), track signal generator: timer channel 4 interrupt, staging buffer of compare values, MM/mfx bit encoders, command buffer consumer |
| `encoder` | `0xBA96`-`0xBB5F` | `0xB8DC`-`0xB9A5` | rotary encoder: counter set-up (value, range, wrap mode) and the quadrature sampling the main loop calls twice per millisecond |
| `hal` | `0xDCCF`-`0xE0A3` | `0xDB55`-`0xDF29` | EEPROM erase/program, page-$3C database accessors (`CALL $BB08/$BB0C,$38`), CAN0 transmit/receive/error interrupts and frame rings, track-feedback capture (timer channel 0), port reads, 1 ms tick interrupt, display pump, delays |

GCC's own libgcc provides what the original has at `0xE0B0`-`0xE11A` (multiply and shift
helpers), except the left shift `lok_icons.c` calls by name (`src/lib/runtime.c`).

## Source Layout

`tools/functions.tsv` maps each original function (entry address) to the function here;
the files group them by what they do.

| File | Contents |
|---|---|
| `src/main.c` | the main loop: one pass per millisecond, the work of each `ms_tick` phase |
| `src/globals.c` | every RAM variable at its original address (*RAM layout*) |
| `src/sys/app_init.c` | start-up, EEPROM check and restore, ADC scaling |
| `src/sys/sys_main.c` | `show_message`, `track_scheduler` (phase 2) |
| `src/sys/eeprom_save.c` | EEPROM record writer and the save task |
| `src/sys/ms_tick.c`, `timer_init.c` | the millisecond tick; timer and ATD set-up |
| `src/can/mcan.c` | MCAN receive/send, registration, master and slave frame dispatch |
| `src/can/mcan_status.c` | status frames (STOP, overload, mfx registration), control frames |
| `src/can/mcan_lokstack.c` | the slots' changes as CAN frames; the master's answers to slave requests |
| `src/can/mcan_object.c` | object requests (kind, IDs, name, locomotive data); received locomotive state |
| `src/can/mcan_slave.c` | the slave's requests advancing; the master releasing a lost slave's slots |
| `src/can/mcan_name.c` | the device name sent on CAN |
| `src/signal/lok_packets.c` | which track packet each locomotive slot needs next, and the periodic ones |
| `src/signal/track_filler.c` | the track filler when no slot needs a packet (mfx central frame, MM idle) |
| `src/signal/mm_encode.c` | MM packets for the command buffer |
| `src/signal/mfx_encode.c` | mfx frames for the command buffer |
| `src/signal/mfx_register.c` | mfx decoder search, registration and read-out |
| `src/signal/mfx_feedback.c` | evaluating a captured mfx feedback window (1 bit, data) |
| `src/signal/track_monitor.c` | track current, overload shutdown, operating counters |
| `src/lok/lokstack.c` | slot lookups, CAN objects of a slot, address conflicts, unique names |
| `src/lok/lok_functions.c` | a locomotive's functions: CAN object numbers and keys |
| `src/ui/ui_control.c` | the UI state machine: knob, keys, menus |
| `src/ui/ui_mode.c` | switching the operating mode |
| `src/ui/screen_text.c`, `lcd_text.c` | the text line: composing it per screen, placing and scrolling it; menu levels |
| `src/ui/status_icons.c`, `lok_icons.c` | status icons, speed bar; the locomotive's function icons and symbol |
| `src/ui/ui_helpers.c` | knob helpers |
| `src/lib/str.c` | string and number formatting, display character set conversion |
| `src/lib/runtime.c` | the original's compiler helper the C code calls by name |
| `src/hal/gpio_buttons.c`, `serial_debug.c` | key scan; SCI0 debug output (`DEBUG=1`) |
| `src/include/` | `hcs12.h` (integer types, registers, interrupt masking), `lokstack.h` (slot records and states), `track_cmd.h` (command buffer writers), `lcd.h`, `ui.h`, `mcan.h`, `globals.h` (RAM variables), `rom_tables.h` (the original's tables), `str.h` |
| `asm/*.s` | the original's hand-written assembly (*Assembly modules*) |

## Hardware Overview

| Component | Detail |
|-----------|--------|
| **MCU** | Freescale MC9S12D64 (HCS12), 4 MHz crystal, 24 MHz bus (PLL) |
| **CAN** | MSCAN module — connects to Märklin Central Station / track bus |
| **Display** | PT6547 LCD driver IC (Princeton Technology), 304-segment capability, SPI interface |
| **Input** | Pushable rotary encoder (speed knob) + 12 buttons (Lok/OK, Menu/ESC, Headlights, STOP, Aux F1-F8) |
| **Storage** | On-chip EEPROM for persistent config (lok slots, language, calibration) |
| **Track Output** | Timer-driven signal generator for MM/mfx waveforms |

## Firmware Analysis Summary

### Memory Map

| Region | Address Range | Content |
|--------|--------------|---------|
| HCS12 Registers | `0x0000–0x03FF` | Memory-mapped I/O (~200 registers) |
| EEPROM | `0x0800–0x0BFF` | Persistent configuration (1K, mirrored at 0x0C00) |
| RAM | `0x1000–0x1FFF` | Variables, stack (top 0x2000), locomotive slots |
| Main ROM | `0x4000–0xBFFF` | Application code; `0x8000–0xBFFF` is page $3D, mapped for good |
| Fixed ROM | `0xC000–0xF7FF` | Application code, the original's .rodata and identification area (`0xF700–0xF73F`), interrupt trampolines (`0xF740–0xF7FF`) |
| Bootloader | `0xF800–0xFFFF` | Start-up check, CAN update protocol, flash metadata (`0xFF00–0xFF7F`), 64 vectors (`0xFF80–0xFFFF`) pointing at the trampolines (`docs/bootloader.md`) |
| Paged ROM (0x3C) | `0x3C8000+` | Locomotive database and its accessors (`CALL $BB08/$BB0C,$38`) |

### Data Structures

`src/include/lokstack.h` defines the locomotive slots: `lok_record_t` (0x60 bytes, the
stored locomotive: name, protocol, address, mfx UID and configuration addresses, functions)
in `lok_records[]` at `0x13D7`, and `lok_state_t` (0x1C bytes, what changes while it runs:
speed, functions, controller, pending track/CAN/EEPROM work) in `lok_states[]` at `0x17F7`;
`lok_records[LOK_EDIT]` after the slots is the locomotive being added. Compile-time checks
pin both layouts to the original's.

## Documentation (`../doc/`)

| Document | Content |
|----------|---------|
| `60652_Mobilstation1-EN.pdf` | MS1 user manual: controls, display, keys |
| `CAN_Doku_V101_de.pdf` | CAN protocol (German): frame formats, commands, registration |
| `MFX - Trackprotocol.pdf` | mfx track protocol: signal, frames, feedback, configuration blocks |
| `Märklin Motorola Format.mhtml` | MM packet structure, address/speed/function encoding |
| `PT6547.PDF` | PT6547 LCD driver: pins, serial protocol, segment mapping |
| `9S12DJ64DGV1.pdf`, `S12*.pdf` | MC9S12D64 device guide and module manuals (CPU, ECT, MSCAN, SPI, ATD, flash, EEPROM) |

## Protocols Supported

1. **MCAN** (Märklin CAN) — CAN bus protocol for communication with Central Station
   - 8-stage registration handshake with UID exchange
   - Locomotive speed/function/direction commands
   - System commands (emergency stop, track power)
   - Firmware flash update over CAN
2. **mfx** — Märklin digital track protocol (via signal generator)
3. **MM** (Märklin Motorola) — Legacy 18-pulse digital format
4. **DCC**: a slot can hold a DCC locomotive (`LOK_DCC`) and pass it on CAN; the track signal
   generator encodes MM and mfx only (`lok_packets_next` skips other slots)

## Conventions

### Struct Typing Policy

Slot data is reached through `lok_record_t *` / `lok_state_t *` (`&lok_records[slot]`,
`&lok_states[slot]`) with `->field`; no raw offsets like `*(u16 *)(ptr + 0x14)`.

- **The original binary is the reference**: a field is what the original's code does with
  that offset (`../ms1-ghidra` listing and decompilation); `lokstack.h` follows it.
- **Overlapping fields** share a union (`default_address` / `mfx_vmax_cv`), each member
  documented in `lokstack.h`.
- **Byte access within word fields** (big-endian): `HI8(field)` / `LO8(field)` from
  `lokstack.h`, where the original reads or writes one byte of a word, so both compile to the
  same accesses.
- **Named constants**: protocols `LOK_MFX`/`LOK_DCC`/`LOK_MM`, `protocol_flags` bits `LOKP_*`,
  state flags `LOKS_*`.
- **No frame-pointer globals**: `_.frame` (0x1BBD) and `_.tmp` (0x1BB7) are GCC soft
  registers; the original's code there uses them for its locals, which are C locals here.

### Verification
- **All scenarios**: `python3 tools/regress.py` (after `tools/hc12-gcc/run make` and building
  `../ms1-emu`'s `build/difftest` and image) runs every `# run` line of `tools/scenarios/`
  in parallel and prints, per run, the calls that match and differ, and whether the
  whole-firmware comparison holds; then the functions no run calls, and where the rebuild
  spends the most extra time. Exit status 1 on any difference. `--keep DIR` keeps the images,
  reports and CAN logs; run names select runs (`python3 tools/regress.py mfx master`).
- **Function by function** with `../ms1-emu`'s `difftest`: the original runs a scenario, and at
  each call of a function listed in `tools/functions.tsv` the original's and this build's
  function run from the same machine state; return value, RAM, EEPROM and I/O register writes
  must match. `tools/functions.tsv` columns: original entry address, function here, return
  kind, and (optional) stack argument bytes left out of the comparison where the original
  keeps a parameter in its argument slot. `build/ms1.sym` is the symbol table the Makefile
  writes for it. `-n` caps the calls tested per function (default 20): use a large `-n` to
  cover a whole scenario. A single run by hand:

  ```sh
  tools/hc12-gcc/run make
  # the rebuild with the original's parts from the emulator's image (build/ms1.s19 takes
  # them from the device dump, whose configuration area differs)
  python3 tools/mkimage.py build/app.s19 ../ms1-emu/build/ms1_4011.s19 rebuild.s19
  orig=../ms1-emu/build/ms1_4011.s19
  # the master run of drive.txt: the original as slave on the same CAN bus
  ../ms1-emu/build/difftest -t 27 -n 100000 -r 1929:10 -M $orig -A 0=0 \
      -S tools/scenarios/slave.txt -s tools/scenarios/drive.txt \
      $orig rebuild.s19 build/ms1.sym tools/functions.tsv
  ```
  `-r 1929:10` is the CAN transmit ring: data bytes past a frame's length are stale buffer
  contents and not compared. With a mismatch, `-x` prints the addresses each side executed
  in the function; `m68hc11-addr2line -e build/ms1.elf` maps the rebuild's to source lines.
- **Scenarios** (`tools/scenarios/`, the `ms1emu -s` format): each file's header says what it
  covers; its `# run[ LABEL]: OPTIONS` lines are its runs — difftest options, `ORIGINAL` for
  the original's image, other `*.txt` for scenario files (a second MS1's inputs).

  | Scenario | Runs | Covers |
  |---|---|---|
  | `menu.txt` | `menu`, `overload` | boot, NEUE LOK, the menus down to SPRACHE; with a track current over the limit |
  | `drive.txt` | `drive`, `master` | adding a locomotive by address, speed up and down, light, function keys, direction change, reverse, OK, STOP; the functions no scenario reaches, called directly (`-u`); as master of the original running `slave.txt` |
  | `database.txt` | `database` | adding locomotives from the database, an address conflict (ADR.FEHLER), unique names |
  | `slave.txt` | `slave` | slave of the original running `drive.txt`: registration with the master, its locomotive's state, names and parameters, the slave's menus |
  | `twolok.txt` | `twolok` | master with two locomotives, handed over to the original as slave (`slaveswitch.txt`), which then vanishes from the bus: the master's timeout |
  | `slaveswitch.txt` | `slaveswitch` | slave taking the original master's locomotives in turn, driving one, then the CAN cable pulled (KEIN MASTER) |
  | `mfx.txt` | `mfx` | an mfx decoder (`-D`): UID search, SID assignment, configuration read-out, driving (speed, functions, emergency stop, reverse, STOP) |
  | `mfxedit.txt` | `mfxedit` | the mfx locomotive's name, Vmax, acceleration, braking, volume changed (CV writes); ESC out of the database list and the address entry |
  | `mfxfault.txt` | `mfxfault` | mfx decoders with a bad configuration (another address, slot dropped) and without data answers (read-out given up, decoder deleted) |
  | `bootloader.txt` | `bootloader` | OPTIONEN, RESET, PA3+PA7: the jump to the bootloader |

- **Whole firmware**: `tools/regress.py` also runs the rebuild as the whole machine
  (`difftest -W`) and compares what each scenario shows with the original's run: text lines,
  decoder events (`-D`, `../ms1-emu/docs/decoders.md`) and CAN frames, without times and with
  timing-derived numbers masked; CAN frames are compared per direction and ID, since frames
  of different IDs interleave by timing. The rebuild's code is somewhat slower (`difftest -y`; the
  runner prints the functions with the most extra time), so the same events happen
  milliseconds later — by tens of milliseconds over an mfx registration — and as the main
  loop fills command buffer entries at other moments, the order of track packets can differ
  (an MM refresh packet between two mfx frames). Bit timing, pauses and
  feedback windows come from the original's interrupt code and match exactly. By hand:
  `../ms1-emu/build/ms1emu` with the same scenario on both images, comparing LCD
  text and icons (`-l`, `text` lines), CAN frames (`-c`), track output (`-T`, decoded by
  `../ms1-emu/tools/track_decode.py`), decoder events (`-D`), EEPROM.
- **Version 4010**: `python3 tools/regress.py --release 4010` (after `tools/hc12-gcc/run make
  RELEASE=4010`) runs the same scenarios with `build/4010/` against release 4010
  (`--firmware`, default `../original-firmware`), which `tools/mkimage.py --original` makes
  bootable with the device dump's bootloader (*Release images*), with
  `tools/functions_4010.tsv` and the CAN ring at its 4010 address (`-r 191E:10`). ms1-emu reads
  4010's display tables and difftest its end of RAM variables (0x1BAC) by the version word.
  The `overload` run sets ATD0 channel 3, which 4010 does not read; its over-current input is
  PB6 (`press pb6` in a scenario), effective while PB7 is high. By hand:

  ```sh
  fw=../original-firmware
  python3 tools/mkimage.py --release 4010 --original $fw/4010_1-8_mobile-station_A.bin \
      $fw/firmware_20260317_192757_usbdm.s19 orig4010.s19
  python3 tools/mkimage.py --release 4010 build/4010/app.s19 orig4010.s19 rebuild4010.s19
  ../ms1-emu/build/difftest -t 16 -r 191E:10 -s tools/scenarios/menu.txt \
      orig4010.s19 rebuild4010.s19 build/4010/ms1.sym tools/functions_4010.tsv
  ```
- **Calibration**: the images carry the device's calibration words at `0xFF60–0xFF7F` (the
  bootloader's area: `flash_adc*_scale`, `flash_current_limit` in `gcc/ms1.ld`; the track
  current sense scale `flash_adc4_scale` is 0 there). `tools/calpatch.py IN.s19 OUT.s19
  ADDR=VALUE...` overrides words for an experiment (patch both images alike); vary the ADC
  inputs with `-a CH=V` (e.g. `-a 3=200` overload, `-a 0=300 -a 1=300` low track voltage).
- Check a function against the original's decompilation (`../ms1-ghidra/out/ms1_4011_decomp.c`)
  and instruction listing (`../ms1-ghidra/out/listing.tsv`)
- Data the code reads by address stays the original's (see *Image layout*)
