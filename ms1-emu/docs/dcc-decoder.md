# DCC decoders

`-D dcc:ADDR[,…]` (a locomotive decoder) and `-D dccacc:ADDR[,…]` (a basic accessory
decoder); usage and the common parts: [decoders.md](decoders.md). Source: `src/decoder_dcc.c`,
tests: `tests/dcc_test.c` (`make check`).

The decoders follow the RailCommunity standards (RCN-210 bits, RCN-211 packets, RCN-212
locomotive commands, RCN-213 accessory commands, RCN-214 CV access, RCN-216 service mode,
RCN-225 CVs; NMRA S-9.2, S-9.2.1, S-9.2.3). The receiver of the ZIMO DCC library
([github.com/ZIMO-Elektronik/DCC](https://github.com/ZIMO-Elektronik/DCC), `include/dcc/rx`)
is the reference for what a decoder accepts and how it enters and leaves service mode. The
code here is its own C, written from those documents.

## Specification

| `SPEC` | Decoder |
|---|---|
| `dcc:ADDR` | locomotive decoder, address 1–10239 (above 127 a long address: CV17/CV18, CV29 bit 5), 28/128 speed steps (CV29 bit 1) |
| `dcc:ADDR,long` | a long address below 128 |
| `dcc:…,steps=14` | 14 speed steps (CV29 bit 1 clear: bit 4 of the speed instruction is F0); `steps=28` or `128` keep bit 1 set |
| `dccacc:ADDR` | basic accessory decoder, decoder address 1–510 (CV1 bits 0–5, CV9 bits 6–8): ports 0–3, two outputs each |
| `…,cv=N:V` | sets CV N (1–1024) to V (decimal); repeatable |
| `…,ack=MA` | the acknowledgement's current, mA (default `ACK_MA`, 100) |
| `…,noack` | no acknowledgement in service mode: a decoder the MS1 cannot read |
| `…,packets` | also logs every packet the decoder receives, in hex without the XOR byte |

CVs at the start: CV1 the address (or 3 with a long one), CV7 1, CV8 13 (NMRA: public domain
and do-it-yourself), CV29 `0x02` (`0x22` with a long address); an accessory decoder's CV29
`0x80`. Writing 8 to CV8 restores them.

## Events

`NAME` is `dcc:ADDR` or `dccacc:ADDR`:

| Event | When |
|---|---|
| `speed +N/S` / `speed -N/S` | speed step N of S (126, 28 or 14 by the packet), direction `+` forward |
| `emergency stop +` / `emergency stop -` | an emergency stop instruction |
| `F3 on` / `F3 off` | a function changes (F0–F28) |
| `CV 5 = 200 (POM)` / `CV 1 = 5 (service)` | a CV write on the main (the second of two identical packets) or in service mode |
| `CV 8 = 8: CVs reset (…)`, `CV 7 = …: read-only (…)` | the CV8 reset; writes to CV7/CV8 |
| `verify CV 5 = 200 (POM: answered only by RailCom)` | a verify on the main: the MS1 has no RailCom receiver |
| `verify CV 1 = 3`, `verify CV 29 bit 1 = 1`, `…: no` | a service-mode verify, matching or not |
| `ack` | the acknowledgement: `ACK_MA` more track current for `ACK_S` (6 ms) |
| `service mode on` / `service mode off` | a broadcast reset enters it; a packet that is no reset and no service packet leaves it |
| `register/paged mode packet: not modelled` | a 2-byte service packet |
| `decoder digital reset` / `decoder hard reset` | decoder control instruction 0x00 / 0x01 to the address |
| `output 19 (port 2) 1 on` | accessory: output 1 of port 2 switched (19 = the output address, `(decoder address − 1) × 4 + port + 1`) |
| `output 19 (port 2) aspect N` | accessory: an extended (signal) packet |
| `track voltage on after X ms off` | the track had 0 V for longer than `POWER_OFF_US` |
| `packet 03 3F AB` | (`packets`) every packet received |

The state lines (at the end, and in the TUI) read
`dcc:3  speed +42/126  F0-F12 1100000000000  (157 packets)`, or for an accessory decoder
`dccacc:5  outputs 00 00 01 00  (12 packets)` (per port its outputs 0 and 1), with
`service mode` and `no track voltage` appended when they apply.

## Decoding

- **Half bits.** Each level between two edges is half a bit: 52–64 µs for a 1, 90–10000 µs for
  a 0 (`BIT1_*`, `BIT0_*`, ZIMO's limits). Anything else — MM pulses, mfx half cells — restarts
  the receiver and counts as "not DCC" when a packet was under way. A 0 V level restarts it too.
- **Packets.** At least `MIN_PREAMBLE` (10) ones, then each byte after a 0 start bit, a 1 end
  bit after the last byte; the two halves of every bit must be of the same kind. The XOR of
  all bytes must be 0 and a packet has at least 3 bytes. The end bit counts as the first bit
  of the next preamble.
- **Addresses.** 0 broadcast; 1–127 a short locomotive address; 128–191 an accessory (the
  decoder address's bits 6–8 complemented in the second byte); 192–231 a long locomotive
  address; 255 the idle packet. A locomotive decoder takes the short or the long form, as
  CV29 bit 5 selects.
- **Locomotive commands.** `0x3F DSSSSSSS` 128 steps (0 stop, 1 emergency stop, 2–127 steps
  1–126); `01DCSSSS` 28 steps (C the low bit: codes 0–1 stop, 2–3 emergency stop, 4–31 steps
  1–28), or 14 steps with C as F0; `100FFFFF` F0 (bit 4, with 28/128 steps) and F1–F4;
  `1011FFFF` F5–F8; `1010FFFF` F9–F12; `0xDE`/`0xDF` F13–F20 / F21–F28. Broadcast speed
  commands apply to every locomotive decoder.
- **POM.** `1110CCVV VVVVVVVV DDDDDDDD` (CV = V + 1): CC 11 writes the byte, 10 a bit
  (`111KDBBB`, K = 1 writes), 01 verifies. A decoder acts on the second of two identical
  packets in a row for it. Writing CV1 clears CV29 bit 5, as ZIMO's decoder does (RCN-225
  permits it).
- **Service mode** (direct mode). A broadcast reset (`00 00`) enters it. Then `0111CCVV
  VVVVVVVV DDDDDDDD` with the codes of POM, acted on at the second of two identical packets:
  every write, and a verify that matches, acknowledge. Resets keep the mode; any other packet
  ends it.
- **Accessory commands.** `10AAAAAA 1aaaCPPO`: decoder address A (9 bits), port PP, output O,
  C on or off. `10AAAAAA 0aaa0PP1 DDDDDDDD`: an aspect.

## Acknowledgement

The decoder answers through the board's feedback path (`decoder_feedback_t.current`): the
board adds `ACK_MA` to the track current for `ACK_S`, on ATD0 channel 3 — version 4011's
current input — in counts of the device's calibration word `flash_adc3_scale` (0xFF66):
`ATD0DR3 × 10 × scale >> 16` mA, about 67 mA per 10-bit count with the dump's 0x1AA2. Add the
current to the scenario's channel 3 value with `-a 3=V` (the default 512 reads as 0 mA there,
the firmware's product overflows). Version 4010 has no current input: the board pulls PB6, its
over-current comparator, low while the decoder's extra current is above the threshold the
firmware sets with PWM channels 2-3 (`comparator_tripped`: duty = L (L + 1000) / 1550 for L
mA, as `signal_gen_init` computes it); the firmware lowers that threshold in service mode.

## Analysis

Printed at the end of the run (`dcc_report`):

| Line | Meaning |
|---|---|
| `DCC packets N (bad checksum, too short), for this decoder` | packets with a valid preamble and bits, those failing the XOR or length check, those for the decoder's address |
| `idle, reset, broadcast, accessory, service mode, acknowledgements` | packets by kind; the acknowledgements given |
| `not DCC (other signal)` | packets under way that an invalid half bit ended |
| `half bit of a 1`, `half bit of a 0` | min–max, mean and count over the halves of valid packets (µs) |
| `halves of a 1 differ` | the difference between the two halves of each 1 (µs) |
| `preamble` | the preamble length of valid packets (bits) |
| `packet interval` | time between packets for the decoder's address (ms): how often the firmware refreshes it |
| `track voltage off` | 0 V periods longer than `POWER_OFF_US` (ms) |
| `packets by locomotive address` | valid packets per locomotive address, whatever the decoder's own |
