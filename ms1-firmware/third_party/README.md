# third_party — parts of the original MS1 firmware

What this directory holds is the original firmware's, not original work of this repository:
the MS1 firmware is Gebr. Märklin & Cie. GmbH's product, developed by ESU electronic
solutions ulm (its identification area carries "(c) ESU 2006"). It is **not covered by the
repository's license** (`LICENSE` at the repository root, Apache 2.0). It is here so that the
firmware builds from this repository alone. Paths below are relative to `ms1-firmware/`.

| Path | What it is | Made and checked with |
|---|---|---|
| `third_party/loader/4011.s19`, `third_party/loader/4010.s19` | the RAM loader of the version's update files, as S-records at 0x1000-0x17FF: the code the bootloader runs to program an update (`docs/bootloader.md`, *Update mode*). Within a version, the releases' loaders differ only in identification strings their code does not read and in the CRC word | `../tools/bci.py loader` wrote them from the 4011 and 4010 update files; `make` puts the version's into each `build/<release>.bci` (`../tools/bci.py encode`) |
| `third_party/lokdb.tsv` | the locomotive database of page $3C: Märklin's and Trix's locomotives, one line per locomotive | `tools/lokdb.py extract` wrote it from a release image; the build turns it into page $3C (`tools/lokdb.py asm`); the columns are in `tools/lokdb.py` |

Outside this directory, `src/` has the original's contents where the firmware must match it:

- the read-only tables, as C: the menu texts (`src/ui/lang.c`), the display's font, glass maps
  and images (`src/ui/lcd_tables.c`), the identification area of each release
  (`src/sys/ident.c`), and the other tables `static` in the file that reads them;
- the instructions of the track signal interrupt and the feedback capture
  (`src/signal/signal_gen.c`, `src/signal/feedback_capture.c`), inline assembly that keeps the
  original's timing (README *Inline assembly*).

## Editing

- **Update file loader:** code, not edited here; `../tools/bci.py loader` takes it from an
  update file again.
- **Locomotive database:** edit `third_party/lokdb.tsv`; entries can be added, changed or
  removed. Page $3C holds the database in 16 KiB, with its entry table and two accessors
  (`src/include/page3c.h`, `src/lok/lokdb.c`).
