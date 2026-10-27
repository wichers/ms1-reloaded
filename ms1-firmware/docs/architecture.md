# MS1 firmware architecture

How the parts of the firmware fit together: what runs when, how data gets from the inputs
(keys, knob, CAN, ADC, mfx feedback) through the locomotive slots to the outputs (track
signal, CAN, LCD, EEPROM), and how the code drives the hardware.

- **Where a function lives:** `tools/functions.tsv` (original entry → function here), the
  "Functions:" list in each source file's header (with the original entries, the interrupt
  handlers' too), README *Source Layout* (file → subsystem).
- **Addresses** are the original's, of application version 4011. Function entries are as in
  `tools/functions.tsv`. RAM variables sit at their original addresses (README *RAM layout*,
  `build/ms1.sym`). The original's read-only tables are C in this build, with the original's
  contents (README *Image layout*); the addresses given with them are the original's.
- **Version 4010** (releases 4010 and c010) differs in the track current, the booster line, a
  few functions and its addresses: section 12.
- **Timer ticks:** the timer runs at 6 MHz (24 MHz bus, prescaler 4 set by `signal_gen_init`),
  so 6 ticks = 1 µs.

```
 keys (PORTA, PORTB)    knob (PTT 6/7)    CAN0 RX ring     ADC (ATD0)     mfx feedback (PT0, PTT 1, PB5, ATD0 ch 2)
        |                    |                 |                |                    |
        +------> ui_task <---+        mcan_process         track_current_monitor   mfx_registration_process
                    |                          |                |                    |
                    v                          v                v                    v
          +------------------------------------------------------------------------------+
          |  lok_records (0x13D7) / lok_states (0x17F7): the locomotive slots            |
          |  pending-work words per slot: track_pending, can_pending, save_pending       |
          +------------------------------------------------------------------------------+
                    |                          |                         |
           track_scheduler (master)    mcan_lokstack_sync          eeprom_save_task
                    |                          |                         |
      command -> send -> staging rings     CAN0 TX ring             EEPROM 0x0800
                    |                          |
      isr_timer_ch4_signal_gen: PT3-PT5    can0_tx_isr

 display: UI tasks write lcd_buf (0x1108) directly; isr_spi0_lcd streams it to the PT6547
```

## 1. Master and slave

`app_main_init` (0xD34F) decides the role once, at start-up. If `adc_track_voltage` is above
5000 (mV), the device has track power: `is_master` is set.

| | Master (`is_master` nonzero) | Slave |
|---|---|---|
| Track signal | generates it: `signal_gen_init`, the send-buffer executor and command-buffer consumer in the main loop, `track_scheduler` | none: `track_scheduler` only clears every slot's `track_pending` and `track_functions` |
| Slots | copied from the EEPROM (`EE_SLOTS`) at start-up | marked free; filled from the master's slot list over CAN (`slot_from_master`) |
| CAN | takes one slave's registration, answers its requests | registers with the master, then follows its slots |
| `track_current_monitor` | current, overload, track on/off | only the operating-minute counter |
| Start-up state (`system_status_flags`) | bit 0 (STOP) until the start-up screen ends | bits 0 and 4; bit 4 clears when the master confirms this device after its registration (`slave_frame_for_me`) |

## 2. Start-up

1. Reset runs the bootloader (0xF800-0xFFFF, taken from the original image, README *Image
   layout*; its checks, update mode and metadata: `docs/bootloader.md`). After its checks it
   enters the application through the last entry of the trampoline table (`src/sys/vectors.c`),
   which jumps to `_start` (0x4000).
2. `_start` (`src/sys/crt0.c`) sets up the MMC registers, maps page $3D into the 0x8000 window for
   good, and brings the PLL to a 24 MHz bus. It then falls into `crt_restart` (0x403C), which
   masks interrupts, drives PORTE bit 2 low, turns off the timer channel 4 interrupt, makes
   channel 3 an input capture again (track output off, see 4.4), copies .data, clears .bss, sets
   the stack to `_stack` and calls `main`.
3. `main` (0xB9D6) calls `app_main_init`, then runs the main loop (section 3).
4. `app_main_init`, with interrupts masked throughout:
   - LCD: `lcd_init`, then `lcd_buf_init` (the start-up image) and `lcd_pump_all`, which sends
     it synchronously (section 7);
   - PORTE bit 2 high; `timer_atd_init`, `timer_modulus_init`; the voltages are read and the
     role is decided (section 1). The master runs `signal_gen_init` and starts in STOP;
   - EEPROM (section 9): rewritten from `eeprom_defaults` when `EE_LANGUAGE` holds an unknown
     value; the counter ring is repaired and its newest records loaded into `eeprom_record`;
     language and settings are loaded. On the master, a nonzero `EE_MFX_UID` (a stored mfx
     UID, 4.8) starts `mfx_registration_start`;
   - slots (section 1); `ui_enter_mode(0)`; `can_controller_full_init`; `lcd_buf_load` (a blank
     display); `timer_modulus_read`; interrupts on.
5. `ui_task` in operating mode 0 (the start-up screen) averages `track_current` into
   `track_current_offset`, the current-sense zero (not in version 4010, section 12). On the
   master it then clears STOP, so the track signal starts (4.6), and switches to mode 1.

Leaving the application:
- `crt_restart` restarts it. Callers: the slave losing its master (`src/can/mcan.c`), and the
  reset paths in `src/ui/ui_control.c` after they erase or rewrite EEPROM.
- `enter_bootloader` (0x407C) clears RAM and jumps to the bootloader entry in the flash
  metadata. Callers: the options menu (`src/ui/ui_control.c`), and a flag in the master's
  registration answer (`src/can/mcan.c`).

## 3. Scheduling

**Main loop** (`main`): one pass per millisecond. `wait_for_next_ms_tick` waits until
`rtc_ms_counter` (counted by `isr_ms_tick`) differs from `ms_tick`, then counts `ms_tick` up.

| Runs | Work |
|---|---|
| every pass | `adc_read_and_scale_voltages`, `track_current_monitor`, `encoder_sample` (twice), `signal_gen_send_buf_executor` (master) |
| every 4 ms, one phase by `ms_tick & 3` | 0 `mcan_process`; 1 `signal_gen_cmd_buf_consumer` (master); 2 `track_scheduler`; 3 the phase-3 task |
| every 32 ms, phase 3 by `ms_tick` bits 2-4 | `eeprom_save_task`, `button_scan`, `ui_task`, `lcd_status_icons`, `lcd_speed_bar`, `ui_screen_text`, `lcd_draw_text`; the last slot is empty |

**Interrupts** (vectors: `src/sys/vectors.c`; there is no RTOS, everything else runs in the main
loop):

| Handler | File | Trigger | Work |
|---|---|---|---|
| `isr_ms_tick` (0xDFEC) | `src/sys/ms_tick.c` | modulus down-counter, every 1 ms (`timer_modulus_init`) | counts `rtc_ms_counter`; `rtc_minute_ms`, `rtc_day_minutes`, `rtc_day_counter` count ms in the minute, minutes in the day, days |
| `isr_timer_ch4_signal_gen` (0xC4ED) | `src/signal/signal_gen.c` | timer channel 4 compare (master only) | the next staging entry to the track outputs (4.4) |
| `isr_feedback_capture` (0xDF13) | `src/signal/feedback_capture.c` | timer channel 0 input capture, during a feedback capture | samples the mfx feedback receiver (4.8) |
| `isr_spi0_lcd` (0xC23E) | `src/ui/lcd.c` | SPI0 transfer complete | the next `lcd_buf` byte to the PT6547 (section 7) |
| `can0_rx_isr` (0xDD61) | `src/can/can0.c` | CAN0 receive | frame into the receive ring (section 6) |
| `can0_tx_isr` (0xDD1B) | `src/can/can0.c` | CAN0 transmit buffer empty | the next frame from the transmit ring |
| `can0_error_isr` (0xDD10) | `src/can/can0.c` | CAN0 receiver overrun | clears the overrun flag |
| `unexpected_interrupt` | `src/sys/vectors.c` | every other vector | stops (BGND) |

The handlers, and what they call, lie in fixed flash, as in the original: an interrupt may
come while the application runs code in page $3C, with page $3C in the window
(`src/include/page3c.h`; `tools/banks.py` places them).

Code that masks interrupts itself: `crt_restart`, `enter_bootloader`, `app_main_init`,
`can_controller_full_init`, the ring update in `can_tx_enqueue_frame`, `encoder_set_count`,
`erase_and_restart`.
`lcd_pump_start` calls `isr_spi0_lcd` as a subroutine by building an interrupt frame.

`delay_short(n)` and `delay_long(n)` are busy waits of about n ms and n s.

## 4. Track signal (master)

### 4.1 Pipeline

The C code writes commands. `src/signal/signal_gen.c` expands them in two stages into
timer compare entries, and the timer channel 4 interrupt plays those out on the track outputs.
The display does not use this pipeline (section 7).

```
 track_scheduler (phase 2) --writers in track_cmd.h--> track_cmd_buf        0x1268-0x1297  command entries
                                                            | signal_gen_cmd_buf_consumer  (phase 1, 8 steps)
                                                            v
                                                       signal_gen_send_buffer 0x1228-0x1267  [opcode, data] pairs
                                                            | signal_gen_send_buf_executor (every pass, 8 pairs)
                                                            v
                                                       signal_staging_buf    0x1160-0x1227  [control, time] entries
                                                            | isr_timer_ch4_signal_gen     (one entry per compare)
                                                            v
                                                       PT3, PT4, PT5 -> track
```

| Stage | Written by (pointer) | Read by (pointer) | Flow control |
|---|---|---|---|
| command | the writers in `src/include/track_cmd.h`; `track_cmd_advance` closes each entry (`track_cmd_write_ptr`, 0x101E; `track_cmd_free`, 0x1020, counts free entries) | consumer (`track_cmd_read_ptr`, 0x101C; `track_cmd_pos`, 0x1298, is the step within the entry) | `track_scheduler` writes while `track_cmd_free` is nonzero; the consumer starts a step only when the send buffer is empty or has more than 10 bytes free |
| send | consumer (`signal_gen_send_buf_write_ptr`, 0x101A) | executor (`signal_gen_send_buf_read_ptr`, 0x1018) | the executor takes a pair only when the staging ring is empty or has more than 48 bytes free |
| staging | executor through `signal_gen_staging_buf_write` (0xC66F; `signal_gen_staging_write_ptr`, 0x1016) | the interrupt (`signal_gen_staging_read_ptr`, 0x1014) | none: the interrupt never compares with the write position, so if the main loop falls behind it replays old entries |

- Every command entry is 12 bytes (`track_cmd_advance`); the `mfx_` names cover MM
  entries as well.
- `signal_gen_init` (0xC58E) fills all three stages with 1 ms track-off pauses before it turns
  on the channel 4 interrupt, so the track starts at 0 V.

### 4.2 Command entries

Byte 0 is the type and byte 1 the number of steps; data starts at byte 2. Each consumer step
writes one or more send-buffer pairs, and the entry is freed after its last step (jump table
in `signal_gen_cmd_buf_consumer`, 0xC81D).

| Type | Writer | Steps → send-buffer pairs |
|---|---|---|
| 0 | — | none |
| 1 | `track_cmd_pause(µs)` (0xCA46) | track off for about the given µs: opcode 1, 0x80 per step and the remainder in the last |
| 2 | `track_cmd_write_type2` (0xCA5E): no caller | opcode 2 with byte 2 |
| 3 | `mfx_cmd_syncs(n)` | per step: opcode 0x0B with 4, two sync patterns |
| 4 | `mfx_frame_end` | byte 1 = bits in the last byte × 16 + byte count. Step 0 adds one sync pattern (0x0B, 2) first; then one byte per step: opcode 0x0A, for the last byte 2 + its bit count |
| 5 | `mfx_cmd_feedback(n, mode)` | a feedback window, 1 + n steps (4.8) |
| 8 | none | opcode 0x0E with byte 2 |
| 9 | `mm_write_*` | an MM double packet (4.7), by step: 0, 4 pause (0x0E); 1, 3, 5, 7 eight bits (0x0F); 2, 6 two bits (0x10) |
| 12 | none | opcode 0x11 with byte 2 |
| 6 (`CMD_DCC`) | `src/signal/dcc.c` (4.9) | a DCC packet {6, steps, preamble bits, byte count, bytes with the XOR byte}: step 0 the preamble (opcode 0x1D with 0xFF per 8 ones, then the remainder), then a byte per step (0x16 with 0, the start bit; 0x1D with the byte), the last with the end bit and one more 1 (0x17 with 0xC0) |

No code writes the other types; the original's jump table entries for them are not handlers.

**Switching protocol.** The consumer keeps the last type in `signal_gen_prev_cmd_type`
(0x129B). Types 1, 2, 12 and 13 do not change it. When an mfx entry (4, 5) is followed by
anything but 3-5, `signal_gen_filter_aux_cmds` inserts two sync patterns (0x0B, 4). When an MM
entry (9) is followed by anything but 8-10, an MM pause (0x0E, 0x3C) is inserted. A DCC
packet after either gets the same; DCC needs nothing before the next protocol, since its
last 1 ends the end bit with an edge.

### 4.3 Send-buffer opcodes

These are the jump-table entries of `signal_gen_send_buf_executor` (0xC614); `d` is the data
byte. The mfx timing ops alternate the polarity with each edge: `track_polarity` (0x12A0)
holds the output bit, 0x10 or 0x20, that the next edge drives. The MM ops set absolute
levels.

| Opcode | Staging entries written |
|---|---|
| 0, 0x0C, 0x0D | none |
| 1 | four entries of d × 16 ticks, both outputs low (0 V), bit 7 set |
| 2 | capture control: d goes into `track_capture_cmd` (0x129C), or into `track_capture_next` (0x129D) when that is busy; then one zero-length entry with bit 6, the trigger |
| 3-0x0A | the top 1-8 bits of d as mfx cells (`signal_gen_fullbit_entry`, `signal_gen_halfbit_pair`): 0 → one 100 µs cell, 1 → two 50 µs halves. A 0 is inserted after eight 1s in a row (bit stuffing; `mfx_stuff_count`, 0x12A1, counts the 1s) |
| 0x0B | d half sync patterns (`signal_gen_idle_pulse`: full, half, full cell, 250 µs). Two of them are the mfx sync pattern 0 1 1 1 0. With d bit 7 set, one more is added unless `track_polarity` is 0x20, which fixes the polarity after them |
| 0x0E | MM pause: d units of 24000/39 ticks (≈ 102.6 µs) at negative polarity (PT5), in entries of up to 4 units |
| 0x0F, 0x10 | the top 8 or 2 bits of d as MM bits: 1 = 182 µs positive then 26 µs negative, 0 = 26 µs then 182 µs |
| 0x11 | 6 × [(d & 0x7F) × 8 ticks at the polarity d bit 7 selects, then (127 − (d & 0x7F)) × 8 ticks at 0 V] |
| 0x12 | one edge, d µs |
| 0x13 | one edge, then 2 × 2d µs at that level (the same entry twice) |
| 0x14 | d µs without an edge (the level continues) |
| 0x15 | 2 × 2d µs without an edge |
| 0x16-0x1D | the top 1-8 bits of d as DCC bits: two entries each, an edge before each, 58 µs (348 ticks) for a 1, 100 µs (600 ticks) for a 0 (4.9) |

### 4.4 Staging entries and the track outputs

An entry is a 16-bit word: a control byte and a low byte.

| Bits | Meaning |
|---|---|
| control 0-3 + low byte | how long the entry's levels last, in ticks (12 bits); 0: the interrupt goes straight on to the next entry at the same compare |
| control bit 4 | PT4 level (TCTL1 OL4) |
| control bit 5 | PT5 level (TCTL1 OL5) |
| control bit 6 | capture trigger: acts on `track_capture_cmd` (1 start the data capture, 2 start the level capture, 0x81 / 0x82 stop it, the result into `track_capture_samples`), then moves `track_capture_next` into it |
| control bit 7 | PT3 level (TCTL2 OL3) |

- `isr_timer_ch4_signal_gen` sets the entry's levels for the compare at
  `signal_timer_compare_base` (0x1021) on channels 3, 4 and 5, then adds the entry's ticks to
  the base.
- Track voltage: PT4 high and PT5 low is positive, the other way round negative, both equal
  0 V (`../ms1-emu/docs/decoders.md`, *Track voltage*).
- PT3 follows bit 7 only while TIOS bit 3 makes channel 3 an output compare. Otherwise PT3 is
  the port bit, which `signal_gen_init` sets high. So PT3 is high in opcode-1 (track off)
  entries and whenever TIOS bit 3 is clear, and low otherwise.
- On the master outside operating mode 0, `track_current_monitor` sets TIOS bit 3 on every pass
  unless STOP (`system_status_flags` bit 0) or an overload shutdown (bit 6) is on.
  `crt_restart`, `enter_bootloader` and the restart paths in `src/ui/ui_control.c` clear it.
- Version 4010 drives bit 7 on PB7 instead, inverted and at once (section 12).

### 4.5 What goes on the track: `track_scheduler`

`track_scheduler` (0x598D, phase 2) runs on the master only. While `track_cmd_free` is
nonzero, it gives each free entry to the first of these that writes one:

1. `lok_packets_next` (0x40A3), only while `mfx_cmd_steps` is 0: the next packet of a slot's
   pending job (`track_pending`, `track_functions`; job kinds in the header of
   `src/signal/lok_packets.c`).
   In STOP, when no job is in progress, it writes a 2 ms track-off pause instead, which keeps
   the track at 0 V. Meanwhile only an mfx command already under way (`mfx_cmd_steps` nonzero)
   still reaches `mfx_registration_process`.
2. `mfx_registration_process` (0x4B8F): the mfx engine (4.8).
3. `lok_packets_refresh` (0x4922): the periodic refresh, one packet per call, slot by slot.
   MM slots cycle through phases (`mm_phase`); mfx slots get one drive-and-functions frame;
   DCC slots one packet of their turn (4.9).
4. `track_filler` (0x5952) when none of them writes: it steps `track_filler_state` and
   alternates the mfx central frame (this device's UID from `flash_uid_hi`/`flash_uid_lo`, and
   `mfx_reg_counter`) with an MM idle packet.

The code also checks `track_filler_hold` (0x105B) and `track_scheduler_hold` (0x1036), but
nothing sets either of them, in the original too, so they stay 0. While DCC service mode is
on, `dcc_service_next` gets every entry instead (4.10).

### 4.6 STOP, track current and overload

- STOP is `system_status_flags` bit 0. The STOP key toggles it (`ui_task`); CAN status frames
  set or clear it (`mcan_receive_status`). Each change is flagged in `mcan_status_pending` bit 0
  for `mcan_send_status`.
  In STOP the track is at 0 V: `lok_packets_next` writes pauses and TIOS bit 3 is clear.
  Holding STOP while stopped sets `mcan_status_pending` bit 9; on the master,
  `mcan_send_status` then zeroes every slot's speed and functions.
- `track_current_monitor` (0x9333) runs every pass:
  - `track_current` (0x10F2, mA) is the channel 4 value (× `flash_adc4_scale`) less
    `track_current_offset`, at least 0, or the channel 3 value (× 10 × `flash_adc3_scale`) if
    that is larger. In operating mode 0 it is the scaled channel 4 value alone. In
    `firmware_20260317_192757_usbdm.s19` `flash_adc4_scale` is 0, so the current is channel
    3's.
  - Two accumulators integrate the current above `flash_current_limit` (0xFF7A), each clamped
    to 0-10000: `overload_slow` (0x10F6), 1/100 of the excess over the limit + 200 mA, and
    `overload_fast` (0x10F8), the excess over the limit + 1200 mA.
  - Either accumulator nonzero, or `adc_track_voltage` outside 12000-25000: overload
    (`system_status_flags` bit 1) is set and `track_fault_timer` (0x10AE) rises by 3 per pass.
    Otherwise it falls by 1, and bit 1 clears at 0, or at once in STOP.
  - `track_fault_timer` reaching 4500 (at once when either accumulator exceeds 9999 or the
    voltage leaves 10000-27000): overload shutdown (bit 6, track output off) for 200 passes,
    and `shutdown_count` (0x10FA) + 1. A fifth shutdown with no 1000-pass gap (`shutdown_ticks`)
    sets STOP.
  - Counters: operating minutes every 60000 passes (`minute_countdown`), and the entries into
    overload (`operating_minutes`, `overload_events`, section 9).
- Version 4010 has no current measurement and a simpler monitor (section 12).

### 4.7 MM

- Entry (header of `src/signal/mm_encode.c`): {9, 8, pause 0x3C, address, function, data,
  pause 0x0F, address, function, data}. The pause before the pair is ≈ 6.15 ms and the one
  between the copies ≈ 1.54 ms. An MM bit is 208 µs; a trit is two bits (00 "0", 11 "1", 10
  open). An address is 4 trits, the function 1 trit, the data 4 trits.
- `mm_trit_table` (0xF35A): the address trits per address index. Index 0 is all open (the
  idle packet, `mm_write_idle`); index 80 is all "0", which is MM address 80, the address
  `lok_packets_next` programs decoders on.
- `mm_data_table` (0xF3BA): the data trits per packet kind and speed step (`mm_write_packet`).
- Writers: `mm_write_packet` (speed and direction, F1-F4, F5-F8 on the next address),
  `mm_write_direction` (the MM2 direction packet), `mm_write_reverse` (the MM1 reverse packet,
  speed step 1).
- Timing measured on the original: `../ms1-emu/docs/mm-decoder.md`, *Analysis*.

### 4.8 mfx

**Frames.**
- `mfx_put_bits` (0xCA80) writes plain bits into the entry. The cells, the bit stuffing and
  the sync patterns come from the generator (4.3).
- A frame entry (header of `src/signal/mfx_encode.c`): a leading 1 outside the CRC, the address
  (`mfx_frame_begin`: 0 + 7, 10 + 9, 110 + 11 or 111 + 14 bits; version 4010: section 12), the commands
  (`mfx_put_command` and the `mfx_put_*`), then the CRC-8 (polynomial 0x07, start 0xFF,
  `mfx_frame_end`). The bit position is `mfx_bit_pos` / `mfx_byte_idx` (0x12A2 / 0x12A3) and
  the running CRC `mfx_crc_state` (0x12A4).
- Drive and function frames use the command codes stored in the slot (`mfx_drive_cmd`,
  `mfx_function_cmd`), which come from the decoder's configuration. A direction change first
  sends emergency-stop frames (`mfx_put_estop`), then the new direction (`lok_packets_next`).

**Feedback window** (type 5, `mfx_cmd_feedback(n, mode)`):
- Step 0: a sync pattern (and a half one if the polarity needs it), the bits 0011, then
  capture start (opcode 2 with `mode`). The last step queues the stop (`mode` | 0x80).
- Mode 2, the 1-bit window: two pauses without edges, each a 10 µs edge followed by n/2 steps
  of 912 µs, with three half sync patterns between them. Every edge of the receiver clock
  (PT0) stores ATD0 channel 2 (the carrier level) as a byte. `mfx_feedback_bit` (0xCF0A)
  compares the first pause's samples with the second's.
- Mode 1, the data window: a 28 µs clock mark every 912 µs, and from step 24 on also one in the
  middle. Every rising edge of PT0 stores two bits, PTT bit 1 (data) and PB5 (quality).
  `mfx_feedback_data` (0xCFE4) reads them from 0x1B50 (the 21st edge): a start pattern, the
  bytes, a CRC byte (polynomial 0x07, start 0xFF), and a quality count that each quality-0
  bit halves.
- Capture driver (`src/signal/feedback_capture.c`): `timer_capture_start_bit_mode` (rising edges) or
  `timer_capture_start_adc_mode` (both edges) clears `timer_capture_buffer` (0x1B4B) and turns
  on the channel 0 interrupt. `isr_feedback_capture` stores samples up to the buffer end.
  `timer_capture_stop_and_align` or `timer_capture_stop_and_count` stops the capture and returns
  the sample count, which the signal interrupt stores in `track_capture_samples`. 0 means nothing
  was captured.
- The receiver on the board (an RDS demodulator: clock on PT0, data on PTT 1, quality on PB5,
  carrier level on ATD0 channel 2): `../ms1-emu/docs/mfx-decoder.md`, *Answers*.

**Registration engine** (`mfx_registration_process`, 0x4B8F; states in the header of
`src/signal/mfx_register.c`):
- A command that wants an answer takes three scheduler calls (`mfx_cmd_steps` counts 3 down to 1):
  the frame, `mfx_cmd_syncs(5)`, then the window. `timer_capture_ptr` = 0xFFFF marks the window
  as pending. Once the capture has stopped (`timer_capture_mode` 0), the next call evaluates
  it.
- The assign command (states 3, 4) is sent three times, without a window.
- While `mfx_cmd_steps` is nonzero, `lok_packets_next` is not called.
- `mfx_registration_start` (0x4A90) resumes with the UID stored in the EEPROM. Despite its
  name, `EE_MFX_UID` holds an mfx UID (`eeprom_save_task` writes `mfx_uid_hi`/`mfx_uid_lo`
  there). The decoded answers go into the slot record (`mfx_read_buf`,
  `src/signal/mfx_register.c`); CAN only sees the resulting slot changes.

### 4.9 DCC

`src/signal/dcc.c`, in page $3C (`src/include/page3c.h`): `lok_packets_next` hands a DCC slot
(`LOK_DCC`) to `dcc_slot_job`, `lok_packets_refresh` to `dcc_slot_refresh`. Each call writes
one packet as a type-6 entry (4.2); the packet formats, the jobs and the refresh are in the
file's header.

- **Packets** (RCN-211, RCN-212, RCN-214): a 16-bit preamble (`DCC_PREAMBLE`); the address in
  one byte (1-127) or two (0xC0 | high, low: above 127 or with `LOKP_DCC_LONG`); speed in 128,
  28 or 14 steps (`LOKP_DCC_STEPS`) from the slot's 0-127; the function groups F0-F4, F5-F8,
  F9-F12, F13-F20; CV writes on the main (POM), each sent twice.
- **Spacing:** when the entry before is for the same decoder, an idle packet comes first, so
  two packets to one decoder are at least 5 ms apart.
- **Jobs** (`track_pending`): VMAX, ACC, DEC, REG (`prog_cv`, `prog_value`) and RESET as POM
  writes of CV5, CV3, CV4, the CV and CV8 = 8 (RESET then sets the entry to `default_address`,
  as for MM); then speed and direction; then the groups of the changed functions. An address
  change confirmed with "PROG LOK?" programs the decoder in service mode (4.10).
- **Refresh:** a slot's speed and its used function groups (a function with a key or one that
  is on) in turn, one packet per call (`mm_phase`), then the next slot.
- **Signal** (measured with `../ms1-emu`'s DCC decoder, `tools/scenarios/dcc.txt`): halves
  57.5-58.5 µs and 99.6-100.4 µs, the two halves of a 1 within 0.9 µs; at least 9.8 ms
  between packets to one decoder; the track interrupt's slack and the staging ring as with
  MM and mfx (README *Interrupt timing*).
- **New locomotive:** NEUE LOK → DCC (`menu_item` 3) asks for the address (1-9999) and the
  symbol as ADRESSE does for MM, with 128 speed steps.

### 4.10 DCC service mode

`src/signal/dcc_service.c`, in page $3C (state: `dcc_svc`, `src/include/dcc.h`). Service mode
(RCN-216, direct mode) reads and writes CVs of the decoder on the track, and of every other
decoder there, so it starts only after "PROG LOK?".

- **The track:** while `dcc_svc.state` is set, `track_scheduler` hands every command entry to
  `dcc_service_next`: no locomotive packets, no mfx, no filler. STOP is switched off meanwhile
  (the track needs power) and back on at the end if it was on; `system_status_flags` bit 3
  shows PROG.
- **A job:** a list of operations (a CV read: 8 bit verifies, then the byte; a write; an
  address: CV1 and CV29 bit 5, or CV17, CV18 and bit 5). Per operation 3 resets (20 at the
  start: the decoders' power-on), 5 instruction packets, then resets until 60 ms after the last
  instruction was written (the command buffer runs at most that far ahead). Preamble 22 bits.
- **The acknowledgement:** `dcc_ack_sample`, every main-loop pass while service mode is on.
  Version 4011: `track_current` at least 50 mA above its running baseline for 4 passes.
  Version 4010: its over-current comparator, PB6 low for 4 passes, with the threshold lowered
  to about 60 mA (PWM channels 2-3, duty 41) and its overload check paused; the threshold
  goes back at the end. Over 500 mA for 100 ms (4011) ends service mode with STOP.
- **Uses:** LOK ÄNDERN → ADRESSE after "PROG LOK?" (`dcc_slot_job`, track_pending bit 2), CV
  LESEN, LESEN (4.11). A job without an acknowledgement shows FEHLER; the writes have reached a
  decoder that does not acknowledge all the same.

### 4.11 The programming items

`src/ui/prog.c`, in page $3C: LOK ÄNDERN's items after SPEICHERN (`CHANGE_SAVE`), on the master,
for MM and DCC locomotives (`include/ui.h`: `PROG_READ_CV` ... `CHANGE_LAST`):

| Item | What it does |
|---|---|
| CV LESEN (DCC) | "PROG LOK?", then CV after CV: the knob chooses, OK reads (service mode): `CV8=13` |
| LESEN (DCC) | "PROG LOK?", then CV29, CV1 or CV17/CV18 read: the locomotive takes the decoder's address, long form and speed steps (14, else 128) |
| SYMBOL | the symbol, shown while the knob turns; ESC keeps the old one |
| TASTEN | a function key or LIGHT selects a key (its icon blinks); the knob sets the function it switches (F0-F15, none: another key that had it loses it), a knob push the next icon beside the key, OK momentary or not |
| FAHRSTUFEN (DCC) | 128, 28 or 14 speed steps |

The keys: `prog_keys` gives per key its input (PORTA bit, the light key PB2) and the icons the
glass has beside it, with the function symbol each stands for; a function's key byte is key
<< 4 | icon variant, variant 3 none. The places of the PORTA bits (left F1-F4 from the top)
come from the manual. TASTEN numbers the CAN objects as `lok_assign_functions` does but keeps
the keys the user chose.

## 5. Locomotive slots

`src/include/lokstack.h` owns the layout. `lok_records` (0x13D7) holds what is stored about a
locomotive; it is the same layout as the EEPROM copy at `EE_SLOTS`. `lok_states` (0x17F7)
holds what changes while it runs. `lok_edit` is the record being added; on the master,
`mcan_lokstack_sync` moves it into a free slot.

Subsystems hand work to each other through a slot state's pending words. A change sets the
same bit in the word of each subsystem that must act on it; for example, a speed change in
`ui_task` sets bit 0 of both `track_pending` and `can_pending`. Each consumer clears the bits it
has served.

| Word (`lok_state_t`) | Consumer | Result |
|---|---|---|
| `track_pending`, `track_functions` | `lok_packets_next` | track packets and frames |
| `can_pending`, `can_functions` | `mcan_lokstack_sync` | CAN frames |
| `save_pending` (`SAVE_BITS`) | `eeprom_save_task` | the slot's EEPROM copy |
| `request` (slave) | `mcan_lokstack_sync`, `mcan_request_advance` | the slave's requests to the master |

Writers are `ui_task` (`src/ui/ui_control.c`), the CAN receive side (`mcan_receive_lok_state`,
`mcan_object_request`), `mfx_registration_process` (slots found or dropped), `mcan_send_status`
(emergency stop) and `lok_packets_next` (an MM decoder's new address). Bit meanings:
`lok_state_t` in `src/include/lokstack.h`.

## 6. CAN

### 6.1 Driver (`src/can/can0.c`)

- `can_controller_full_init(rate)` (0xDDB5) takes the bit rate in units of 100 bit/s.
  `app_main_init` passes 0x9C4, which gives 250 kbit/s: 24 time quanta per bit from the
  24 MHz bus, sampled three times (BTR1 0xFE).
- Acceptance: both 32-bit filters compare only bits 3 and 4 of IDR1: the first passes every
  extended (29-bit) frame (SRR and IDE set), the second every standard data frame (RTR and
  IDE clear). The receive and overrun interrupts
  are on (CAN0RIER 3).
- A ring entry is 13 bytes: IDR0-3, 8 data bytes and DLC, as the MSCAN buffers hold them.

| Ring | Buffer | Written by (pointer) | Read by (pointer) |
|---|---|---|---|
| receive | `can_rx_ring` (0x19AB-0x1B4A) | `can0_rx_isr` (`can_rx_ring_write`, 0x1923) | `can_rx_dequeue_frame` (`can_rx_ring_read`, 0x1921) |
| transmit | `can_tx_ring` (0x1929-0x19AA) | `can_tx_enqueue_frame` (`can_tx_ring_write`, 0x1927) | `can0_tx_isr` (`can_tx_ring_read`, 0x1925) |

The ring pointers' names say rx where they serve the transmit ring, and tx where they serve
the receive ring.

- `can_tx_enqueue_frame(id, data, dlc)` (0xDE43) packs a 29-bit identifier (a negative value:
  an 11-bit standard one) into IDR format. It returns 0 and drops the frame when the ring is
  full; otherwise it turns on the transmit interrupt.
- `can0_tx_isr` sends one frame per interrupt through transmit buffer 0 and turns its
  interrupt off when the ring is empty.
- `can0_rx_isr` copies each received frame without checking for a full ring, so an overrun
  overwrites unread frames.
- `can_rx_dequeue_frame(dest)` (0xDEAC) unpacks the identifier into 32 bits (standard
  identifiers negative), 8 data bytes and DLC & 0x0F: 13 bytes at `dest`. It returns 0 when the
  ring is empty. With `dest` 0 it only reports whether a frame waits.
- `can_poll` (0xDD98) and `can_poll_tx` (0xDDA6) call `can0_rx_isr`
  and then `can0_tx_isr` like subroutines. Nothing calls them, in the original too.

### 6.2 Protocol (`src/can/`)

- **Identifier** (`src/can/mcan.c` header): command bits 0-6, type bits 7-9, hash bits 10-25,
  priority bits 26-28. The command field carries the sender's node number (0 the master) or
  0x7E in registration frames.
- **Receive and send:** `mcan_receive` (0x5E70) splits a frame into `mcan_rx_node`,
  `mcan_rx_cmd`, `mcan_rx_handle` and `can_rx_frame_buf` (0x106F); macros at the end of
  `src/include/globals.h` name its data fields. `mcan_send` (0x5EA5) builds the identifier.
  It drops frames with command 0 (and returns 1) while `mcan_peer_node` is 0, so the
  master sends nothing under command 0 until a slave has registered.
- **`mcan_process`** (0x801A) runs every 4 ms.
  - Master: on its first call it puts eight frames with identifier 0x380 (type 7, all data 0)
    straight into the transmit ring. It takes one slave's registration (`master_frames`),
    answers each step, and gives the slave node number 1 in the last answer. It then handles
    the slave's frames (`mcan_receive_control`, `mcan_object_request`, `mcan_master_request`,
    `mcan_receive_status`, `mcan_receive_lok_state`) and polls the slave every 64 calls. After
    0x4FF calls (≈ 5.1 s) without a frame from the slave, `mcan_slave_lost` releases the
    slave's slots and forgets it.
  - Slave: registers in steps under command 0x7E (`slave_frames`, `registration_data`): steps
    0-3 send its UID (flash 0xFF10-0xFF13) a byte at a time in the hash, steps 4-7 eight data
    bytes each from the flash metadata and the configuration area. The master's last answer
    gives it its node number (`mcan_node`) and two flags: enter the bootloader, or
    set `system_status_flags` bit 5. It then takes the master's slot list and slots
    (`slot_from_master`) and polls the master every 64 calls. It restarts (`crt_restart`) on
    the master's reset request, when the master frees the locomotive it drives, or after
    0x4FF calls without a frame.
  - Both then run `mcan_send_status` (status frames for the `system_status_flags` bits flagged
    in `mcan_status_pending`), `mcan_lokstack_sync` (slot changes, section 5) and
    `mcan_send_device_name`.
- **Registration state lives in variables named after locomotive data:**

  | Variable | Holds |
  |---|---|
  | `mcan_reg_step` | the registration step, then on the slave the slot-list step |
  | `mcan_reg_mid` | a 5-bit tag in hash bits 0-4 of registration frames, changed when a registration stalls |
  | `mcan_peer_node` | on the master, the registered slave's node number (0: none) |
  | `mcan_node` | on the slave, its own node number |
  | `mcan_peer_timeout` | the silence countdown |
  | `mcan_peer_uid_hi`, `mcan_peer_uid_lo` | on the master, the slave's UID |

- What each frame carries: the headers of `src/can/mcan_status.c`, `src/can/mcan_lokstack.c`,
  `src/can/mcan_object.c`, `src/can/mcan_slave.c` and `src/can/mcan_name.c`.

## 7. Display

- **Hardware:** a PT6547 LCD driver on SPI0 (MODRR routes SPI0 to port M), with its strobe on
  PM3. `lcd_init` (0xC268) sets up SPI0 as an interrupt-driven master: mode 3, MSB first,
  SPI0BR 0x77 (11.7 kbit/s, so one byte takes ≈ 0.68 ms and a full pass over `lcd_buf`
  ≈ 30 ms).
- **`lcd_buf`** (0x1108-0x115F) holds [strobe flag, data] pairs. On each SPI interrupt,
  `isr_spi0_lcd` drives PM3 low for flag 0 and high otherwise, writes the data byte to SPI0DR,
  and wraps at the end. The chain started by `lcd_init` never stops. The images at 0xEE07 and
  0xF5E8 lay the buffer out as four blocks, each an address byte (flag 0, 0xA2) and ten data
  bytes (flag 1), the last of them the block code 0-3. The data bytes are bit-reversed
  (`src/include/lcd.h`).
- **Writers:** the main loop writes `lcd_buf` while the interrupt reads it, without
  synchronisation. The drawing routines are `lcd_render_text`, `lcd_set_bar`,
  `lcd_clear_segments` and `lcd_text_width` (`src/ui/lcd.c`). The UI files also set icon
  bits directly (`src/include/lcd_icons.h`, and the `lcd_buf` field macros at the end of
  `src/include/globals.h`).
- **Text:** `ui_screen_text` composes `lcd_text` (0x108C) and places it (`lcd_text_layout`,
  `lcd_text_offset`). `lcd_draw_text` draws it with `lcd_render_text`. A character with bit 7
  set blinks: while `ms_tick` bits 8-9 are both 0 it is drawn as a blank of the same width.
- **Start-up:** `lcd_init` loads the blank image and starts the SPI chain. `lcd_buf_init` loads
  the start-up image, and `lcd_pump_all` sends it with interrupts masked (`lcd_pump_start` step
  by step, the buffer twice over). `lcd_buf_load` blanks the display at the end of
  `app_main_init`.
- **Glass maps** (`src/ui/lcd_tables.c`, declared in `src/include/rom_tables.h`; the address
  is the original's):

  | Address | Symbol | Content | Read by |
  |---|---|---|---|
  | 0xEE07 | `lcd_blank_image` | the blank `lcd_buf` image | `lcd_buf_load` |
  | 0xEE5F | `lcd_icon_segments` | an (`lcd_buf` offset, mask) pair per icon (indices in `src/include/lcd_icons.h`) | `lcd_clear_segments`, `src/ui/lok_icons.c` |
  | 0xEF05 | `lcd_font` | 8 bytes per character code: width in the low nibble of byte 0, then column bytes | `lcd_render_text`, `lcd_text_width` |
  | 0xF450 | `lcd_bar_segments` | the speed bar: an (offset, mask) pair per bar segment | `lcd_set_bar` |
  | 0xF46C | `lcd_text_dots` | the text dot matrix, 38 columns: five (offset, mask) pairs per column, one per dot, bit 7 of the glyph's column byte first | `lcd_render_text` |
  | 0xF5E8 | `lcd_start_image` | the start-up `lcd_buf` image (most segments on) | `lcd_buf_init` |

## 8. Keys, knob, analog inputs

- **Keys:** `button_scan` (0xD76F) runs every 32 ms. It reads PORTA (the function
  keys) and PORTB bits 0-4, active low (`gpio_read_porta_active_low`,
  `gpio_read_portb_active_low`), into `porta_buttons_raw`, `porta_buttons_edge`,
  `button_state_raw` and `button_state_edge`. It also keeps hold counters for PB0
  (`ui_esc_held`) and STOP (`ui_stop_held`). The key bits are listed in the header of
  `src/ui/ui_control.c`.
- **Knob:** `encoder_sample` (0xBAB1) runs twice per pass.
  - It reads PTT bits 6 and 7 through shift-register debouncing (`encoder_a_debounce`,
    `encoder_b_debounce`, masked by `encoder_debounce_mask`) and a quadrature transition
    table.
  - Each step moves `encoder_count` by ± `encoder_step` (flash 0xFF64 when nonzero) within
    `encoder_range`, wrapping or clamping by `encoder_mode`. `encoder_set_count` (0xBA96) sets
    these three.
  - Each step also counts up the knob step counter (`encoder_clicks_hi`,
    `encoder_clicks_lo`), which `ui_task` copies into the EEPROM counter record.
- **ADC:** `timer_atd_init` sets ATD0 to convert channels 0-7 continuously.

  | Channel | Read by | Use |
  |---|---|---|
  | 0 | `adc_read_and_scale_voltages` | `adc_track_voltage`: the role (section 1), the voltage window (4.6) |
  | 1 | `adc_read_and_scale_voltages` | `adc_decoder_voltage` |
  | 2 | `isr_feedback_capture` | mfx feedback carrier level (4.8) |
  | 3, 4 | `track_current_monitor` | track current (4.6; not read by version 4010) |

- **Calibration words:** the bootloader's flash metadata at 0xFF60-0xFF7F (`flash_adc*_scale`,
  `flash_current_limit`, `flash_encoder_step` in `gcc/ms1.ld`), part of the device's
  bootloader sector (`docs/bootloader.md`).

## 9. EEPROM

- **Layout:** `src/include/eeprom_config.h` (EEPROM at 0x0800, `INITEE` in `_start`).
- **Access:** `eeprom_erase_sector` and `eeprom_program_word` (`src/hal/eeprom.c`) work on 4-byte
  sectors. Program waits for completion; erase does not, so `eeprom_save_task` programs an
  erased record on its next call (`eeprom_write_finish`).
- **Saving:** `eeprom_save_task` (0x8E87, every 32 ms) writes one record per call. It serves
  the items in `ee_save_pending`, then slots with `save_pending` bits, and otherwise the
  counter ring (the `src/sys/eeprom_save.c` header).
- **Counters:** the ring at `EE_RECORDS` holds records of kind 1-3, copies of `eeprom_record`.

  | Kind | Counter | Counted by |
  |---|---|---|
  | 1 | `operating_minutes` | `track_current_monitor` |
  | 2 | `overload_events` | `track_current_monitor` |
  | 3 | knob steps (`encoder_clicks_rec_hi`, `encoder_clicks_rec_lo`) | `encoder_sample`, copied by `ui_task` |

  `counter_inc_24bit` counts kinds 1 and 2. `app_main_init` loads the newest record of each
  kind and repairs the ring.

## 10. Peripherals

| Peripheral | Set up by | Used by |
|---|---|---|
| clocks, memory map | `_start` (PLL, INITRG/INITRM/INITEE, PPAGE $3D) | — |
| timer: TSCR1 | `timer_atd_init`, `timer_modulus_init`, `signal_gen_init` (timer on, fast flag clear) | — |
| timer: TSCR2 | `signal_gen_init` (prescaler 4) | — |
| timer channels 3-5 (output compare) | `signal_gen_init` | `isr_timer_ch4_signal_gen`; TIOS bit 3: `track_current_monitor`, `crt_restart`, `enter_bootloader`, the restart paths in `src/ui/ui_control.c` (version 4010: channels 4 and 5 only, section 12) |
| timer channel 0 (input capture) | `timer_capture_start_bit_mode`, `timer_capture_start_adc_mode` (TCTL4 edges) | `isr_feedback_capture` |
| timer channels 6, 7 | `timer_atd_init` (no capture edges) | PT6/PT7 are read as port bits by `encoder_sample` |
| modulus down-counter | `timer_modulus_init` (1 ms) | `isr_ms_tick`; `timer_modulus_read` reloads it at the end of `app_main_init` |
| ATD0 | `timer_atd_init` | section 8 |
| SPI0, PM3, MODRR | `lcd_init` | `isr_spi0_lcd` |
| MSCAN0 | `can_controller_full_init` | `can0_tx_isr`, `can0_rx_isr`, `can0_error_isr`, `can_tx_enqueue_frame` |
| EEPROM (ECLKDIV, ECMD, ESTAT) | `eeprom_erase_sector` | `eeprom_program_word` |
| PORTA | pull-ups (`timer_atd_init`) | function keys |
| PORTB | PB7 output low, pull-ups (`timer_atd_init`) | PB0-PB4 keys; PB5 feedback quality; version 4010: PB6 over-current, PB7 booster line (section 12) |
| PWM channels 0-3 | version 4010 only: `signal_gen_init` (section 12) | — |
| PTT | PT3-PT5 outputs (`signal_gen_init`) | PT0 feedback clock, PT1 feedback data, PT3-PT5 track, PT6/PT7 knob |
| PORTE bit 2 | output, low (`lcd_init`) | high from `app_main_init` once the display is up; low again in `crt_restart` and `enter_bootloader` |
| SCI0 | `src/hal/serial_debug.c`, only with `DEBUG=1` (`MS1_DEBUG_SERIAL`) | — |

## 11. Device library

`src/lok/library.c`, in page $3C (`src/include/page3c.h`): locomotives the user saved (MM and
DCC), in flash page $3C's top 4 KiB, 0xB000-0xBFFF (`LIB_START`; `gcc/page3c.ld` ends the code
below it).

- **Use:** LOK ÄNDERN → SPEICHERN (`CHANGE_SAVE`) saves the selected locomotive, replacing its
  record (found by protocol and address) or adding one; NEUE LOK → DATENBANK lists them,
  newest first, and adds the chosen one as the original's database did (`library_to_edit`
  into `lok_edit`). At most `LIB_MAX` devices.
- **Layout:** 8 blocks of 512 bytes (the flash's erase sector), 8 rows of 64 bytes each: kind,
  id, sequence number, the record (name, protocol, symbol, flags, address, functions), a
  check byte, and a correction word for the bootloader's CRC (`docs/bootloader.md`, rule 3;
  the computation in the file's header).
- **Log:** rows are written in ring order with rising sequence numbers. Before the writing
  moves into a block, the block after it is collected: its devices' newest rows move to the
  new block with new numbers and it is erased, so one block is always erased. Walking back
  from the newest row meets every device's newest row first (`walk_device`), a single pass.
- **Writing** (`flash_run`): the flash cannot be read while it programs or erases, so a short
  routine is copied to RAM (`flash_ram`) and runs with interrupts masked, about 1.5 ms per row
  and 20-27 ms per erased block (190 kHz flash clock, `FCLKDIV` set on first use). The track
  signal interrupt waits meanwhile, so `library_save` sets STOP first, writes on its next
  call, and clears STOP if it set it.
- **Tested** with `../ms1-emu`'s flash model: saving one locomotive 70 times (the ring wraps,
  blocks are collected) and eight locomotives with one of them saved 60 more times (live rows
  of the collected block move on); the flash written out with `-F` boots through the
  bootloader's CRC check and lists them all (`tools/scenarios/library.txt` covers one run).

## 12. Version 4010

Releases 4010 and c010 run application version 4010, for the MS1 hardware without track current
measurement. The source builds it with `MS1_RELEASE` 0x4010 (README *Release images*); its
function entries are in `tools/functions_4010.tsv`. Everything not listed here is the same code.

| | Version 4011 | Version 4010 |
|---|---|---|
| Over-current | track current from ATD0 channels 3 and 4, against `flash_current_limit` in software (4.6) | the booster's comparator pulls PB6 low above a threshold that `signal_gen_init` sets from `flash_current_limit` with PWM channels 2-3 (concatenated, centre-aligned, period 0x0FFF, duty limit × (limit + 1000) / 1550); PWM channels 0-1 get duty 39 |
| Booster line: staging control bit 7 (4.4) | PT3, set at the entry's compare (TCTL2 OL3; channel 3's compare reloaded with each entry) | PB7, set at once by `isr_timer_ch4_signal_gen`, inverted: bit 7 drives PB7 low |
| `signal_gen_init` | channels 3-5: TIE, TIOS, PTT3 high, DDRT, TC3-TC5, TCTL2 | channels 4 and 5 only; the PWM set-up; PB7 low and an output |
| Track off on restart (`crt_restart`, `enter_bootloader`, the restart paths in `src/ui/ui_control.c`) | TIOS bit 3 clear | PB7 low (`TRACK_OUTPUT_OFF`) |
| `track_current_monitor` | 4.6 | overload while PB7 is high and PB6 low, or `adc_track_voltage` outside 12000-25000: bit 1 at once and `track_fault_timer` + 3 per pass; at 4500, or at once outside 10000-27000, STOP (bit 0). Back in range the timer falls by 1 and bit 1 clears at once. No shutdown (bit 6), no TIOS, no current in operating mode 0 |
| Start-up screen (operating mode 0) | measures the current zero (section 2) | counts `menu_top` down only |
| INFO item 9 (`ui_screen_text`) | `IT:` the current and the limit, while `ms_tick & 0xE0` is 0 | `OVC:` the limit (`a / 100 % 10` for the tenths), every pass |
| `mfx_frame_begin`, 9- and 11-bit addresses (4.8) | 10 + 9 and 110 + 11 bits | 01 + 9 and 011 + 11 bits: not the mfx format (`../doc/MFX - Trackprotocol.pdf`, frame structure), kept as the original sends them. The MS1 assigns SIDs from 1, so only SIDs from 128 to 2047 meet it |
| RAM (`tools/releases.py`) | `src/globals.c` | no variables at 0x10F2-0x10FC (`track_current` … `shutdown_ticks`); every variable from 0x10FD up 0x0B lower, the GCC soft registers at 0x1BAC |
| Flash, in the original | application 0x4000-0xE11A, its read-only data from 0xE11B | 0x4000-0xDFA0, from 0xDFA1; the tables hold the same bytes, so this build has one set (README *Image layout*) |
| Identification area (`src/sys/ident.c`) | "Mobile Station 2 MFx Motorola", version word 0x4011 | "Mobile Station MFx Motorola", 0x4010 |
| DCC service mode's acknowledgement (4.10) | `track_current` against a baseline | PB6, the comparator threshold lowered to about 60 mA meanwhile, its overload check paused |
