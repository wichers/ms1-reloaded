# Track signal generation

How the 4011 application turns locomotive commands into the track waveform, read from the Ghidra program `ms1_4011` (function names are imported IDA names; the behaviour below is from the code). Register and buffer addresses are CPU addresses; see [firmware-map.md](firmware-map.md) for the memory map.

**Summary.** Three port-T pins switch in hardware on timer output compares. A ring buffer of 16-bit *staging entries* — each entry the pin levels for the next edge plus the time to it — is consumed one entry per edge by the timer channel 4 interrupt. Protocol encoders (Märklin Motorola, mfx) only fill that buffer. A DCC encoder can use the same mechanism unchanged.

## Timer and pins

`signal_gen_init` (`0xC58E`):

| Code | Effect |
|---|---|
| `DDRT \|= $38`; `PTT &= $CF`, `PTT \|= $08` | PT3, PT4, PT5 outputs; PT3 high, PT4/PT5 low |
| `TIOS \|= $30` | channels 4 and 5 output compare |
| `TCTL1 \|= $0A`, `TCTL1 &= $FA`; `TCTL2 \|= $C0` | channels 4/5: clear pin on compare; channel 3: set pin on compare (the levels are rewritten per edge by the ISR) |
| `TSCR2 = 2` | timer prescaler ÷4 |
| `TSCR1 = $90` | timer on, fast flag clear |
| `TIE \|= $10` | channel 4 interrupt |

**Tick = 1/6 µs.** With the 24 MHz bus (`SYNR = 23`, `REFDV = 3`, 4 MHz oscillator) the ÷4 prescaler gives 6 MHz. The protocol encoders confirm it: their durations come out at exactly the Motorola and mfx specifications only with 6 ticks per µs, and some encoders convert microseconds with `MUL` by 6.

## Staging entry

| Bits | Meaning | ISR action |
|---|---|---|
| 15 | PT3 level after the next compare | `TCTL2` bit 6 (`OL3`) |
| 14 | track-feedback capture step | runs the action selected by `track_capture_mode` (start capture in bit or ADC mode, or stop and store `track_capture_value`), then takes the next mode from `signal_gen_encoder_param` |
| 13 | PT5 level after the next compare | `TCTL1` bit 2 (`OL5`) |
| 12 | PT4 level after the next compare | `TCTL1` bit 0 (`OL4`) |
| 11–0 | ticks until the next edge | added to `signal_timer_compare_base` and written to `TC3`, `TC4`, `TC5` |

`isr_timer_ch4_signal_gen` (`0xC4ED`) runs at each channel 4 compare, i.e. at each edge: it applies the control bits of the entry at `signal_gen_staging_read_ptr` to the output-compare actions, loads `TC3`/`TC4`/`TC5` with `signal_timer_compare_base` (the current edge time) and advances the base by the entry's duration. An entry with duration 0 only applies its control bits and the ISR takes the next one. The pins therefore change exactly at the compare, independent of interrupt latency; the longest edge-to-edge time an entry can express is 4095 ticks = 682.5 µs.

PT4 and PT5 are the two track phases: every Motorola and mfx edge has exactly one of bits 12/13 set. The role of PT3 (bit 15) is not visible from the code alone.

## Buffers and pipeline

| Buffer | Range | Pointers | Producer → consumer |
|---|---|---|---|
| command | `0x1268–0x1297` | `signal_gen_render_buf_ptr` (`0x101C`) | `signal_gen_cmd_buf_write` → `signal_gen_cmd_buf_consumer` |
| send | `0x1228–0x1267` | read `0x1018`, write `0x101A` | command consumer → `signal_gen_send_buf_executor` |
| staging | `0x1160–0x1227` (100 entries) | read `0x1014`, write `0x1016` | executor (via `signal_gen_staging_buf_write`) → `isr_timer_ch4_signal_gen` |

- `signal_gen_cmd_buf_consumer` dispatches command entries by type through the jump table at `0xC87C` (14 types) and a call table at `0xC899`.
- `signal_gen_send_buf_executor` interprets opcode/data pairs through the jump table at `0xC643` (22 opcodes). It runs only while the staging ring has more than 48 bytes free, so the main loop tops up the ring ahead of the ISR.
- `signal_gen_staging_buf_write` (`0xC66F`) stores one entry and tracks the phase of the last written entry in `signal_gen_stb_state` (`0x12A0`): an entry driving PT5 (bit 13) sets it to the PT4 phase (`$10`) and vice versa, so OR-ing `signal_gen_stb_state` into the next entry produces a phase change.

## Observed encodings

**Märklin Motorola** (executor, `0xC7DD–0xC7F4`), per bit shifted out of the frame:

| Bit | First entry | Second entry | Bit time |
|---|---|---|---|
| `1` | `$1444` — PT4, 1092 ticks = 182 µs | `$209C` — PT5, 156 ticks = 26 µs | 208 µs |
| `0` | `$109C` — PT4, 26 µs | `$2444` — PT5, 182 µs | 208 µs |

**mfx** (`signal_gen_fullbit_entry`, `signal_gen_halfbit_entry`): a full cell is `$258 | stb_state` (600 ticks = 100 µs) and a half cell `$12C | stb_state` (300 ticks = 50 µs); because every write flips the phase, a full cell is one transition and two half cells two — frequency-modulated coding with 100 µs cells.

Other executor opcodes compute durations as a data byte × 6 or × 12 (microseconds or double microseconds to ticks) and invert the phase with `EORA #$30`.

## Consequences for a DCC generator

- The ISR, timer setup and staging format carry DCC unchanged. A DCC `1` (58 µs half-bits) is `$1000 | 348` then `$2000 | 348`; a DCC `0` (100 µs half-bits) is `$1258` then `$2258`.
- Zero bits stretched beyond 682 µs need more than one entry per half-bit (same pin bit, durations summing to the total).
- Only the encoders (command consumer cases and executor opcodes) are protocol-specific; the command → send → staging pipeline and the free-space throttle can stay.
- Open: the hardware role of PT3 and of the capture path (bit 14, timer channel 0, ATD0 — see `isr_timer_ch0_track_capture`) need the schematic or a scope; a DCC cutout (RailCom window) would most likely use them.
