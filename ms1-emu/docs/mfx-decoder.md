# mfx decoder and feedback receiver

`-D mfx:UID[,name=NAME][,symbol=SYMBOL][,cv=VVV/II:XX]...[,noread][,frames]` (usage and the common parts:
[decoders.md](decoders.md)). The decoder reads the MS1's mfx frames and answers over the track
like an mfx locomotive decoder: the MS1 finds it, gives it a track address (SID), reads its
configuration, then drives it. Sources: `src/decoder_mfx.c` (decoder), `src/board.c`
(feedback receiver, `board_feedback`). Protocol: S. Krauss, *Description of the mfx rail
format* v2.3, `../doc/MFX - Trackprotocol.pdf`, summarised in the header comment of
`src/decoder_mfx.c`.

## Events

| Event | When |
|---|---|
| `central UID, registration counter Z` | the first "central" command, and every change of UID or counter |
| `search C0: answering` / `search C32: answering` | the first and last search it answers (it answers every one that matches its UID) |
| `registered: SID A (central UID, counter Z)` | an "assign" command for its UID |
| `read CV VVV/II: XX [XX …]` | a read for its SID: the bytes it sends back |
| `write CV VVV/II = XX` | a write for its SID that changes a value |
| `speed +N` / `speed -N` | speed (0–127) or direction changes; `-` is reverse |
| `emergency stop` | speed step 1, once per run of them |
| `F<n> on` / `F<n> off` | a function state changes (F0–F31) |
| `ping with another UID: registering again` | a ping for its SID with another UID |
| `another central or registration counter: registering again` | a central command that does not match the stored one |
| `the central is known: taking commands` | after power-up, the second matching central command |
| `track voltage on after X ms off` | the track had 0 V for longer than `POWER_OFF_US` |
| `the window ended after N clocks, the answer needs M` | a data answer did not fit the window |
| `frame SID n: …` | (`frames`) every frame with a valid CRC, parsed; `frame of n bits: CRC error` otherwise |

The state line reads `mfx:7A3F0012  SID 1  speed +36/127  F0-F15 0085  (904 frames)`, with
`not registered` or `(waiting for the central)` in place of a usable SID.

## Decoding

- **Cells.** Each level between two polarity changes is a half cell (`HALF_MIN_US`–
  `HALF_MAX_US`) or a full cell (`FULL_MIN_US`–`FULL_MAX_US`); anything else resets the
  decoder (MM packets, pauses). Biphase mark: two half cells are a 1, a full cell a 0.
- **Sync patterns.** A half cell followed by a full one breaks the coding rule: a sync
  pattern. The bits since the previous break, without the last 0 (the pattern's first bit),
  are a frame. A full pattern breaks the rule twice; the half pattern that can end a series
  once. The data after a break starts there.
- **Frames.** The 0 after eight 1s is removed (any 1 there: a stuffing error), then the CRC
  must come out 0. Address formats 7/9/11/14 bits, SID 0 broadcast. The commands follow until
  the CRC; an unknown or reserved command ends the parse.
- **Feedback window.** Bits `0011` directly after `WINDOW_SYNCS` rule breaks: the edge that
  completes them starts the pause in which the decoder answers.

## Registration and commands

Registration follows the document's sections 4.2–4.3:

- Unregistered, it answers every search (broadcast) whose UID's top C bits match its own.
- "Assign" for its UID gives it the SID; it keeps the central's UID and registration counter
  from the last "central" command with it, and takes commands at once.
- It answers pings for its SID that carry its UID. A ping with another UID, or a central
  command with another UID or counter, unregisters it. If the counter changes while it listens,
  it stores the new value and stays registered.
- 0 V longer than `RESTART_US` restarts it: registered, it takes commands again only after
  two matching central commands.
- It takes drive, function, read and write commands for its SID; drive and function commands
  also on the broadcast address once registered. A frame for its SID without a command asks
  for a 1-bit answer. Writes to index 0 (CA types) are ignored.

## Answers

The decoder answers through the board's feedback receiver (`decoder_feedback_t`):

| Answer | Decoder | Receiver (MS1 inputs) |
|---|---|---|
| 1-bit | carrier on from the window start to the end of the first pause | ATD0 channel 2 at `RDS_CARRIER_LEVEL`, PB5 (quality) high |
| data | carrier on for the window; on each clock mark of the central one bit: 1s for `IDLE_CLOCKS` marks, then `010`, the bytes (MSB first), the checksum, then 1s | per bit: PTT 1 (data) set, then a rising edge on PT0 (RDCL) |

- Clock marks: in the window's pause, a level shorter than `PULSE_MAX_US` (and not shorter
  than `PULSE_MIN_US`, the polarity correction at the window start) is a mark. A mark less
  than 3/4 of `CLOCK_US` after the last clock is a mid-bit mark and is ignored.
- The checksum: CRC start 0xFF, polynomial 0x07, over the data bits (the firmware's
  `mfx_feedback_data`; the document's algorithm in 2.3.4 gives the same).
- The CV bytes come from the read's address on, the index counting up within the CA.

**Feedback receiver** (`board.c`): the MS1's RDS demodulator for the 52.63 kHz carrier.

| Output | MS1 pin | Model |
|---|---|---|
| RDCL (bit clock) | PT0, input capture 0 | free-running square wave, a level per `RDS_HALF_S`, from the first track output; a decoder's bit re-phases it with a rising edge, and the next free-running rising edge waits one bit time more |
| RDDA (data) | PTT 1 | the last bit (1 at rest) |
| QUAL (quality) | PB5 | high while a decoder sends the carrier |
| carrier level | ATD0 channel 2 | `RDS_CARRIER_LEVEL` while a decoder sends the carrier, else the `-a 2=` value |

How the firmware reads it (`isr_feedback_capture`, `ms1-firmware/src/signal/feedback_capture.c`, the signal
generator starting and stopping the capture at fixed points of the window):

- **1-bit** (capture mode 2, both RDCL edges): ATD0 channel 2 into `timer_capture_buffer`.
  `mfx_feedback_bit(14)` compares the samples of the first pause with those of
  the second: an answer if the difference, each pause's largest sample left out, exceeds 30
  per sample.
- **Data** (capture mode 1, rising edges): two bits per edge, PTT 1 then PB5.
  `mfx_feedback_data` starts at the 21st edge, looks for `1 1 … 0 1 0` within 9 edges,
  then reads the bytes and the checksum; each edge with PB5 low halves a quality count that
  must stay above 0x13.

## Configuration variables

A fixed image (`cv_image`), addressed `V << 6 | I` (CV number V, index I); unset CVs read 0.
NAME and SYMBOL go into block 01.

| CA | Type | Contents |
|---|---|---|
| `000` | 01 | block 01 (decoder properties), 1 group of 6 CAs |
| `001` | 10 | manufacturer Märklin (83), product 0E |
| `002` | 16 | symbol `01 SS` (SS: 08 none, 09 E-Lok, 0A diesel, 0B steam), manufacturer 83 |
| `003` | 18 | name, 0-terminated |
| `004` | 17 | block table: block starts / 4 = 02, 03, 10, 12 |
| `005` | 14 | protocol 1.0, 64-bit commands, multi-byte read/write bits `05` |
| `008` | 01 | block 02 (functions), 2 groups of 1 CA |
| `009` | 10 | group 0: driving function, block 05 + 1 |
| `00A` | 11 | group 1: F0–F15, each the offset of its group in block 04 (01, 04, … 2E) |
| `00C` | 01 | block 04 (function assignment), 16 groups of 3 CAs |
| `00D`+3g | 10, 12, 13 | function g: symbol `M\|G S1 S2`, forward and reverse outputs |
| `040` | 01 | block 05 (driving characteristics), 1 group of 7 CAs |
| `041`–`046` | 10, 11, 13, 14, 17, 18 | one motor, regulated DC motor, acceleration/braking, trim, Vmin/Vmax, speedometer |
| `048` | 01 | block 08 (sound), 1 group of 2 CAs |
| `049` | 10 | volume |

Function symbols F0–F7: light, outdoor lighting, sound, shunting gear, ABV off, sound
(momentary), mechanical, interior lighting; F8–F15: undefined (`02`).

What the MS1 reads (`mfx_registration_process`, states 6–17), which this layout satisfies:

- Block 01: the description (index 2 must be 01, index 5 the number of CAs), the CA types,
  then CA types 14 (index 1 must be 01; index 5 bits 2–3 allow 2-byte reads), 16 (index 1
  must be 01, index 2 the symbol), 18 (the name, 2 bytes per read) and 17 (the block table).
- For each block in the table: its type and version, and for blocks 02, 04, 05, 08 their
  group counts and sizes.
- Without CA type 14 or 18 the MS1 skips that step. A missing CA type 16 or 17 or block 02,
  or a check above that fails, makes it assign the decoder another SID
  (`mfx_reassign`) and then give up the slot.
- Block 02: the group of each CA picks the command the MS1 uses for it — the command code
  is `rom_func_icon_table[group]` (codes 1, 2, …: drive `00x`, functions `01x`, …). So the
  driving function must be in group 0 and the switching functions in group 1; with both in
  one group the MS1 sends the function states with the drive code.
- From block 05 the motor count, Vmax and acceleration/braking; from block 04 each function's
  symbol (the MS1 maps group G or S1 to its icons with `mfx_icon_lookup`); from block 08 the
  volume.

## Analysis

Printed at the end of the run (`mfx_report`):

| Line | Meaning |
|---|---|
| `mfx frames`, `CRC errors`, `stuffing errors`, `sync patterns` | frames with a valid CRC, rejected frames, rule breaks / 2. DCC packets on the track count as stuffing errors: DCC's 58 µs halves fall in the half-cell window, so the decoder reads them as cells until the stuffing rule rejects them (`../ms1-firmware/tools/scenarios/dccmix.txt`) |
| `commands: …` | commands by kind, whatever their address |
| `half cell`, `full cell` | level lengths in frames (µs) |
| `central interval` | time between central commands (ms) |
| `feedback windows: 1-bit, data; answered: …` | windows by kind (data: with clock marks), and those this decoder answered |
| `1-bit window pause` | the first pause of 1-bit windows (µs) |
| `clock marks per window` | clock marks (not mid-bit marks) per data window |
| `clock interval`, `clock mark width` | between clock marks, and their length (µs) |
| `track voltage off` | 0 V periods longer than `POWER_OFF_US` (ms) |
| `registered at`, `frames for SID` | when it got its SID, and the frames for it since |

Measured on the original 4011 image with the calibration patch (`calpatch.py`), erased EEPROM,
running `ms1-firmware/tools/scenarios/mfx.txt` for 42 s with
`-D 'mfx:7A3F0012,name=BR 185'`:

```
mfx:7A3F0012: mfx frames 1614, CRC errors 0, stuffing errors 0, sync patterns 5700
  commands: central 591, search 116, assign 3, ping 50, read CV 87, write CV 0, drive 639, functions 571, unknown 0
  half cell                49.5-50.5 us, mean 50.0 (59478)
  full cell                99.5-100.5 us, mean 100.0 (80149)
  central interval         29.9-1032.0 ms, mean 51.8 (590)
  feedback windows: 1-bit 166, data 87; answered: 1-bit 82 (searches 33), data 87
  1-bit window pause       6393.9-6394.1 us, mean 6394.0 (166)
  clock marks per window   46.0-54.0 , mean 48.8 (87)
  clock interval           911.7-912.3 us, mean 912.0 (4155)
  clock mark width         27.8-28.4 us, mean 28.0 (6483)
  track voltage off        967.4-11381.3 ms, mean 6174.3 (2)
  registered at 20.154 s; frames for SID 1: 904 (20.173-41.991 s)
```

The MS1 as the decoder sees it in that run:

- The central command (UID `000752E2`, counter 0) comes about every 30 ms while nothing else
  is sent.
- Before 14 s the MS1 searches only with C29, for the UID stored in its EEPROM (erased: all
  1s). From 14.1 s it searches with C0, then bit by bit, each search followed by a 1-bit window
  (11 sync patterns, `0011`, two pauses of 6394 µs); at 20.0 s C32 matches the whole UID.
- Every command that wants an answer is sent with its window once, then evaluated.
- SID 1 at 20.15 s (the assign command comes three times), then 87 CV reads, 104.6–137.9 ms
  apart, to 30.8 s; 30 of them read 2 bytes. Data windows have 46 clock marks for 1 byte and
  54 for 2 (23 idle, then 23 or 31), 912 µs apart, each 28 µs wide.
- The LCD then shows the name and the symbol: `BR 185`, icons `F1-light1 F2-speaker
  F3-turtle F4-ramp F5-function F8-light2 light electric-loco mfx`.
- Changing the locomotive (LOKÄNDERN: NAME, VMAX, ACC, DEC, VOL) writes the CVs the
  read-out found: the name at CA type 18 index 1 on, Vmax at CA type 17 index 2,
  acceleration and braking at CA type 13 index 1 and 2, the volume at block 08's CA type 10
  index 1 (`ms1-firmware/tools/scenarios/mfxedit.txt`).
- Faults (`mfxfault.txt`, `-D mfx:00000011,cv=000/02:02 -D mfx:00000022,noread`): a block 01
  of version 2 makes the MS1 assign the decoder SID 13998 and drop the slot; a decoder that
  never answers a read is given up after its retries (`mfx_registration_abort`), then
  deleted as the document's section 4.4 describes (a ping for its SID with UID 0, the
  registration counter up by one) and found again by the next search. With several
  unregistered decoders the search finds the smallest UID first.
- Driving: a speed change goes out at once as a drive frame; the refresh frames carry drive
  and function state together. A knob press (direction change) sends four frames at speed
  step 1 (emergency stop), then speed 0 in the new direction. Light and the keys PA0/PA1
  switch F0, F7 and F2.
