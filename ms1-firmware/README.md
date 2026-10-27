# ms1-firmware — the Märklin Mobile Station 1 firmware in C

A C source of the Märklin Mobile Station 1 (MS1) application firmware, with inline assembly
where the cycles count (the interrupt handlers), built with GCC, that behaves like the original:
verified function by function and as a whole against the original running in the emulator
(`../ms1-emu`). In place of the original's locomotive database it keeps a library of the
user's own locomotives and accessories in flash (NEUE LOK → DATENBANK, LOK ÄNDERN →
SPEICHERN, ZUBEHÖR), drives and programs DCC locomotives beside MM and mfx, and switches DCC
accessories, all from flash page $3C, where the database was (*Image layout*,
`docs/architecture.md` 4.9-4.12 and 11). It builds the
release images from this repository alone (the original's update file loader is in
`third_party/`, see [third_party/README.md](third_party/README.md)).

## Project Goal

Produce a C source of the MS1 firmware that behaves like the original where the original has
the behaviour; it replaces the locomotive database by a device library and adds DCC
(`docs/architecture.md`, 4.9-4.12 and 11). The original binary is the reference: `../ms1-ghidra`
(decompilation, instruction listing) for what the code does and `../ms1-emu` (the emulator
running the original) for how it behaves; comments in the sources are hints, not facts.

- [docs/architecture.md](docs/architecture.md): how the parts of the firmware fit together —
  start-up, scheduling, the track signal (MM, mfx, feedback, DCC), the locomotive slots, CAN, the
  display, keys and knob, EEPROM, the peripherals
- [docs/bootloader.md](docs/bootloader.md): the bootloader and the application — how it starts
  and updates the application, the flash metadata block, and what a modified firmware must keep

## Building

The firmware is built with the GNU 68HC12 toolchain the original was built with (GCC m68hc1x:
same soft-register frame pointer `_.frame`, start-up code and section markers), GCC 3.3.6 +
m68hc1x 3.1 and binutils 2.18 as Debian 7 packaged them, in a container:

```sh
tools/hc12-gcc/run make          # build/app.s19, the release images build/<release>.bin/.bci, and
                                 # with a device dump the flashable build/<release>.s19, build/ms1.s19
tools/hc12-gcc/run make DEBUG=1  # with SCI0 debug output (MS1_DEBUG_SERIAL, SCI0 115200 8N1)
tools/hc12-gcc/run make RELEASE=4010  # version 4010 (releases 4010, c010) in build/4010/
tools/hc12-gcc/run make clean    # all of build/, every version
../ms1-emu/build/ms1emu -i -S build/ms1.sym build/ms1.s19   # run it in the emulator
```

`tools/hc12-gcc/run` builds the `ms1-hc12-gcc` Docker image from `tools/hc12-gcc/Dockerfile` on
first use and runs the command in it, in this directory, with the repository around it mounted
at the same path.

**From outside this repository** (the workspace README's *What you provide*), none of it
needed to build, as `make` variables:

| Variable | Default | Used for |
|---|---|---|
| `RELEASE` | `4011` | the application version built: `4011` in `build/`, `4010` in `build/4010/` (*Release images*) |
| `FIRMWARE` | `../original-firmware` | the directory of the release images and the device dump |
| `ORIGINAL` | `$(FIRMWARE)/firmware_20260317_192757_usbdm.s19` | a device dump (a 4011 device's): the bootloader of the flashable `.s19` images, which are built only when it exists |

**Release images.** Releases 4011 (article 60652) and c011 (60651) have the same code and
differ only in their identification area (`src/sys/ident.c`: the article number, build strings
and words), so one application builds both. Per release (the Makefile's `RELEASES`, named as
the original's update files are: `<code>_<major>-<minor>_<product>`, the version the
Makefile's `VERSION`; its identification part `build/ident/<code>.s19`):
`build/<release>.bin` is the application, page $3C and the release's identification area in
the update file's page order, 0xF800-0xFFFF zero as in a release image; `build/<release>.bci` is
the update file of it: the release files' header, made from the image's identification area,
the version's loader (`third_party/loader/<version>.s19`), then every 0x200-byte slot of the
image below 0xF800 (`../tools/bci.py`; it decodes back to the image, it has not been tried on
a device); with `ORIGINAL`, `build/<release>.s19` is the flashable image with the
dump's bootloader, and `build/ms1.s19` the version's first release.

The application version in INFO (`AP:`) and the CAN identification (word 0xF704,
`AP_VERSION` in `src/sys/ident.c`) is the Makefile's `VERSION`. The version word at 0xF73C and the product string stay the
original release's: the bootloader starts an application by the version word, and both go
out in the CAN identification as the original's do.

Releases 4010 and c010 are application version 4010, for the MS1 hardware without track
current measurement, and `make RELEASE=4010` builds them the same way into
`build/4010/`. The version is one source with `#if MS1_RELEASE == 0x4010` points (in C and
in the inline assembly); `docs/architecture.md` section 12 lists what differs:

| | 4011 | 4010 |
|---|---|---|
| code (`MS1_RELEASE`) | the default | `track_current_monitor`, `ui_task` start-up screen and restart paths (`TRACK_OUTPUT_OFF`), INFO item 9 (`OVC:`), `mfx_frame_begin` 9/11-bit address prefixes (not the mfx format, as the original sends them), `crt_restart`/`enter_bootloader`, `isr_timer_ch4_signal_gen` (the booster line) |
| RAM (`tools/releases.py`) | `src/globals.c` | no variables at 0x10F2-0x10FC; from 0x10FD up 0x0B lower, soft registers at 0x1BAC |
| identification area (`src/sys/ident.c`) | product string "Mobile Station 2 MFx Motorola", version word 0x4011 | "Mobile Station MFx Motorola", 0x4010 |
| function table (difftest) | `tools/functions.tsv` | `tools/functions_4010.tsv`, row for row the same functions |
| update file loader | `third_party/loader/4011.s19` | `third_party/loader/4010.s19` |

The bootloader starts an application only if the version word at 0xF73C agrees with its flash
metadata word at 0xFF1A; the device dump is a 4011 device's (0xC011) and refuses 4010. So the
4010 and c010 `.s19` images (and `build/4010/ms1.s19`) carry the dump's bootloader with the low
byte at 0xFF1B set to 0x10 (`tools/mkimage.py` does so when the check fails, and says so); a
4011 image keeps the bootloader as it is. The firmware shows that word in INFO (`HW:`). The
`.bin` and `.bci` carry no bootloader and are unaffected.

| File | Role |
|---|---|
| `Makefile` | sources, flags (`-m68hcs12 -mshort -O1 -fomit-frame-pointer`, per-function sections, `--gc-sections`), `UNCALLED` (functions the original has but nothing calls, kept with `-u`) |
| `src/sys/crt0.c` | start-up, in the `.crt0` section at 0x4000, laid out as the original's 0x4000-0x40A2 (MMC, PLL, then `crt_restart` = the original's 0x403C: .data copy, .bss clear, stack, `main(1, {"main"})`; `enter_bootloader`) |
| `src/sys/vectors.c` | interrupt trampolines 0xF740-0xF7FF: the bootloader's vectors point at 0xF740 + 3n; unused vectors stop in `unexpected_interrupt` (BGND) |
| `gcc/ms1.ld` | linker script template: memory map (the application up to 0xF67F), the fixed-flash code first (`@FIXED_TEXT@`, checked to end below the window, read-only data above it); symbols for the bootloader's flash metadata words the code reads by name (`docs/bootloader.md`); INCLUDEs `build/page3c_symbols.ld` |
| `gcc/page3c.ld`, `src/include/page3c.h` | page $3C, linked after the application against its symbols, at the linker's banked address 0x100000: the entry table (`src/sys/page3c_entries.c`) at 0x8000 and the functions behind it |
| `tools/banks.py` | the code that must stay in fixed flash, placed first in `build/ms1.ld` (`place`); the application's addresses of the page-$3C entry table, `build/page3c_symbols.ld` (`entries`) |
| `gcc/ident.ld` | the identification area of a release (`src/sys/ident.c`), linked on its own: 0xF700-0xF73F, its strings from 0xF680 |
| `tools/releases.py` | the versions: RAM layout, the per-version files |
| `tools/ramlayout.py` | writes `build/ms1.ld` from the template, placing every RAM variable at its address (below) |
| `gcc/mc9s12d64.h` | register definitions, from the MC9S12DJ64 Device User Guide register map |
| `tools/mkimage.py` | the images: application + page $3C + identification area + CRC, and a dump's bootloader for the `.s19` |
| `tools/regress.py`, `tools/scenarios/`, `tools/functions.tsv`, `tools/functions_4010.tsv`, `tools/calpatch.py` | verification against the original with `../ms1-emu` (*Verification*) |
| `tools/objcmp.py` | two builds compared function by function, for changes meant to keep the code |
| `third_party/` | the original's part the build uses: the update file loader ([third_party/README.md](third_party/README.md)) |

**Image layout.** The application (code, read-only data, the .data image) takes
0x4000-0xF67F; page $3D is mapped into the 0x8000 window, as in the original, except while the
application runs code in page $3C. Two parts are linked on their own:
- page $3C (`src/include/page3c.h`), linked after the application against its symbols. The
  application calls its functions only through the entry table at 0x8000, a JMP per function,
  with GCC's far calls (`CALL`/`RTC`); `tools/banks.py entries` gives it the table's addresses
  (`build/page3c_symbols.ld`). While page $3C is in the window, interrupts stay on: the
  interrupt handlers, the application's functions page-$3C code calls and what these call lie
  in fixed flash, first from 0x4000 on (`tools/banks.py place`, computed from the objects'
  relocations; `gcc/ms1.ld` checks that they end below the window). Its code takes
  0x8000-0xAFFF; 0xB000-0xBFFF is the device library, which the firmware programs at run time
  (`docs/architecture.md`, 11) and the build leaves erased;
- per release the identification area 0xF700-0xF73F with its strings from 0xF680
  (`src/sys/ident.c`), which the bootloader and the CAN identification read by address
  (`docs/bootloader.md`).

`tools/mkimage.py` puts the parts together, with `ORIGINAL`'s 0xF800-0xFFFF (bootloader, flash
metadata, vectors) for the `.s19` images, and sets the word at 0xF73E so that the bootloader's
CRC-16 over page $3C and 0x4000-0xF7FF comes out 0, as the original does. The original's
read-only tables the code uses are C with the original's
contents: the menu texts (`src/ui/lang.c`) and display tables (`src/ui/lcd_tables.c`) global,
the others `static` in the file that reads them.

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

**Inline assembly.** The interrupt handlers are assembly in the C file of their subsystem
(`__asm__` at file scope, each handler in a section `.text.<name>`): GCC's `interrupt`
attribute saves three soft registers on every entry, some 30 bus cycles, and every interrupt
handler delays the track signal interrupt (*Interrupt timing*). The track signal interrupt and
the feedback capture (`src/signal/signal_gen.c`, `src/signal/feedback_capture.c`) keep the
original's instruction timing: when a capture starts and when it samples its inputs counts in
cycles, and the whole-firmware comparison matches the original's track output and feedback
exactly. `src/include/asm.h` gives the assembly the register names of `gcc/mc9s12d64.h` (the
Makefile writes them as `$(B)/mc9s12d64.inc`) and `ASM_NUM`, a C constant's value in an
assembly string. The main loop's code is C, also where GCC's takes longer than the
original's assembly (`difftest -y`): it is not time-critical. The other
inline assembly: `delay_short` (a timed loop, `src/sys/delay.c`),
the page-$3C entry table (`src/sys/page3c_entries.c`), `IRQ_SAVE`/`IRQ_RESTORE`
(`src/include/hcs12.h`), the start-up code and the trampolines (`src/sys/crt0.c`,
`src/sys/vectors.c`). `docs/architecture.md` lists the interrupt handlers.

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
| `src/sys/crt0.c`, `vectors.c` | start-up, restart and the way into the bootloader; the interrupt trampolines |
| `src/sys/ms_tick.c`, `timer_init.c` | the millisecond: its interrupt, the modulus counter, the main loop's pace; timer and ATD set-up |
| `src/sys/delay.c` | busy waits |
| `src/can/can0.c` | the CAN0 controller: its interrupts, set-up, the frame rings |
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
| `src/signal/dcc.c` | DCC packets of the slots: jobs, refresh, POM (page $3C) |
| `src/signal/dcc_service.c` | DCC service mode: CV reads and writes, the acknowledgement (page $3C) |
| `src/ui/prog.c` | LOK ÄNDERN's programming items: CV LESEN, LESEN, SYMBOL, TASTEN (the key table), FAHRSTUFEN (page $3C) |
| `src/ui/accessory.c` | DCC accessory devices: the ZUBEHÖR menu and editor, accessory mode, the accessory packets (page $3C) |
| `src/signal/mfx_feedback.c` | evaluating a captured mfx feedback window (1 bit, data) |
| `src/signal/feedback_capture.c` | capturing an mfx feedback window: the timer channel 0 interrupt, its start and stop |
| `src/signal/signal_gen.c` | the track signal generator: the timer channel 4 interrupt, and the main-loop stages that turn command entries into send-buffer pairs into staging entries; the command entry writers it owns |
| `src/signal/track_monitor.c` | track current, overload shutdown, operating counters |
| `src/lok/lokstack.c` | slot lookups, CAN objects of a slot, address conflicts, unique names |
| `src/lok/lok_functions.c` | a locomotive's functions: CAN object numbers and keys |
| `src/sys/page3c_entries.c` | the page-$3C entry table (`src/include/page3c.h`) |
| `src/lok/library.c` | the device library in flash: saving, listing, the flash commands from RAM (page $3C) |
| `src/ui/ui_control.c` | the UI state machine: knob, keys, menus |
| `src/ui/ui_mode.c` | switching the operating mode |
| `src/ui/screen_text.c`, `lcd_text.c` | the text line: composing it per screen, placing and scrolling it; menu levels |
| `src/ui/status_icons.c`, `lok_icons.c` | status icons, speed bar; the locomotive's function icons and symbol |
| `src/ui/encoder.c` | the knob: sampling the encoder, its count |
| `src/ui/ui_helpers.c` | knob helpers; the name editor's characters |
| `src/ui/lang.c`, `lcd_tables.c` | the menu texts per language; the display's glass maps, font and images |
| `src/ui/lcd.c` | the display: the SPI0 interrupt sending `lcd_buf`, SPI set-up, text, speed bar and icons drawn into `lcd_buf` |
| `src/sys/ident.c` | the identification area of each release (linked on its own, `gcc/ident.ld`) |
| `src/lib/str.c` | string and number formatting, display character set conversion |
| `src/lib/runtime.c` | the original's compiler helper the C code calls by name |
| `src/hal/gpio_buttons.c`, `serial_debug.c`, `eeprom.c` | key reads and scan; SCI0 debug output (`DEBUG=1`); the EEPROM's erase and program commands |
| `src/include/` | `hcs12.h` (integer types, registers, interrupt masking), `asm.h` (*Inline assembly*), `page3c.h` (the functions in page $3C), `lokstack.h` (slot records and states), `track_cmd.h` (the signal generator, command buffer writers), `lcd.h`, `ui.h`, `mcan.h` (also the CAN0 driver), `delay.h`, `globals.h` (RAM variables), `rom_tables.h` (the shared tables, the bootloader's metadata words), `str.h` |

## Hardware Overview

| Component | Detail |
|-----------|--------|
| **MCU** | Freescale MC9S12D64 (HCS12), 4 MHz crystal, 24 MHz bus (PLL) |
| **CAN** | MSCAN module — connects to Märklin Central Station / track bus; L9615 CAN transceiver |
| **Display** | PT6547 LCD driver IC (Princeton Technology), 304-segment capability, SPI interface |
| **Input** | Pushable rotary encoder (speed knob) + 12 buttons (Lok/OK, Menu/ESC, Headlights, STOP, Aux F1-F8) |
| **Storage** | On-chip EEPROM for persistent config (lok slots, language, calibration) |
| **Track Output** | Timer-driven signal generator for MM, mfx and DCC waveforms |
| **mfx feedback** | PT2579(S), an RDS demodulator: its clock, data and quality outputs fit the three inputs the feedback capture samples (PT0, PT1, PB5; `src/signal/feedback_capture.c`) |
| **Other logic** | HEF4555B (dual 1-of-4 decoder) and LM339 (quad comparator), at the track output and feedback; their wiring is not traced, so the firmware assigns them no role |

## Firmware Analysis Summary

### Memory Map

| Region | Address Range | Content |
|--------|--------------|---------|
| HCS12 Registers | `0x0000–0x03FF` | Memory-mapped I/O (~200 registers) |
| EEPROM | `0x0800–0x0BFF` | Persistent configuration (1K, mirrored at 0x0C00) |
| RAM | `0x1000–0x1FFF` | Variables, stack (top 0x2000), locomotive slots |
| Main ROM | `0x4000–0xBFFF` | Application code; `0x8000–0xBFFF` is page $3D, mapped for good |
| Fixed ROM | `0xC000–0xF7FF` | Application code and data up to `0xF67F`, the identification strings and area (`0xF680–0xF73F`), interrupt trampolines (`0xF740–0xF7FF`) |
| Bootloader | `0xF800–0xFFFF` | Start-up check, CAN update protocol, flash metadata (`0xFF00–0xFF7F`), 64 vectors (`0xFF80–0xFFFF`) pointing at the trampolines (`docs/bootloader.md`) |
| Paged ROM (0x3C) | `0x3C8000+` | The entry table at 0x8000 and the functions behind it (`src/include/page3c.h`), up to 0xAFFF; the device library at 0xB000-0xBFFF (`src/lok/library.c`) |

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
4. **DCC** (RCN-211, -212, -214, -216): DCC locomotives in the slots (`LOK_DCC`, also passed on
   CAN), on the track beside MM and mfx: speed in 128, 28 or 14 steps, F0-F15, CV writes on
   the main (POM); NEUE LOK → DCC adds one by address; service mode reads and writes CVs and
   the address after "PROG LOK?"; accessory decoders (RCN-213) switched from ZUBEHÖR, their
   address written in service mode (`docs/architecture.md`, 4.9-4.12)

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
  keeps a parameter in its argument slot, and `pN` for a function that gets strings: a call
  whose string (in D or the first N bytes of stack arguments) is in the original's read-only
  data, which this build keeps elsewhere, is skipped and counted as such; the whole-firmware
  comparison covers those calls. `build/ms1.sym` is the symbol table the Makefile
  writes for it. `-n` caps the calls tested per function (default 20): use a large `-n` to
  cover a whole scenario. A single run by hand:

  ```sh
  tools/hc12-gcc/run make
  # the rebuild with the bootloader of the emulator's image of the original
  orig=../ms1-emu/build/ms1_4011.s19
  python3 tools/mkimage.py --bootloader $orig --s19 rebuild.s19 \
      build/app.s19 build/page3c.bin build/ident/4011.s19
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
  the original's image, other `*.txt` for scenario files (a second MS1's inputs) — and its
  `# expect[ LABEL]: OPTIONS` lines runs of behaviour the original does not have (*Expected
  output*). A header line must not itself start with `# run` or `# expect`.

  | Scenario | Runs | Covers |
  |---|---|---|
  | `menu.txt` | `menu`, `overload` | boot, NEUE LOK, the menus down to SPRACHE; with a track current over the limit |
  | `drive.txt` | `drive`, `master` | adding a locomotive by address, speed up and down, light, function keys, direction change, reverse, OK, STOP; the functions no scenario reaches, called directly (`-u`); as master of the original running `slave.txt` |
  | `conflict.txt` | `conflict` | two locomotives added on one address (ADR.FEHLER, address conflict), unique names |
  | `slave.txt` | `slave` | slave of the original running `drive.txt`: registration with the master, its locomotive's state, names and parameters, the slave's menus |
  | `twolok.txt` | `twolok` | master with two locomotives, handed over to the original as slave (`slaveswitch.txt`), which then vanishes from the bus: the master's timeout |
  | `slaveswitch.txt` | `slaveswitch` | slave taking the original master's locomotives in turn, driving one, then the CAN cable pulled (KEIN MASTER) |
  | `mfx.txt` | `mfx` | an mfx decoder (`-D`): UID search, SID assignment, configuration read-out, driving (speed, functions, emergency stop, reverse, STOP) |
  | `mfxedit.txt` | `mfxedit` | the mfx locomotive's name, Vmax, acceleration, braking, volume changed (CV writes); ESC out of the address entry |
  | `mfxfault.txt` | `mfxfault` | mfx decoders with a bad configuration (another address, slot dropped) and without data answers (read-out given up, decoder deleted) |
  | `bootloader.txt` | `bootloader` | OPTIONEN, RESET, PA3+PA7: the jump to the bootloader |
  | `dcc.txt` | `dcc` (expect) | a DCC locomotive added (NEUE LOK → DCC) and driven as in `drive.txt`, read by a DCC decoder |
  | `dccpom.txt` | `dccpom` (expect) | a DCC locomotive's VMAX, a REG CV and RESET written on the main (POM) |
  | `dccmix.txt` | `dccmix` (expect) | MM, DCC and mfx on one track: an MM and a DCC locomotive driven while an mfx decoder registers |
  | `library.txt` | `library` (expect) | a DCC locomotive saved into the device library (SPEICHERN), deleted, added again from NEUE LOK → DATENBANK and driven |
  | `service.txt` | `service`, `service_noack` (expect) | DCC service mode: a new address written (PROG LOK?) and driven, CV8 read; with a decoder that never acknowledges (FEHLER) |
  | `read.txt` | `read` (expect) | LESEN: the address of a decoder with a long address (1027) read into the locomotive |
  | `prog.txt` | `prog` (expect) | TASTEN (F3 on the second left key, its speaker icon, momentary), SYMBOL, FAHRSTUFEN 28, then driven |
  | `accessory.txt` | `accessory` (expect) | ZUBEHÖR: an accessory device made (address, the icons beside two keys), saved and used: outputs switched by the function keys |
  | `accprog.txt` | `accprog` (expect) | ZUBEHÖR → PROG: an accessory decoder's address written in service mode (CV1, CV9), then the device saved and used |

- **Expected output**: an `# expect` run runs the rebuild alone (`difftest -W`) and compares
  what it shows — the observables of the whole-firmware comparison below — with
  `tools/scenarios/LABEL.expected` (LABEL: the run's label, else the scenario's name; version
  4010 takes `LABEL.4010.expected` where that exists). `python3 tools/regress.py --update RUN`
  writes the file from the rebuild; read its diff before committing it. Decoder events of
  `dcc:` and `dccacc:` decoders (`../ms1-emu/docs/dcc-decoder.md`) are observables like MM's
  and mfx's.
- **Whole firmware**: `tools/regress.py` also runs the rebuild as the whole machine
  (`difftest -W`) and compares what each scenario shows with the original's run: text lines,
  decoder events (`-D`, `../ms1-emu/docs/decoders.md`) and CAN frames, without times and with
  timing-derived numbers masked (the rebuild's display is read back with the tables its
  symbol table names, `difftest -W`, `ms1emu -S`); decoder events are compared per decoder, a
  run of a decoder's speed events in one direction as its last (the steps a ramp passes depend
  on when the knob is read), and CAN frames per direction and ID, since events of different
  decoders and frames of different IDs interleave by timing, and a frame repeated unchanged
  counts once: how often a state repeats depends on timing (the master's mfx status frame
  after each mfx search that finds no decoder: the search moves with the main loop's timing,
  by up to tens of milliseconds, so more or fewer fit in before the slave's cable is pulled in
  `twolok`). These runs, and the function test, take the rebuild with the original's
  application version at 0xF704 (INFO `AP:`, CAN identification; `tools/mkimage.py
  --version-from`); the expect runs take it as built. The rebuild's code is somewhat slower
  (`difftest -y`; the runner prints the functions with the most extra time), so the same events happen
  milliseconds later — by tens of milliseconds over an mfx registration — and as the main
  loop fills command buffer entries at other moments, the order of track packets can differ
  (an MM refresh packet between two mfx frames). Bit timing, pauses and
  feedback windows come from the interrupt code, which keeps the original's instruction timing
  (*Inline assembly*), and match exactly. By hand:
  `../ms1-emu/build/ms1emu` with the same scenario on both images, comparing LCD
  text and icons (`-l`, `text` lines), CAN frames (`-c`), track output (`-T`, decoded by
  `../ms1-emu/tools/track_decode.py`), decoder events (`-D`), EEPROM.
- **Interrupt timing**: `../ms1-emu/build/ms1emu -I` (for the rebuild with `-S build/ms1.sym`)
  or `difftest -W -I` prints after a run each interrupt's time, the track signal interrupt's
  latency and slack, the longest stretch with interrupts masked, and the fill of the staging
  ring (`../ms1-emu/src/irqstat.c`); run the same scenario on the original and compare. The
  track signal interrupt (timer channel 4) has the least to spare: its compares come as little
  as 10 µs apart and its slack falls to about 3 µs in the runs of `mfx.txt` and `mfxedit.txt`.
  So code that masks interrupts, and every other interrupt, must not take longer than the
  original's, and the main-loop stages of the signal generator must keep the staging ring from
  running empty: in the runs of `tools/scenarios/` at least 52 entries (about 3 ms of signal)
  are ahead of the interrupt, in the original and in this build.
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
  python3 tools/mkimage.py --release 4010 --bootloader orig4010.s19 --s19 rebuild4010.s19 \
      build/4010/app.s19 build/4010/page3c.bin build/4010/ident/4010.s19
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
- The tables hold the original's contents (*Image layout*); data read by address is only the
  identification area (`docs/bootloader.md`)
