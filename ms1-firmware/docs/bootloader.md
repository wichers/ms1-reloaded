# The bootloader and the application

The MS1's bootloader occupies 0xF800-0xFFFF of flash page $3F. It is not part of this source:
the build takes it from a device dump (`ORIGINAL`, README *Release images*), and a firmware
update file does not carry it, so an update never changes it. It starts the application,
receives firmware updates over CAN, and holds the device's identity and calibration.

This document is the contract between the two: what any firmware built here, including a
modified one, must keep to start and to stay updatable. The facts come from the original
bootloader as `../ms1-ghidra` analyses it (its `bootloader_*` functions in
`docs/firmware-map.md`, the instructions in `out/listing.tsv`).

## Layout of 0xF800-0xFFFF

| Range | Content |
|---|---|
| 0xF800-0xFDDF | code (`bootloader_hw_init`, `bootloader_crc_check_and_dispatch`, the CAN update protocol from `bootloader_flash_handler`), its strings and data |
| 0xFDE0-0xFEFF | unused (0xFF) |
| 0xFF00-0xFF7F | the metadata block (below) |
| 0xFF80-0xFFFF | the vector table: vector n (0xFF80 + 2n) points at 0xF740 + 3n, in the application; reset (0xFFFE) points at 0xF800 |

## Reset

1. `bootloader_hw_init` (0xF800) sets up the memory map (INITRM, INITRG, INITEE, MISC: write-once
   in normal mode, so the application's later writes leave them), maps page $3D, engages the PLL
   for a 24 MHz bus (the sequence `_start` in `src/sys/crt0.c` repeats), masks interrupts, drives
   PORTE bit 2 low, turns off the timer channel 4 interrupt and makes timer channel 3 an input
   capture (4011's booster line off). It uses RAM from 0x1800 and the stack from 0x2000.
2. `bootloader_crc_check_and_dispatch` (0xF877) checks the application:
   - the application version word at 0xF73C against the hardware code at 0xFF1A: the low bytes
     equal, the low nibbles of the high bytes equal, and every bit 5-6 of the high byte that the
     application word sets also set at 0xFF1A (`bootloader_accepts` in `tools/mkimage.py`);
   - a CRC-16 over page $3C 0x8000-0xBFFF, then 0x4000-0xF7FF with page $3D in the window: it
     must come out 0 (the algorithm: `tools/mkimage.py`, which sets the compensation word
     0xF73E).
3. Both pass: `JMP 0xF7FD`, the application's last trampoline, which jumps to `_start`. Either
   fails: update mode (0xF8F6).

There is no key or CAN check at reset: an application that passes both checks always starts.

## Update mode

At 0xF8F6 the bootloader enables CAN0 with the bus clock as its clock source, 250 kbit/s at the
24 MHz bus (BTR0 0xC3, BTR1 0xFE: prescaler 4, 24 time quanta per bit), and its own acceptance
filters. It announces itself with the UID (0xFF10-0xFF13) and waits for an update file's blocks
(file format: `../tools/bci.py`; the master's side of the protocol: `../ms1-master`, flashing).
It does not program the flash itself: it decodes the file's first blocks, the update file's
loader, into RAM 0x1000-0x17FF, and once the four 0x200-byte parts (dest_keys 0x00-0x06) are in
and their CRC-16 comes out 0, calls 0x1000 with the file offset of the next block: the rest
of the file is the loader's. An update
file therefore brings its own loader: `build/<release>.bci` carries the original's
(`third_party/loader/`, what it does: `../tools/bci.py`). That loader erases the application
version word first and programs it last, so an interrupted update leaves no application the
checks above start. It also erases EEPROM 0x0800-0x0803: after an update the application finds
`EE_LANGUAGE` erased and rewrites the EEPROM from its defaults (`src/include/eeprom_config.h`).
It never returns to the application; it leaves update mode only through a reset, after which
the checks above start a valid application again (power off and on is enough).

| Way in | Condition |
|---|---|
| reset | the version word or the CRC check fails: no application, an interrupted update, an application for other hardware |
| `enter_bootloader` from the options menu | the RESET item with PA3 and PA7 held (`porta_buttons_raw` 0x88, `src/ui/ui_control.c`; scenario `tools/scenarios/bootloader.txt`) |
| `enter_bootloader` from CAN | a master's registration answer for this device's UID with bit 0 of its last data byte set (`src/can/mcan.c`) |

`enter_bootloader` (`src/sys/crt0.c`, the original's 0x407C) masks interrupts, turns off the timer
channel 4 interrupt and the booster line, drives PORTE bit 2 low, clears RAM 0x1000-0x1FFF,
sets the stack to 0x2000 and jumps through the pointer at 0xFF70 (to 0xF8F6 if it is 0). It
bypasses `bootloader_hw_init`, so update mode runs on the clock the application left: its CAN
bit rate is right only at the 24 MHz bus.

## What the application takes from the bootloader

No code: the application calls no bootloader routine. It takes the entry of update mode
(0xFF70, above) and reads the metadata block. Its own restarts (`crt_restart`) stay inside the
application; the original's restart through the reset vector (0x4077 in `src/sys/crt0.c`) is
never called.

## The metadata block

0xFF00-0xFF7F, words big-endian. Names are the symbols in `gcc/ms1.ld` (declared in
`src/include/rom_tables.h`); the other words the code reads through their address.
"identification i/s" is item i, sub-item s of the M-CAN identification group (group 1,
`src/can/mcan_object.c`); "registration step k" is the data of `registration_data` in
`src/can/mcan.c`.

| Address | Content | Read by |
|---|---|---|
| 0xFF00 | ASCII `ESU 2004` | nothing |
| 0xFF10, 0xFF12 | `flash_uid_hi`, `flash_uid_lo`: the device's 32-bit UID | M-CAN registration and requests (`src/can/mcan.c`), `can_device_uid` (`app_main_init`), the mfx central frame (`track_filler`), INFO `U…`, the bootloader's update mode |
| 0xFF14-0xFF17 | manufacturer: 0x97 (151) is ESU's NMRA manufacturer ID (`MS1_VENDOR_ESU` in `../ms1-master`) | identification 1/1, registration step 5 |
| 0xFF18 | `flash_hw_version`: hardware version, major.minor | INFO `HW:`, identification 1/0, registration step 5, the choice of `encoder_debounce_mask` (`app_main_init`) |
| 0xFF1A | `flash_hw_code`: the hardware code the application version word must match (*Reset*) | the bootloader's check, INFO `HW:`, identification 1/0, registration step 5 |
| 0xFF1C-0xFF23 | four words | identification 5/0, 5/1 |
| 0xFF24 | bootloader version, major.minor | INFO `BL:`, identification 2/0, registration step 6 (with 0xFF26) |
| 0xFF28-0xFF2B | two words | identification 2/1 |
| 0xFF2C | `flash_compat_flags` | the choice of `encoder_debounce_mask` (`app_main_init`) |
| 0xFF50-0xFF57 | pointers to the bootloader's strings: product name, build date, author, copyright | identification 1/2 sends the product name |
| 0xFF60, 0xFF62 | `flash_adc0_scale`, `flash_adc1_scale`: track and decoder voltage scales | `adc_read_and_scale_voltages` |
| 0xFF64 | `flash_encoder_step` (byte): the knob step; 0 keeps the default | `timer_atd_init` |
| 0xFF66, 0xFF68 | `flash_adc3_scale`, `flash_adc4_scale`: the track current sense scales | `track_current_monitor` (version 4011) |
| 0xFF70 | the entry of update mode | `enter_bootloader` |
| 0xFF7A | `flash_current_limit`: the over-current limit, mA | `track_current_monitor` (4011); `signal_gen_init` (4010: the comparator threshold); INFO item 9 |

What belongs to what:
- **The device:** the UID is unique per device. The calibration words (0xFF60-0xFF7A) are
  presumably set per device at the factory; a single dump cannot tell per device from per
  batch.
- **The hardware variant:** 0xFF18 and 0xFF1A. The low byte of 0xFF1A must equal the
  application version word's: 0x11 runs 4011 and c011, 0x10 runs 4010 and c010 (both releases of
  a pair carry the same word, 0x4011 or 0x4010). This keeps version 4010, which drives the
  booster on PB7 and reads over-current on PB6, off hardware built for 4011, and the reverse.
- **The bootloader build:** its code, version (0xFF24) and strings.

## The application's side: 0xF700-0xF7FF

| Address | Content | Read by |
|---|---|---|
| 0xF700-0xF70F | the application's identification words (`struct ms1_ident` in `src/sys/ident.c`): 0xF702 its version word, 0xF704 its version (major.minor; `AP_VERSION`), 0xF706 and 0xF708 per release build, 0xF70E the hardware code of its article | identification 3/0-3/2, 4/0; registration steps 6 and 7; INFO `AP:` |
| 0xF720 | pointers to the application's strings | identification 3/3 sends the first, the product name |
| 0xF73A | pointer to the article number string | INFO `AN:` |
| 0xF73C | the application version word | the bootloader's check |
| 0xF73E | the CRC compensation word | the bootloader's CRC |
| 0xF740-0xF7FF | the trampolines (`src/sys/vectors.c`): a 3-byte `JMP` per vector, the last (0xF7FD) to `_start` | the bootloader's vector table and its jump into the application |

`src/sys/ident.c` holds 0xF700-0xF73F of each release, with the original release's values,
linked on its own with its strings from 0xF680 (`gcc/ident.ld`); `tools/mkimage.py` computes
0xF73E; the build provides the trampolines.

## Keeping a modified firmware startable and updatable

1. **Trampolines:** keep `src/sys/vectors.c`'s table at 0xF740 (the `tramp` region of `gcc/ms1.ld`), one
   3-byte `JMP` per vector in vector order, the last one to `_start`. An interrupt the firmware
   enables needs its handler there.
2. **Version word:** build the version for the device's hardware variant (`RELEASE`), so that
   0xF73C passes that device's 0xFF1A (*What belongs to what*).
3. **CRC:** let `tools/mkimage.py` write 0xF73E; every image the Makefile builds goes through it.
   The application must end below the identification strings (the `rom` region of
   `gcc/ms1.ld`; `tools/mkimage.py` checks it). Flash the application programs at run time
   is in the CRC too: the device library (`src/lok/library.c`, page $3C 0xB000-0xBFFF) ends
   every 64-byte row in a correction word that makes the row count as an erased one, so the
   CRC holds whatever the library contains. A row cut off by a power loss breaks it; the
   bootloader then waits in update mode, and an update (which erases the library) recovers.
4. **A way into update mode:** the bootloader enters it at reset only when a check fails. A
   firmware that passes the checks but hangs, or has neither the menu nor the CAN path to
   `enter_bootloader`, can then be recovered only over BDM. Enter it with the 24 MHz bus clock
   that `_start` sets up.
5. **The metadata block is read-only and belongs to the device.** Flashing a whole image over
   BDM replaces 0xF800-0xFFFF with `ORIGINAL`'s: read the target device's own flash first and
   build with it as `ORIGINAL`, so that it keeps its UID, calibration and hardware code.
6. **The 0xFF1B adjustment:** `tools/mkimage.py` changes the low byte of 0xFF1A when `ORIGINAL`'s
   bootloader refuses the application (README *Release images*). That lifts the hardware check
   above: flash such an image only onto hardware of the application's own variant.
