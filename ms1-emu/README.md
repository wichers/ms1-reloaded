# ms1-emu — Märklin Mobile Station 1 emulator

Runs the unmodified MS1 firmware (MC9S12D64, HCS12) on an emulated CPU and microcontroller, with
the MS1 board around it, so the firmware can be observed and tested without hardware.

Status: the CPU core is complete and checked (see *Checks*), and the MC9S12D64 and MS1 board
models run the unmodified 4011 image: the bootloader verifies the application CRC and starts
it, and the application drives the LCD itself — all segments on during start-up, then its
menu (`NEUE LOK`) — probes the CAN bus and generates the track signal (MM and mfx). It can be
used interactively in a terminal (`-i`) or driven from a script, and attached to a CAN bus
(SocketCAN or a local UDP bus shared with other processes), and given locomotive decoders on
the track (`-D`): MM decoders that act on its packets, and mfx decoders that answer through the
modelled feedback receiver, so the firmware registers them and reads their configuration. As a
CAN slave it works with the `ms1-master` library as master (see *CAN*). Not there yet: EEPROM
persistence across runs.

## Usage

```sh
make                      # build build/ms1emu (see Makefile targets)
python3 -I tools/compose_image.py ../original-firmware/4011_1-8_mobile-station-2_A.bin \
    ../original-firmware/firmware_20260317_192757_usbdm.s19 build/ms1_4011.s19
build/ms1emu -i build/ms1_4011.s19          # interactive
build/ms1emu -t 10 -p 1 build/ms1_4011.s19    # batch: run 10 s, profile 1 s
make check                # CPU tests and the decoder check against the Ghidra listing
make diff                 # differential CPU test against sim6812
```

### `ms1emu` options

| Option | Effect |
|---|---|
| `-i` | interactive (below); `-t`, `-s`, `-p` do not apply |
| `-t SECONDS` | emulated time to run (default 1) |
| `-b ADDR` | breakpoint at a CPU address (hex; the PPAGE window is whatever is mapped), repeatable |
| `-p SECONDS` | after the run, sample the PC every 5 µs for this long and print the hottest addresses |
| `-e FILE` | EEPROM contents, 1024 bytes (default: erased, all `$FF`) |
| `-n N` | instructions of trace printed when the run stops (default 24, at most 4096) |
| `-q` | no diagnostics on stderr |
| `-d ADDR:LEN` | dump memory (hex) after the run, repeatable |
| `-l` | print the LCD after the run |
| `-c FILE` | log CAN frames (`-` = stdout): time, `TX`/`RX`, 29-bit ID, DLC, data |
| `-s FILE` | input script (below) |
| `-T FILE` | log every change of the track outputs: time (s), PT3, PT4, PT5 levels |
| `-I` | interrupt timing, after the run: per interrupt vector the bus cycles from entry to the end of its `RTI` (nested interrupts included); for the track signal interrupt (timer channel 4) the latency after its compare, the slack before the next compare (≤ 0: a compare passed before it was set up) and the shortest compare interval; the longest stretch with interrupts masked outside interrupts and where it starts; at the track signal interrupt's entries, the fewest entries and the least signal time in the firmware's staging ring, and how often it was empty (the interrupt then plays an old entry) (`src/irqstat.c`) |
| `-S SYMBOLS` | the image's symbol table (`nm -n`, e.g. `../ms1-firmware/build/ms1.sym`), for the firmware rebuild: the LCD is read back with the tables it names (`board_lcd_symbols`) instead of the original's |
| `-C BUS` | attach a CAN bus: a SocketCAN interface (`can0`, `vcan0`) or `udp:PORT` (below) |
| `-r` | run no faster than real time (needed when sharing a bus with real-time partners) |
| `-a CH=VALUE` | ATD0 channel input, 10-bit (default 512 on every channel); `-a 0=0` starts the MS1 as a CAN slave |
| `-D SPEC` | attach a locomotive decoder to the track, repeatable: `mm:ADDR[,mm1][,packets]` or `mfx:UID[,name=NAME][,symbol=SYMBOL][,frames]`. It logs what it does (registration, CV reads, speed, functions) and prints an analysis of the signal after the run ([docs/decoders.md](docs/decoders.md)) |

Interactive mode runs in step with the wall clock (or as fast as possible, `f`), redraws the
LCD every 50 ms with the text it reads, the lit icons and the state of each `-D` decoder, and
maps keys to the controls; a terminal reports no key releases, so each button is released
150 ms after its key:

| Key | Control |
|---|---|
| `←`/`→`, `-`/`+` | turn the speed knob one detent |
| space | push the knob |
| enter, `o` | Locomotive OK |
| `m`, esc | Menu/ESC |
| `s` | STOP |
| `l` | headlights |
| `1`–`8` | PORTA inputs PA0–PA7 |
| `f` | toggle fast / real time |
| `q`, ctrl-C | quit |

Input script lines, in time order (`#` starts a comment):

| Line | Effect |
|---|---|
| `SECONDS press BUTTON` / `SECONDS release BUTTON` | `BUTTON` = `pa0`–`pa7`, `pb0`–`pb7` (active low: the MS1's key inputs; `pb6` is application version 4010's over-current input), or a name below |
| `SECONDS turn N` | turn the encoder N detents (negative: the other way), one transition per `ENC_TRANSITION_S` (`board.c`): each detent moves a menu one entry (see *Encoder*) |
| `SECONDS lcd` | print the LCD: text, matrix, speed bar, icons |
| `SECONDS text` | print only the text on the LCD |
| `SECONDS unplug` / `SECONDS plug` | pull / plug in the CAN cable: frames to and from the MS1 are lost while it is out (`board_can_plug`); its controller still sees an error-free bus |

```sh
printf '1.5 lcd\n10 lcd\n30 lcd\n' > boot.txt
build/ms1emu -q -t 30 -s boot.txt build/ms1_4011.s19
# menu: OPTIONEN > SPRACHE
printf '12 turn 1\n12.5 turn 1\n13 turn 1\n13.5 press knob\n13.65 release knob\n14.5 text\n' > menu.txt
build/ms1emu -q -t 15 -s menu.txt build/ms1_4011.s19
```

The run stops at a breakpoint or when the CPU halts (`BGND`, an illegal encoding, an
unimplemented instruction); then the last instructions are printed. Diagnostics on stderr name
accesses the model does not cover: reads/writes of registers no module models (first access
each), unmapped addresses, write-once registers written again, EEPROM writes outside a
command, flash writes, and peripheral modes left out (below).

### `difftest`: a rebuild against the original, function by function

`make build/difftest` builds `build/difftest`, which checks a firmware rebuild (such as the GNU
build of `../ms1-firmware`) against the original image:

```sh
build/difftest [-t SECONDS] [-s SCRIPT] [-a CH=V] [-e EEPROM] [-n N] [-m MS] [-v] [-f NAME]... \
    [-D SPEC]... [-u NAME[:N]]... [-c FILE] [-y] [-W] ORIGINAL.s19 REBUILD.s19 SYMBOLS FUNCTIONS
```

- The original runs a scenario: `-t` seconds (default 15), inputs from `-s` (the `ms1emu -s`
  format; `text` lines print the LCD text, `lcd` lines are ignored), `-a`, `-e`, `-D` as for
  `ms1emu`. Decoders are on the original's track only; with an mfx decoder the firmware's mfx
  registration and feedback code runs and is tested.
- Each time it calls (`JSR`/`BSR`/`CALL`) a function in `FUNCTIONS`, the machine — MCU and
  board — is copied twice. One copy runs the original function; the other gets the rebuild's
  flash and starts at the rebuild's function. Both run with interrupts held off
  (`mcu_t.irq_block`) until the function returns to its caller, or `-m` emulated ms
  (default 50) pass.
- The board's inputs hold still in both copies (no `EV_BOARD` event: knob transitions, the
  mfx feedback clock, CAN bus polling). An input change inside the call would reach only the
  side that reads the port after it: the slower code reads later.
- Compared: the return value (when the table gives one), RAM from `0x1000` up to the GCC
  soft registers (`ram_vars_end`: `0x1BB7` in application version 4011, `0x1BAC` when the
  original's version word at `0xF73C` is 4010) and the stack from the return address up, the
  EEPROM, and the sequence of writes to the I/O registers (`mcu_t.write_hook`).
- The EEPROM is compared with the commands under way completed on both sides. A sector
  erase takes milliseconds: one started before the call can finish inside the longer of the
  two calls only, which would otherwise show as a difference.
- `-r ADDR:COUNT` names a ring of `COUNT` CAN frames at `ADDR`, each as the MSCAN buffers
  hold one (identifier 4 bytes, data 8, length 1; repeatable). The data bytes past a
  frame's length are never sent and are left out of the comparison: firmware that sends a
  short frame from a buffer copies whatever the rest of that buffer held. For the MS1
  firmware the transmit ring is `-r 1929:10`.
- `-M IMAGE` runs a second MS1 with `IMAGE` in the same process, on the same CAN bus
  (`ms1_board_t.peer`: each board's frames go straight into the other's controller), with
  its own inputs from `-S FILE` and analog inputs from `-A CH=V`. The two run in lockstep,
  at most 0.1 ms apart. With the original as `-M` and the tested firmware started as slave
  (`-a 0=0`), slave mode can be tested; with the second MS1 started as slave (`-A 0=0`),
  the master's handling of a slave. The tested function's copies are cut off from the bus.
- `-n` calls are tested per function (default 20); the first mismatch of each function is
  printed (all of them with `-v`), then a table: calls, tests, matches, mismatches. `-f`
  restricts the test to the named functions. Exit status 1 if any call differs.
- `-u NAME[:N]` calls a listed function directly, N times (default 50), from the machine
  state at the end of the scenario, as a `JSR` would: return address `DIRECT_RETURN`, D, X,
  Y and the table's stack argument bytes in turn a small number, a RAM address or a word
  below the flash (`input_word`, a fixed pseudo-random sequence; pointers into the flash
  would read different code in the two images). For functions no scenario reaches; the
  table marks them `(called directly)`.
- `-y` adds a table of the mean time of a tested call of each function in the original and
  in the rebuild (emulated µs, interrupts held off): where the rebuild's code is slower.
- `-c FILE` logs the machine's CAN frames (as `ms1emu -c`).
- `-I` with `-W`: interrupt timing after the run, as `ms1emu -I`; for a run with a second MS1
  on the bus (`-M`).
- `-W` runs `REBUILD.s19` as the whole machine and tests nothing: with the same options, its
  text lines, decoder events and `-c` log can be compared with a normal run of the original
  (`../ms1-firmware/tools/regress.py` does this for every scenario). Its LCD is read back with
  the tables `SYMBOLS` names.
- `SYMBOLS`: the rebuild's `nm -n` output — function entries, and RAM variable names for
  the report. `FUNCTIONS`: lines `ORIG_ADDR NAME [v|b|w [ARGBYTES]]` — the original's entry
  (hex), the rebuild's function, its return value (none / `B` / `D`), and the bytes of its
  stack arguments (default 0). Those bytes above the return address are left out of the
  stack comparison: the argument slots belong to the called function and are dead after it
  returns, and the original keeps some parameters there where the rebuild's compiler holds
  them in registers.
- It requires the rebuild's RAM variables at the original's addresses and the same calling
  convention (GCC m68hc1x `-mshort`: first argument in `D`, the rest on the stack).
  Interrupt handlers cannot be listed (they return with `RTI`); a function that waits for an
  interrupt (e.g. the 1 ms tick) times out in both copies and counts as untested.

### Firmware image

`tools/compose_image.py <app.bin> <dump.s19> <out.s19>` builds the flash image the emulator
loads: the application from a BCI-decoded image (`../tools/bci.py`; pages in file order
3C, 3E, 3D, 3F) plus the bootloader and vectors (`0xF800`–`0xFFFF` of page 3F) from a device
dump. Output and dump use the banked S-record layout USBDM writes:

| Records | Addresses | Flash page |
|---|---|---|
| S1 | `0x4000`–`0x7FFF` | `$3E` (fixed) |
| S1 | `0xC000`–`0xFFFF` | `$3F` (fixed) |
| S2 | `PPAGE:8000`–`PPAGE:BFFF` | `$3C`, `$3D` (PPAGE window) |

The bootloader starts only the application version its flash metadata names, so the dump's
bootloader (a 4011 device's) refuses release 4010 or c010: compose those with
`../ms1-firmware/tools/mkimage.py --release 4010 --original <release.bin> <dump.s19> <out.s19>`,
which adapts that word (see its docstring).

## Technical description

C11, no dependencies. Sources in `src/`:

| File | Contents |
|---|---|
| `cpu12.h`, `cpu12.c` | HCS12 CPU core (S12CPUV2): every instruction except the fuzzy-logic and table group, exceptions, WAI/STOP, CALL/RTC through bus callbacks |
| `cpu12_cycles.inc` | bus cycles per opcode and indexed form — generated by `tools/gen_cycles.py` from Table A-1 of the S12CPUV2 Reference Manual |
| `cpu12_dis.c` | disassembler |
| `image.h`, `image.c` | S-record loader (layout above) |
| `mcu.h`, `mcu.c` | MC9S12D64: memory map, register dispatch, interrupt priority, CRG, ports, event scheduler, run loop |
| `ect.c` | Enhanced Capture Timer and modulus down-counter |
| `spi.c` | SPI0 |
| `atd.c` | ATD0 |
| `eeprom.c` | EEPROM controller |
| `mscan.c` | MSCAN0 |
| `board.h`, `board.c` | MS1 board: buttons, encoder, LCD driver, analog inputs, track output, mfx feedback receiver, CAN log |
| `irqstat.h`, `irqstat.c` | interrupt timing (`-I`), through `mcu_t.step_hook` |
| `decoder.h`, `decoder.c`, `decoder_int.h` | locomotive decoders on the track output: API and front ([docs/decoders.md](docs/decoders.md)) |
| `decoder_mm.c` | Märklin Motorola decoder ([docs/mm-decoder.md](docs/mm-decoder.md)) |
| `decoder_mfx.c` | mfx decoder: frames, registration, configuration variables, answers ([docs/mfx-decoder.md](docs/mfx-decoder.md)) |
| `tui.h`, `tui.c` | interactive terminal front end |
| `canbus.h`, `canbus.c` | CAN transports: SocketCAN and the UDP bus |
| `main.c` | `ms1emu` command |
| `../tools/difftest.c` | `difftest` command (above) |

### CPU core

- Semantics follow the **S12CPUV2 Reference Manual, Rev. 4.0** (HCS12 behaviour where it
  differs from the M68HC12): PC-relative indexed operands, MOVB/MOVW included, are relative to
  the next instruction; `EXG` between 8- and 16-bit registers follows the EXG glossary table;
  the X mask bit can be cleared but never set by software (`ANDCC`/`ORCC`/`TFR`/`EXG`/`PULC`/`RTI`).
- Clearing I (`CLI`, `ANDCC`, `TAP`, `PULC`) lets the next instruction run before a pending
  interrupt is taken; `RTI` with an interrupt pending enters it without re-stacking (11 cycles).
- `cpu12_step()` takes a pending exception or executes one instruction and returns its bus
  cycles. The MCU model supplies memory, the highest-priority pending interrupt vector, XIRQ
  and PPAGE through `cpu12_bus_t`.
- Not implemented — the core halts (`CPU12_HALT`, `halt_reason`) rather than guess: `BGND`,
  `MEM`, `REV`, `REVW`, `WAV`/`WAVR`, `TBL`, `ETBL`, `EMACS`. The MS1 firmware uses none of them.
- Cycle counts are exact per instruction form; two refinements are not modelled: the extra
  cycle of `EMULS` before a page-2 instruction and misaligned word accesses.

Regenerate the cycle table after changing `tools/gen_cycles.py`:

```sh
pdftotext -layout S12CPUV2.pdf s12cpuv2.txt
python3 -I tools/gen_cycles.py s12cpuv2.txt > src/cpu12_cycles.inc
```

### MC9S12D64 model

Sources: MC9S12DJ64 Device User Guide V01.20 and the S12 block guides (`../doc/`).

- **Time** is counted in ticks of 1/(12 f<sub>OSC</sub>) (48 MHz with the MS1's 4 MHz crystal),
  so the oscillator, the reset bus clock (OSC/2) and the PLL bus clock are whole tick counts.
  Peripherals keep their counters as "value at the last edge + elapsed time" and schedule an
  event for their next state change; the run loop executes instructions up to the next event.
  Register accesses see the time at the start of the instruction.
- **Machine state** is all in `mcu_t` and `ms1_board_t` — no module keeps state of its own —
  so a machine is copied by assigning the two structs and re-pointing `cpu.bus.ctx`,
  `board.ctx` and `ms1_board_t.m` (`difftest` does this for every tested call).
- **Memory map (MMC):** registers (INITRG, 1K) over RAM (INITRM, 4K; RAM15–RAM12 decode) over
  EEPROM (INITEE, 1K mirrored in its 2K window, EEON) over flash: page `$3E` at `0x4000`, PPAGE
  window at `0x8000` (PPAGE bits 1–0 select `$3C`–`$3F`, so `$38`–`$3B` alias onto them), page
  `$3F` at `0xC000`. INITRM, INITRG and MISC are write-once, as in normal single-chip mode: the
  bootloader's `INITRM=$18` stays, the application's later `$10` is ignored — both map RAM at
  `0x1000`.
- **Interrupts:** each module drives its vector's request line; the CPU takes the HPRIO vector
  if pending, else the highest vector address.
- **CRG:** bus clock OSC/2 until PLLSEL, then OSC·(SYNR+1)/(REFDV+1); writing SYNR/REFDV clears
  LOCK, which returns 0.5 ms later (PLLSEL cannot be set while unlocked). COP and RTI are not
  modelled (the firmware leaves both off); enabling them is logged.
- **Ports** A, B, E, K, T, S, M, P, H, J, AD: reads give the latch on output bits and the
  board's pin levels on inputs; timer output-compare channels override PT pins.
- **ECT:** prescaled counter, output compare with pin actions (TCTL1/2, OC7M/OC7D, CFORC,
  TTOV, TCRE), input capture on PT edges, fast flag clear (TFFCA), overflow, modulus
  down-counter (MODMC reload, FLMC, RDMCL; a MCCNT write in modulus mode only sets the reload
  value). Not modelled: pulse accumulators, input capture queue/latch modes, delay counter.
- **SPI0:** master transfers at SPI0BR's rate, transmit buffer plus shifter, SPIF/SPTEF
  clearing sequences. A SPIDR write is accepted whenever the buffer is empty: the block guide
  requires a prior SPISR read, but the firmware's first write (a bare `CLR`) starts the display
  pump on real hardware.
- **ATD0:** sequences, SCAN/MULT, 8/10-bit, justification and sign, conversion time from
  ATDCTL4, AFFC. Not modelled: external trigger, FIFO mode.
- **EEPROM:** word program, sector erase, sector modify, mass erase, erase verify; one command
  executing plus one buffered; CCIF/CBEIF/ACCERR; durations from fNVMOP. Flash programming is
  not modelled (writes are logged).
- **MSCAN0:** init/sleep handshakes, bit timing, three transmit buffers with priority, five-deep
  receive FIFO, acceptance filters, overrun, listen-only, loopback; an error-free bus. Not
  modelled: error counters, time stamps, wake-up.

### MS1 board model

Wiring, as the firmware uses the pins:

| Function | Pins | Firmware evidence |
|---|---|---|
| buttons | PORTA 0–7, PORTB 0–4, active low | `gpio_read_porta_active_low`, `gpio_read_portb_active_low` |
| rotary encoder A/B | PTT 6/7, active low | `encoder_sample` |
| LCD driver | SPI0 (MOSI, SCK on port M via MODRR), CE = PM3 | `isr_spi0_lcd` |
| track output | PT3, PT4, PT5 (timer output compare) | `isr_timer_ch4_signal_gen` |
| version 4010: booster line, over-current input | PB7 (not in `-T`'s log), PB6 active low (the `pb6` button) | `track_current_monitor` in ms1-firmware |
| track feedback (RDS receiver: RDCL, RDDA, QUAL) | PT0 (input capture 0), PTT 1, PORTB 5 | `isr_feedback_capture` |
| track / decoder voltage | ATD0 channels 0 / 1 | `adc_read_and_scale_voltages` |
| feedback carrier level | ATD0 channel 2 | `isr_feedback_capture`, `mfx_feedback_bit` |

**LCD.** The display driver is not addressed with the PT6547 protocol (command byte after STB
falls): the firmware sends Sanyo CCB-style frames — an address byte while CE is low (`A2` on
the wire, `0x45` LSB-first), then a 10-byte data block while CE is high, latched when CE falls
again; four blocks, each ending in its block code 0–3, refresh the whole glass. SPI0 shifts MSB
first, the driver reads LSB first. The model latches each block by its code and renders the
glass through the firmware's own map: the 38×5 dot matrix at `0xF46C` (per column five
(transmit-buffer offset, mask) pairs, top row first) and the 14-segment speed bar at `0xF450`.
Icons are named from the firmware's icon table at `0xEE5F` (83 (offset, mask) entries, which
the firmware indexes from `lcd_encode_char + 0x43`) with the names identified on MS1 hardware
in ms1-firmware's `src/include/lcd_icons.h` (`*` = the "on" variant of a function icon); the
direction arrows and markings are the bits `lcd_set_bar` sets. Segments outside all of
these are listed as block.byte.mask. The table addresses here are application version 4011's;
for version 4010 the model reads that version's (`rom_tables` in `src/board.c`, chosen by the
version word at `0xF73C`), and for the firmware rebuild, whose tables are elsewhere, the ones
its symbol table names (`-S`, `board_lcd_symbols`).

**LCD text.** The matrix is read back as text with the firmware's font at `0xEF05`
(`lcd_encode_char + 0xE9`): glyph *c* is ASCII *c*, its first byte's low nibble is the width
including a blank spacing column, then one byte per column with rows 0–4 in bits 7–3. Menu
strings are ASCII with lowercase `a`/`o`/`u` standing for Ä/Ö/Ü (those glyph slots hold the
umlauts, `d` holds ß); words too wide for the glass are drawn with condensed L, K, M, N, W,
E, F glyphs (`0x10`–`0x1E`, the `z` slot) and a condensed C that has the pixels of `(`, which the
reader takes as C between letters.

Button names (the controls in the MS1 manual), from what the original firmware does with
each input:

| Name | Input | Evidence |
|---|---|---|
| `esc` (Menu/ESC) | PB0 | in the menus a press goes back one level (OPTIONEN > SPRACHE back to OPTIONEN, then NEUE LOK) |
| `ok` (Locomotive OK) | PB1 | in the menus a press enters the entry shown; on a final choice it confirms and returns to the operating screen |
| `light` (headlights) | PB2 | `ui_task`: the function-key loop reads PB2 for key code 8 (PORTA bit *n* for code *n*), the code the headlight function is bound to |
| `stop` | PB3 | `ui_task`: a press toggles bit 0 of `system_status_flags` (stop/go); held, `ui_stop_held` counts up to the long-press value `0x2E` |
| `knob` (speed knob push) | PB4 | `ui_task`: a press toggles bit 7 of the active locomotive's speed byte and clears its speed (direction change); in the menus it selects like OK |

PORTA (`porta_connector_id` in the imported names) holds the function-key inputs the
locomotive's function table refers to by code 0–7. Holding STOP and the knob during start-up
(`operating_mode` 0: PB3 and PB4 held) erases the first EEPROM sector and restarts the firmware.

**Encoder.** One detent of the MS1 knob is half a quadrature cycle: the firmware adds the step
`encoder_step` (`0xFE`, from `flash_encoder_step` at `0xFF64`) per transition and
`ui_knob_adjust` moves one entry per 4 counts. A line counts as released after 12
equal samples (`encoder_debounce_mask` `0x0FFF`), two samples per main-loop millisecond.
With flag 4 (menus, speed) `ui_knob_adjust` accelerates: more than 3 counts since its
last call add extra steps. The menus call it once per 32 ms (`ui_task`, one
of the `ms_tick` phase-3 handlers). The emulated knob's transitions (`ENC_TRANSITION_S`) are
further apart than that, so a detent always moves one entry. Faster turning, as a hand can do,
loses transitions in the debounce and triggers the acceleration. How many counts that gives
then depends on the main loop's timing, so two images with different code speed would read
the same turn differently.

Defaults: buttons released, encoder at rest (A/B high), ATD0 inputs mid-scale (`0x200`), no
feedback carrier (PB5 low).

**mfx feedback receiver.** RDCL (PT0) runs free from the first track output; an mfx decoder's
answer raises PB5 and ATD0 channel 2, and its data bits re-phase RDCL with the bit on PTT 1
(model and the firmware's use: [docs/mfx-decoder.md](docs/mfx-decoder.md)).

**Track output.** After start-up the signal generator drives PT4/PT5 in antiphase with Märklin
Motorola bit cells of 182/26 µs and mfx cells of 100/50 µs (`-T` logs every edge).
`python3 -I tools/track_decode.py track.log` decodes the log: MM packets (address trits,
function, speed, MM2 bits) and mfx bursts as bit strings. With an empty locomotive list the
firmware sends MM idle packets (address 80) between periodic mfx frames. The track is at 0 V
(PT4 = PT5) during start-up and while STOP is in effect. `-D` attaches decoders that act on
the signal ([docs/decoders.md](docs/decoders.md)).

### CAN

- **Transports.** `-C can0` / `-C vcan0` uses Linux SocketCAN (a USB-CAN adapter, or a virtual
  bus: `sudo modprobe vcan; sudo ip link add vcan0 type vcan; sudo ip link set up vcan0`).
  `-C udp:PORT` needs no privileges: every process given the same port joins one multicast
  group on loopback (`239.255.12.1`) and sees the others' frames — several emulators, or an
  emulator and `master_bridge`. Frames from the bus are polled every 0.5 ms of emulated time;
  combine with `-r` so that the emulator keeps pace with real-time partners.
- **Master or slave.** `app_main_init` measures the track voltage (ATD0 channel 0, scaled by
  `flash_adc0_scale`, default `0x6C4F`/65536) once at start-up: above 5000 the MS1 is the central —
  it sets the flag imported as `is_master`, starts the signal generator, sends
  eight `0x380` probes when MSCAN comes up (about 9.3 s after reset; to a slave registered
  with master node 0 they are a reset, so a slave left from an earlier session registers
  again) and handles registrations from slaves (`mcan_process`, command `0x7E`). Otherwise it is a slave: no track signal,
  and a registration request `1C00007E` every ~5 s until a master answers. The default analog
  inputs (512) make it the central; `-a 0=0` makes it a slave.
- **Locomotive discovery.** The routine imported as `mfx_registration_start` is the mfx
  discovery over the track (a 32-step UID search using the track feedback capture), not CAN.
  With `-D mfx:UID` a decoder answers and the registration completes.
- **`master_bridge`** (`make build/master_bridge`, `MS1_MASTER` = path to `../ms1-master`) runs
  that library's M-CAN master on a bus, in real time, with two locomotives (`BR 44`: speed,
  light, a function; `V 200 TEST LOK`: speed, a momentary function), assigns both to every slave
  that connects, and prints the frames and the protocol events (connected, took/released,
  speed, function, STOP/GO, disconnected):

  ```sh
  build/master_bridge [-n NODE_BASE] BUS [SECONDS [SCRIPT]]
  build/master_bridge udp:47112 60 &
  build/ms1emu -q -r -t 40 -a 0=0 -C udp:47112 -c - build/ms1_4011.s19
  ```

  `-n` sets the master node of the first slave (default 42, as an ECoS; 0 as an MS1 master).
  `SCRIPT` acts as the master's operator, lines `SECONDS ACTION` in time order: `stop`, `go`,
  `speed LOK VALUE` (hex, bit 7 reverse), `function LOK NUMBER 0|1`, `assign SLAVE LOK`,
  `unassign SLAVE LOK`, `rename SLAVE NAME`.

  With the emulated slave (`-a 0=0`) the slave registers (about 9.4 s after reset), fetches
  both locomotives, takes the first one and shows `BR 44`; its knob, light key (function 2),
  PA0 (function 3) and knob push (direction) reach the library's callbacks; STOP/GO work from
  both sides (STOP icon); the master's speed and functions show on the slave; on the second
  locomotive the momentary function is on PA7 (the MS1 puts icon 2 there). After `unplug` the
  library gives the slave up 5.1 s after its last frame; after `plug` the slave, which has
  restarted meanwhile, registers again. Two slaves (the second from a copy of the image with
  another UID at `0xFF10`–`0xFF13`, which the bootloader's CRC does not cover) register one after
  the other, about 5 s apart.

### Checks

| Target | What it compares | Pass criterion |
|---|---|---|
| `make check` (`tests/cpu_test.c`) | exception entry/return, RTI chaining into a pending interrupt, the delay after clearing I, WAI, SWI, TRAP, CALL/RTC, X-bit rules | all pass |
| `make check` (`tests/decode_check.c`) | every instruction in the Ghidra listing (`ms1-ghidra/out/listing.tsv`, written by `ms1-ghidra`'s `ExportListing.java`) against this disassembler: image bytes, length, mnemonic (aliases folded) | 0 mismatches |
| `make diff` (`tests/diff_sim6812.c`) | each opcode, 20 000 random register/memory states, against sim6812's CPU core (`SIM6812`, default `../sim6812/src/sim6812`: a checkout of the sim6812 HC12 simulator, which is not part of this repository and has its own license; linked into the test program only): registers, condition codes (minus bits the manual leaves undefined), memory writes | no difference outside the `known` table |

The `known` table in `tests/diff_sim6812.c` lists the opcodes where sim6812 departs from the
manual, each with the defect in sim6812's `instr.c` (e.g. `CPS` compares B, `ASR` shifts in 0,
`MINA`/`MAXM` decide on N instead of C). `make diff` skips what the harness does not model:
SWI, TRAP, WAI, STOP, BGND, CALL/RTC and the unimplemented instructions.
