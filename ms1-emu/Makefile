# ms1-emu: MC9S12D64 / MS1 emulator.
#   make            build build/ms1emu
#   make check      CPU tests, and the decoder check against the Ghidra listing
#                   (needs ../ms1-ghidra/out/listing.tsv and $(IMAGE))
#   make diff       differential CPU test against sim6812 (needs its sources in $(SIM6812))
CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -g
SIM6812 ?= ../sim6812/src/sim6812
LISTING ?= ../ms1-ghidra/out/listing.tsv
IMAGE ?= build/ms1_4011.s19

CORE = src/cpu12.c src/cpu12_dis.c src/image.c
MCU = src/mcu.c src/ect.c src/spi.c src/eeprom.c src/atd.c src/mscan.c src/board.c src/tui.c src/canbus.c src/decoder.c src/decoder_mm.c src/decoder_mfx.c

build/ms1emu: src/main.c $(CORE) $(MCU) src/*.h src/cpu12_cycles.inc | build
	$(CC) $(CFLAGS) -o $@ src/main.c $(CORE) $(MCU)

# function-level differential test of a rebuild against the original (tools/difftest.c)
build/difftest: tools/difftest.c $(CORE) $(MCU) src/*.h src/cpu12_cycles.inc | build
	$(CC) $(CFLAGS) -o $@ tools/difftest.c $(CORE) $(MCU)

# disassembler for a range of an image (tools/dis.c)
build/dis: tools/dis.c src/cpu12_dis.c src/image.c src/cpu12.h | build
	$(CC) $(CFLAGS) -o $@ tools/dis.c src/cpu12_dis.c src/image.c

build/cpu_test: tests/cpu_test.c src/cpu12.c src/cpu12.h src/cpu12_cycles.inc | build
	$(CC) $(CFLAGS) -o $@ tests/cpu_test.c src/cpu12.c

build/decode_check: tests/decode_check.c $(CORE) src/*.h src/cpu12_cycles.inc | build
	$(CC) $(CFLAGS) -o $@ tests/decode_check.c $(CORE)

# sim6812 is 1999-2002 GNU C: gnu89 inline semantics and common symbols
build/diff_sim6812: tests/diff_sim6812.c src/cpu12.c src/cpu12_dis.c src/cpu12.h src/cpu12_cycles.inc | build
	$(CC) -std=gnu99 -fgnu89-inline -fcommon -O2 -w -DHAVE_CONFIG_H -I$(SIM6812) -o $@ \
		tests/diff_sim6812.c src/cpu12.c src/cpu12_dis.c \
		$(SIM6812)/instr.c $(SIM6812)/decode.c $(SIM6812)/common.c

check: build/cpu_test build/decode_check
	build/cpu_test
	build/decode_check $(IMAGE) $(LISTING)

diff: build/diff_sim6812
	build/diff_sim6812 20000

build:
	mkdir -p build

.PHONY: check diff

# CAN master on the emulator's bus, from the ms1-master library
MS1_MASTER ?= ../ms1-master
build/master_bridge: tools/master_bridge.c src/canbus.c src/canbus.h | build
	$(CC) $(CFLAGS) -I$(MS1_MASTER)/include -o $@ tools/master_bridge.c src/canbus.c \
		$(MS1_MASTER)/src/ms1_master.c $(MS1_MASTER)/src/ms1_types.c
