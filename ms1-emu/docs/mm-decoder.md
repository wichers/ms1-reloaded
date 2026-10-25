# Märklin Motorola decoder

`-D mm:ADDR[,mm1][,packets]` (usage and the common parts: [decoders.md](decoders.md)). Source:
`src/decoder_mm.c`.

## Events

`NAME` is `mm:ADDR`, or `mm1:ADDR` for an MM1 decoder:

| Event | When |
|---|---|
| `speed +N` / `speed -N` | the speed level (0–14) or direction changes; `-` is reverse |
| `F0 on` / `F0 off` | the function trit changes |
| `f1 on` … `f4 off` | an MM2 function packet changes f1–f4 |
| `reverse packet: stop, the direction stays (MM2)` | an MM2 decoder takes a reverse packet (speed code 1) after it has seen MM2 packets |
| `track voltage on after X ms off` | the track had 0 V for longer than `POWER_OFF_US` |
| `packet F.. ABCD=v EFGH=....` | (`packets`) every command for the address: function trit, speed code, second pulses |

The state lines (at the end, and in the TUI) read
`mm:3  speed -3/14  F0 on  f1-f4 1100  (394 commands)`, with `no track voltage` appended
while the track is off.

## Decoding

From A. Scorzoni, *The manual of the new Märklin-Motorola format* (`../doc/`), as summarised
in the header comment of `src/decoder_mm.c`.

- **Bits.** Each positive pulse starts a bit. The cell is the time from rise to rise:
  208 µs for locomotives, 104 µs for solenoid decoders. Packets with a mean cell below the
  midpoint are counted as solenoid packets and otherwise ignored. A pulse over 0.6 of the
  cell is a 1, under 0.4 a 0. Anything in between (symmetric cells, as in mfx) makes the
  packet "not MM".
- **Packets.** A negative level longer than `MM_PAUSE_US` ends a packet. A packet must have
  exactly `MM_BITS` bits, and every cell must be within ±20 % of nominal; otherwise it counts
  as bad bit timing or not MM. A 0 V level aborts the packet in progress.
- **Double packets.** A decoder acts only on a packet repeated identically within
  `MM_REPEAT_US` of its end. The repeat pause t1 is checked against the MC145027's dead-time
  limit (`MC145027_DEAD_TIME_US`).
- **Address.** The first 4 trits (pairs 00 = 0, 11 = 1, 10 = open, the first trit least
  significant). Numeric 0 is address 80; all open is the idle packet. A pair 01 is no trit
  and counts as an unknown code.
- **Data.** The 5th trit is F0. Then the pairs A E B F C G D H: speed code ABCD (A least
  significant, 0 stop, 1 reverse, 2–15 level 1–14).
  - EFGH = ABCD is the MM1 format.
  - Otherwise MM2: EFGH 1010/1011 is reverse and 0101/0100 forward (with H = not D), or a
    function: EFG 110/001/011/111 = f1–f4, H = on.
  - Also MM2: the codes that would read as MM1 (1010 with D = 0 at speed codes 3, 4, 6, 7:
    f1–f4 off; 0101 with D = 1 at 11, 12, 14, 15: f1–f4 on). A function packet also sets the
    speed unless its code is 1.
- **MM1 decoder** (`mm1`): it uses only A B C D. A reverse packet stops it and toggles the
  direction, once per run of reverse packets.
- **MM2 decoder** (default): it takes the absolute direction from MM2 packets. A reverse
  packet stops it, and toggles the direction only before any MM2 packet was seen.

## Analysis

Printed at the end of the run (`mm_report`):

| Line | Meaning |
|---|---|
| `MM packets N (locomotive, solenoid)` | 18-bit packets with valid MM bits |
| `double packets` | packets taken (repeat found) |
| `unpaired` | packets with no identical repeat after them |
| `not MM (other signal)` | pulse trains that are not 18 MM bits: mfx frames, sync patterns |
| `bad bit timing` | 18 bits with a cell off by more than 20 % |
| `idle` | idle packets (all address trits open) |
| `unknown codes` | an address pair 01, or MM2 data that is no direction or function |
| `cell`, `pulse of a 1`, `pulse of a 0` | min–max, mean and count over locomotive packets (µs) |
| `repeat pause t1` | from the end of a packet to the start of its repeat (µs); a further line counts pauses over the MC145027 limit |
| `pause after a pair` | from the end of a double packet to the next packet, gaps under `MM_SEQUENCE_US` (µs) |
| `command interval` | time between two commands for the decoder's address (ms): how often the firmware refreshes the locomotive |
| `track voltage off` | 0 V periods longer than `POWER_OFF_US` (ms) |
| `commands by address` | double packets per locomotive address, whatever the decoder's own |

Measured on the original 4011 image with the calibration patch (`calpatch.py`), running
`ms1-firmware/tools/scenarios/drive.txt` for 27 s with `-D mm:3`:

```
mm:3: MM packets 1143 (locomotive 1143, solenoid 0), double packets 571, unpaired 0
  not MM (other signal) 454, bad bit timing 0, idle 195, unknown codes 0
  cell                     207.6-208.4 us, mean 208.0 (19431)
  pulse of a 1             181.6-182.5 us, mean 182.0 (7644)
  pulse of a 0             25.6-26.5 us, mean 26.0 (12930)
  repeat pause t1          1538.1-1538.2 us, mean 1538.2 (571)
  pause after a pair       6152.4-47593.1 us, mean 16585.0 (570)
  command interval         15.2-1008.1 ms, mean 29.0 (375)
  track voltage off        959.4-11381.3 ms, mean 6170.4 (2)
  commands by address: 3:376
```

The firmware's packets as the decoder sees them in that run:

- A knob press (direction change) sends the MM1 reverse packet (speed code 1, EFGH = ABCD),
  then MM2 packets with the new direction at speed 0.
- While a locomotive is driven, its speed packet alternates with function packets for
  f1–f4 in turn, each sent as a double packet.
