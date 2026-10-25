# GNU build of the MS1 firmware: GCC 3.3.6 m68hc1x (the compiler family the original was built
# with), binutils 2.18. Run inside the toolchain container:
#   tools/hc12-gcc/run make            -> build/ms1.s19 (flashable, bootloader included)
#   tools/hc12-gcc/run make images     -> the release images: build/<release>.s19/.bin/.bci
#   tools/hc12-gcc/run make DEBUG=1    -> with SCI0 debug output (MS1_DEBUG_SERIAL)
CC      = m68hc11-gcc
AS      = m68hc11-as
OBJCOPY = m68hc11-objcopy
OBJDUMP = m68hc11-objdump
NM      = m68hc11-nm
SIZE    = m68hc11-size
PYTHON  = python3

# What the build takes from the original (README: What you provide)
FIRMWARE  ?= ../original-firmware
ORIGINAL  ?= $(FIRMWARE)/firmware_20260317_192757_usbdm.s19
APP_IMAGE ?= $(FIRMWARE)/4011_1-8_mobile-station-2_A.bin
LISTING   ?= ../ms1-ghidra/out/listing.tsv
# releases with this application (the same code, their own data and article number)
RELEASES  ?= 4011_1-8_mobile-station-2_A c011_1-8_mobile-station-2_A

CFLAGS  = -MMD -MP -m68hcs12 -mshort -O1 -fomit-frame-pointer -ffunction-sections -fdata-sections -g -Igcc -Isrc/include -Isrc
ifdef DEBUG
CFLAGS += -DMS1_DEBUG_SERIAL
endif
comma := ,
LDFLAGS = -m68hcs12 -mshort -nostartfiles -T build/ms1.ld -Wl,-Map,build/ms1.map -Wl,--gc-sections
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

OBJS = $(patsubst src/%.c,build/obj/%.o,$(SOURCES)) $(patsubst %,build/obj/asm/%.o,$(ASM_MODULES)) \
	build/obj/crt0.o build/obj/vectors.o

all: build/ms1.s19 build/ms1.sym

# a change of flags here recompiles everything
$(OBJS): Makefile

build/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/obj/%.o: gcc/%.s
	@mkdir -p $(dir $@)
	$(AS) -m68hcs12 -o $@ $<

# every module's labels come from all the descriptions, functions.tsv and the RAM variables
build/asm/%.s: asm/%.json $(wildcard asm/*.json) tools/asmgen.py tools/functions.tsv src/globals.c $(LISTING) $(APP_IMAGE)
	@mkdir -p $(dir $@)
	LISTING=$(LISTING) APP_IMAGE=$(APP_IMAGE) $(PYTHON) tools/asmgen.py gen $< $@

build/obj/asm/%.o: build/asm/%.s gcc/mc9s12d64.inc
	@mkdir -p $(dir $@)
	$(AS) -m68hcs12 -I. -o $@ $<

build/ms1.ld: gcc/ms1.ld tools/ramlayout.py $(OBJS)
	$(OBJDUMP) -h $(OBJS) | $(PYTHON) tools/ramlayout.py gcc/ms1.ld > $@.tmp
	mv $@.tmp $@

build/ms1.elf: $(OBJS) build/ms1.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc
	$(SIZE) $@

build/app.s19: build/ms1.elf
	$(OBJCOPY) -O srec $< $@

# symbol table for ms1-emu's difftest
build/ms1.sym: build/ms1.elf
	$(NM) -n $< > $@

build/ms1.s19: build/app.s19 tools/mkimage.py $(ORIGINAL)
	$(PYTHON) tools/mkimage.py $< $(ORIGINAL) $@

# a release image: the application with the data of the release's decoded update file, the
# bootloader from ORIGINAL; the .bin and .bci (encoded with the original update file as
# template) are what a firmware update takes
images: $(foreach r,$(RELEASES),build/$(r).s19 build/$(r).bci)

build/%.s19 build/%.bin: build/app.s19 tools/mkimage.py $(ORIGINAL) $(FIRMWARE)/%.bin
	$(PYTHON) tools/mkimage.py build/app.s19 $(ORIGINAL) build/$*.s19 $(FIRMWARE)/$*.bin build/$*.bin

build/%.bci: build/%.bin $(FIRMWARE)/%.bci ../tools/bci.py
	$(PYTHON) -I ../tools/bci.py encode $< $(FIRMWARE)/$*.bci $@ > /dev/null

clean:
	rm -rf build

.PHONY: all images clean

-include $(OBJS:.o=.d)
