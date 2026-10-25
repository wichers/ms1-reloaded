# Track decoders

`ms1emu -D SPEC` attaches a locomotive decoder to the emulated track. The decoder decodes the
track voltage from its edge timing, as the decoder in a locomotive would. It logs what it does
and, at the end of the run, prints an analysis of the signal. An mfx decoder also answers over
the track, through the MS1's feedback receiver, so the firmware can find it, register it and
read its configuration. Use decoders to see what the firmware sends to a locomotive, and to
compare two images (the original and a rebuild) on the same scenario.

| Protocol | Reference |
|---|---|
| Märklin Motorola | [mm-decoder.md](mm-decoder.md) |
| mfx, and the feedback receiver | [mfx-decoder.md](mfx-decoder.md) |

Sources: `src/decoder.h` (API), `src/decoder.c` (the protocol-independent front),
`src/decoder_int.h` (shared by the implementations), `src/decoder_mm.c`, `src/decoder_mfx.c`.
The board feeds every attached decoder from `pins_out` in `src/board.c` and gives it the
feedback path (`board_feedback`).

## Usage

```sh
build/ms1emu -q -t 27 -s ../ms1-firmware/tools/scenarios/drive.txt -D mm:3 -D mm:3,mm1 build/ms1_4011.s19
build/ms1emu -q -t 42 -s ../ms1-firmware/tools/scenarios/mfx.txt -D 'mfx:7A3F0012,name=BR 185' build/ms1_4011.s19
```

| `SPEC` | Decoder |
|---|---|
| `mm:ADDR` | Märklin Motorola locomotive decoder, address 1–80, MM2 behaviour |
| `mm:ADDR,mm1` | an old MM1 decoder: it reads only the first pulse of each pair, and a reverse packet toggles its direction |
| `mm:…,packets` | also logs every command packet for its address |
| `mfx:UID` | mfx locomotive decoder with the 32-bit UID (hex, up to 8 digits) |
| `mfx:…,name=NAME` | the locomotive name in its configuration, up to 16 characters (default `mfx lok`) |
| `mfx:…,symbol=SYMBOL` | `elok` (default), `diesel`, `steam` or `none`: the symbol the MS1 shows |
| `mfx:…,cv=VVV/II:XX` | sets one configuration variable (hex: CV number, index, value) over the built-in image; repeatable — e.g. `cv=000/02:02`, a block 01 of version 2 |
| `mfx:…,noread` | answers no CV read (1-bit answers still): a decoder whose data answers do not arrive |
| `mfx:…,frames` | also logs every frame on the track, parsed |

- `-D` can be repeated, up to the size of `ms1_board_t.decoders`. Each decoder sees the whole
  track signal and acts on what is addressed to it.
- Batch runs: events go to stdout as they happen, and the analysis follows the end-of-run lines.
- Interactive (`-i`): no event lines, since they would scroll the screen. Each decoder's state
  is a line under the LCD (`decoder_status`), and the analysis is printed after quitting.
- A bad `SPEC` stops `ms1emu` with a message (`decoder_create`).
- `difftest -D SPEC` attaches decoders to the original's track as well (see the README). The
  copies that run a tested call have none, so the decoders follow the original only.

## Events

Each line is `TIME NAME EVENT`, with `TIME` in emulated seconds and `NAME` the decoder (`mm:3`,
`mm1:3`, `mfx:7A3F0012`). The events of each protocol are listed in its reference. The state
lines (at the end, and in the TUI) end in `no track voltage` while the track is off.

## Track voltage

The H-bridge outputs give the voltage each decoder sees (`pins_out`, `PORT_T`):

| PT4 | PT5 | Track |
|---|---|---|
| 1 | 0 | + |
| 0 | 1 | − |
| equal | equal | 0 V (track off) |

PT3 does not take part. The original firmware (4011 image) holds the track at 0 V during
start-up, until the signal generator starts, and from the first STOP press until the second.
A decoder starts at 0 V, so its first `track voltage on` event comes when the signal starts.

## Comparing two images

Run both images with the same scenario and decoders, then compare the events without their
times:

```sh
for img in orig.s19 rebuild.s19; do
  build/ms1emu -q -t 27 -s scenario.txt -D mm:3 $img | grep ' mm:3 ' | sed -E 's/^ *[0-9.]+ //' > $img.events
done
diff orig.s19.events rebuild.s19.events
```

- The rebuild's code runs at a different speed, so event times differ: by up to a few packet
  times for MM, and by tens of milliseconds for an mfx registration.
- Where the main loop and the 1 ms tick interrupt race for the command buffer, packets can
  come out in a different order, so intermediate speed steps can also differ.
- Scenarios that turn the knob faster than the menu reads it make the knob counts themselves
  depend on timing (see *Encoder* in the README).
- `difftest -D` points at the function that differs, call by call, where a whole-run
  comparison only shows that the runs part.
