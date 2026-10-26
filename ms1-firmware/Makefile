# GNU build of the MS1 firmware: GCC 3.3.6 m68hc1x (the compiler family the original was built
# with), binutils 2.18. Run inside the toolchain container:
#   tools/hc12-gcc/run make            -> build/ms1.s19 (flashable, bootloader included)
#   tools/hc12-gcc/run make images     -> the release images: build/<release>.s19/.bin/.bci
#   tools/hc12-gcc/run make DEBUG=1    -> with SCI0 debug output (MS1_DEBUG_SERIAL)
#   tools/hc12-gcc/run make RELEASE=4010 [images]  -> version 4010, in build/4010/
CC      = m68hc11-gcc
AS      = m68hc11-as
OBJCOPY = m68hc11-objcopy
OBJDUMP = m68hc11-objdump
NM      = m68hc11-nm
SIZE    = m68hc11-size
PYTHON  = python3

# The application version built (README: Release images): 4011 (releases 4011 and c011) or 4010
# (releases 4010 and c010), its code (MS1_RELEASE, MS1_4010), RAM layout (tools/releases.py),
# linker script part (gcc/<version>/release.ld), assembly modules and function table
RELEASE   ?= 4011

# What the build takes from the original (README: What you provide), per version
FIRMWARE  ?= ../original-firmware
ORIGINAL  ?= $(FIRMWARE)/firmware_20260317_192757_usbdm.s19
ifeq ($(RELEASE),4011)
B          = build
APP_IMAGE ?= $(FIRMWARE)/4011_1-8_mobile-station-2_A.bin
LISTING   ?= ../ms1-ghidra/out/listing.tsv
# releases with this application (the same code, their own data and article number)
RELEASES  ?= 4011_1-8_mobile-station-2_A c011_1-8_mobile-station-2_A
ASM_DIR    = asm
FUNCTIONS  = tools/functions.tsv
# build/ms1.s19 takes the original's parts from the device dump
MS1_DATA   =
else ifeq ($(RELEASE),4010)
B          = build/4010
APP_IMAGE ?= $(FIRMWARE)/4010_1-8_mobile-station_A.bin
# the modules' instructions in the 4010 image: ../ms1-ghidra/ghidra.sh listing (README)
LISTING   ?= ../ms1-ghidra/out/listing_4010.tsv
RELEASES  ?= 4010_1-8_mobile-station_A c010_1-8_mobile-station_A
ASM_DIR    = asm/4010
FUNCTIONS  = tools/functions_4010.tsv
ASFLAGS    = --defsym MS1_4010=1
# the device dump is a 4011 device's: build/4010/ms1.s19 takes the original's parts from the
# 4010 release image
MS1_DATA   = $(APP_IMAGE)
else
$(error RELEASE is 4011 or 4010)
endif

CFLAGS  = -MMD -MP -m68hcs12 -mshort -O1 -fomit-frame-pointer -ffunction-sections -fdata-sections -g -Igcc -Isrc/include -Isrc
CFLAGS += -DMS1_RELEASE=0x$(RELEASE)
ifdef DEBUG
CFLAGS += -DMS1_DEBUG_SERIAL
endif
comma := ,
LDFLAGS = -m68hcs12 -mshort -nostartfiles -T $(B)/ms1.ld -Wl,-Map,$(B)/ms1.map -Wl,--gc-sections -Wl,-Lgcc/$(RELEASE)
# functions the original has but nothing calls: kept, so that difftest -c can check them
UNCALLED = str_skip_ctrl lcd_text_layout_left mfx_frame_cmd20
LDFLAGS += $(addprefix -Wl$(comma)-u$(comma),$(UNCALLED))

SOURCES = \
	src/main.c \
	src/globals.c \
	src/lib/runtime.c \
	src/lib/str.c \
	src/hal/gpio_buttons.c \
	src/hal/serial_debug.c \
	src/sys/app_init.c \
	src/sys/sys_main.c \
	src/sys/eeprom_save.c \
	src/sys/ms_tick.c \
	src/sys/timer_init.c \
	src/can/mcan.c \
	src/can/mcan_status.c \
	src/can/mcan_lokstack.c \
	src/can/mcan_object.c \
	src/can/mcan_slave.c \
	src/can/mcan_name.c \
	src/signal/track_filler.c \
	src/signal/mm_encode.c \
	src/signal/lok_packets.c \
	src/signal/track_monitor.c \
	src/signal/mfx_encode.c \
	src/signal/mfx_register.c \
	src/signal/mfx_feedback.c \
	src/lok/lokstack.c \
	src/lok/lok_functions.c \
	src/ui/ui_helpers.c \
	src/ui/lcd_text.c \
	src/ui/ui_control.c \
	src/ui/ui_mode.c \
	src/ui/status_icons.c \
	src/ui/screen_text.c \
	src/ui/lok_icons.c \

# the original's hand-written assembly, generated from asm/*.json (tools/asmgen.py)
ASM_MODULES = siggen hal encoder

OBJS = $(patsubst src/%.c,$(B)/obj/%.o,$(SOURCES)) $(patsubst %,$(B)/obj/asm/%.o,$(ASM_MODULES)) \
	$(B)/obj/crt0.o $(B)/obj/vectors.o

all: $(B)/ms1.s19 $(B)/ms1.sym

# a change of flags here recompiles everything
$(OBJS): Makefile

$(B)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/obj/%.o: gcc/%.s
	@mkdir -p $(dir $@)
	$(AS) -m68hcs12 $(ASFLAGS) -o $@ $<

# every module's labels come from all the descriptions, the function table and the RAM variables
$(B)/asm/%.s: $(ASM_DIR)/%.json $(wildcard $(ASM_DIR)/*.json) tools/asmgen.py tools/releases.py $(FUNCTIONS) src/globals.c $(LISTING) $(APP_IMAGE)
	@mkdir -p $(dir $@)
	RELEASE=$(RELEASE) LISTING=$(LISTING) APP_IMAGE=$(APP_IMAGE) $(PYTHON) tools/asmgen.py gen $< $@

$(B)/obj/asm/%.o: $(B)/asm/%.s gcc/mc9s12d64.inc
	@mkdir -p $(dir $@)
	$(AS) -m68hcs12 -I. -o $@ $<

$(B)/ms1.ld: gcc/ms1.ld tools/ramlayout.py tools/releases.py $(OBJS)
	$(OBJDUMP) -h $(OBJS) | $(PYTHON) tools/ramlayout.py --release $(RELEASE) gcc/ms1.ld > $@.tmp
	mv $@.tmp $@

$(B)/ms1.elf: $(OBJS) $(B)/ms1.ld gcc/$(RELEASE)/release.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc
	$(SIZE) $@

$(B)/app.s19: $(B)/ms1.elf
	$(OBJCOPY) -O srec $< $@

# symbol table for ms1-emu's difftest
$(B)/ms1.sym: $(B)/ms1.elf
	$(NM) -n $< > $@

$(B)/ms1.s19: $(B)/app.s19 tools/mkimage.py tools/releases.py $(ORIGINAL) $(MS1_DATA)
	$(PYTHON) tools/mkimage.py --release $(RELEASE) $< $(ORIGINAL) $@ $(MS1_DATA)

# a release image: the application with the data of the release's decoded update file, the
# bootloader from ORIGINAL; the .bin and .bci (encoded with the original update file as
# template) are what a firmware update takes
images: $(foreach r,$(RELEASES),$(B)/$(r).s19 $(B)/$(r).bci)

$(B)/%.s19 $(B)/%.bin: $(B)/app.s19 tools/mkimage.py tools/releases.py $(ORIGINAL) $(FIRMWARE)/%.bin
	$(PYTHON) tools/mkimage.py --release $(RELEASE) $(B)/app.s19 $(ORIGINAL) $(B)/$*.s19 $(FIRMWARE)/$*.bin $(B)/$*.bin

$(B)/%.bci: $(B)/%.bin $(FIRMWARE)/%.bci ../tools/bci.py
	$(PYTHON) -I ../tools/bci.py encode $< $(FIRMWARE)/$*.bci $@ > /dev/null

clean:
	rm -rf build

.PHONY: all images clean

-include $(OBJS:.o=.d)
