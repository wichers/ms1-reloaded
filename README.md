# ms1-reloaded

An open re-implementation of the firmware of the **Märklin Mobile Station 1** (MS1, articles
60651/60652), the handheld controller for Märklin model railways, and the tools that check it
against the original: an emulator of the MS1 that runs the unmodified firmware, a Ghidra
analysis of it, and CAN master libraries that drive MS1 handhelds. The firmware rebuild is
verified function by function and as a whole against the original running in the emulator,
and builds the official release images; it is the base for new firmware for this hardware.

| Project | What it is |
|---|---|
| [`ms1-firmware/`](ms1-firmware/README.md) | The firmware in C, with inline assembly for its interrupt handlers (the original's locomotive database and update file loader are in [`third_party/`](ms1-firmware/third_party/README.md)), built with GCC for the MC9S12D64; the release images 4011, c011, 4010 and c010; verification against the original (`tools/regress.py`); [architecture](ms1-firmware/docs/architecture.md), [bootloader interface](ms1-firmware/docs/bootloader.md) |
| [`ms1-emu/`](ms1-emu/README.md) | MC9S12D64 + MS1 board emulator: CPU, timers, ATD, SPI/LCD, MSCAN on a real or virtual CAN bus, EEPROM, track output with MM and mfx decoders; `difftest` runs a rebuild against the original |
| [`ms1-ghidra/`](ms1-ghidra/README.md) | Ghidra project of the original application: HCS12 language fixes, memory model, function boundaries, the rebuild's names; exports the listing and pseudocode the other projects use |
| [`ms1-master/`](ms1-master/README.md) | C99 CAN master libraries: M-CAN for MS1 handhelds as slaves, and the CS2 protocol |
| [`tools/bci.py`](tools/bci.py) | decode and encode the firmware update files (`.bci`) |

## Hardware

| Part | Detail |
|---|---|
| MCU | Freescale MC9S12D64 (HCS12): 64 KiB flash in four 16 KiB pages, 4 KiB RAM, 1 KiB EEPROM, 24 MHz bus |
| Display | PT6547 LCD driver on SPI0: a 38×5 text matrix, icons, speed bar |
| Input | pushable rotary knob, 12 keys |
| CAN | MSCAN0, 250 kbit/s, 29-bit IDs (M-CAN): master of other MS1s, or slave of an MS1, CS1 or ECoS |
| Track | timer-driven H-bridge signal for MM and mfx (as master), mfx feedback receiver |

Firmware releases (update files `<release>_1-8_mobile-station[-2]_A.bci`): 4011 (60652) and
c011 (60651) are the same code with their own data; 4010 (60652) and c010 (60651) are its
version for the hardware without track current measurement
([architecture](ms1-firmware/docs/architecture.md), section 11).

## What you provide

The firmware builds from this repository alone: the parts of the original it uses are in
`ms1-firmware/third_party/` ([its README](ms1-firmware/third_party/README.md)). To run the
original in the emulator, analyse it and verify the rebuild against it, the tools read the
original firmware and the vendors' documents from next to the projects; `.gitignore` keeps
them out:

| Path | Content | Used by |
|---|---|---|
| `original-firmware/<release>.bci` | the firmware update files: `4011_1-8_mobile-station-2_A`, `c011_1-8_mobile-station-2_A`, `4010_1-8_mobile-station_A`, `c010_1-8_mobile-station_A`, which the Central Station 1 "Reloaded" updates carry ([archive.org item](https://archive.org/details/marklin-cs1-reloaded_202605); its first update unpacks with [cs_ecos_bci_decode.py](https://gist.github.com/wichers/41910ed73a28dc50e1efbce4e41ddd04), later ones with the keys it brings) | `tools/bci.py decode` → `<release>.bin`; `tools/bci.py loader` → `ms1-firmware/third_party/loader/` |
| `original-firmware/<release>.bin` | the decoded 64 KiB images (pages in file order $3C, $3E, $3D, $3F; no bootloader) | `ms1-emu`, `ms1-ghidra`, `ms1-firmware` (`tools/regress.py`) |
| `original-firmware/firmware_20260317_192757_usbdm.s19` | a dump of a device's flash, read over BDM (USBDM): supplies the bootloader, which no update file carries | `ms1-emu`, `ms1-firmware` (the flashable `.s19` images, `tools/regress.py`) |
| `doc/` | datasheets (MC9S12D64 and its modules, PT6547), the MS1 manual, the M-CAN (`CAN_Doku_V101_de.pdf`) and CS2 CAN documents, the mfx and MM track formats; page images in `doc/pages/` | reading |
| `ms1-ghidra/ida_export/ms1_firmware.*` | an IDA export of the device dump (`ms1-ghidra/ida_export/export_ida.py`) | `ms1-ghidra` import |
| `ms1-ghidra/out/listing.tsv`, `ms1_4011_decomp.c` | the original's instructions and pseudocode (`./ghidra.sh decompile`) | `ms1-emu` (`make check`); reading, beside the decompilation |

## Getting started

```sh
cd ms1-firmware && tools/hc12-gcc/run make          # releases 4011, c011: build/<release>.bin, .bci
tools/hc12-gcc/run make RELEASE=4010                # releases 4010, c010 in build/4010/
```

With the original firmware next to the projects (*What you provide*):

```sh
for f in original-firmware/*.bci; do python3 -I tools/bci.py decode "$f" "${f%.bci}.bin"; done
cd ms1-emu && make && make build/difftest && python3 -I tools/compose_image.py \
    ../original-firmware/4011_1-8_mobile-station-2_A.bin \
    ../original-firmware/firmware_20260317_192757_usbdm.s19 build/ms1_4011.s19
build/ms1emu -i build/ms1_4011.s19                  # the original, interactive
cd ../ms1-firmware && tools/hc12-gcc/run make       # now also the flashable build/<release>.s19
../ms1-emu/build/ms1emu -i -S build/ms1.sym build/ms1.s19   # the rebuild, interactive
python3 tools/regress.py                            # the rebuild against the original
cd ../ms1-ghidra && ./ghidra.sh import && ./ghidra.sh names && ./ghidra.sh decompile
```

Requirements: Docker (the 68HC12 GCC toolchain runs in a container) and Python 3 to build; a C
compiler for `ms1-emu` and `ms1-master`; Ghidra 12.1 with JDK 21 for `ms1-ghidra` (see its README
for building the HCS12 language).

## License

Apache License 2.0 (`LICENSE`, `NOTICE`), except `ms1-firmware/third_party/`, which holds
parts of the original firmware ([its README](ms1-firmware/third_party/README.md)).

Märklin, Mobile Station, Central Station, Trix and mfx are trademarks of Gebr. Märklin & Cie.
GmbH; ECoS is a trademark of ESU electronic solutions ulm. This project is not affiliated with
or endorsed by either company.
