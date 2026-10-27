# GNU build of the MS1 firmware: GCC 3.3.6 m68hc1x (the compiler family the original was built
# with), binutils 2.18. Run inside the toolchain container:
#   tools/hc12-gcc/run make            -> build/app.s19, the release images build/<release>.bin/.bci,
#                                         and with a device dump (ORIGINAL) the flashable
#                                         build/<release>.s19 and build/ms1.s19
#   tools/hc12-gcc/run make DEBUG=1    -> with SCI0 debug output (MS1_DEBUG_SERIAL)
#   tools/hc12-gcc/run make RELEASE=4010  -> version 4010, in build/4010/
CC      = m68hc11-gcc
OBJCOPY = m68hc11-objcopy
OBJDUMP = m68hc11-objdump
NM      = m68hc11-nm
SIZE    = m68hc11-size
PYTHON  = python3

# The application version built (README: Release images): 4011 (releases 4011 and c011) or 4010
# (releases 4010 and c010), its code (MS1_RELEASE), RAM layout (tools/releases.py),
# read-only data and function table
RELEASE   ?= 4011

# From outside this repository (README: What you provide), not needed to build: a device dump
# for the flashable images (its bootloader)
FIRMWARE  ?= ../original-firmware
ORIGINAL  ?= $(FIRMWARE)/firmware_20260317_192757_usbdm.s19
ifeq ($(RELEASE),4011)
B          = build
# the releases with this application (the same code, their own data and article number)
RELEASES   = 4011_1-8_mobile-station-2_A c011_1-8_mobile-station-2_A
else ifeq ($(RELEASE),4010)
B          = build/4010
RELEASES   = 4010_1-8_mobile-station_A c010_1-8_mobile-station_A
else
$(error RELEASE is 4011 or 4010)
endif

CFLAGS  = -MMD -MP -m68hcs12 -mshort -O1 -fomit-frame-pointer -ffunction-sections -fdata-sections -g -Igcc -Isrc/include -Isrc
CFLAGS += -DMS1_RELEASE=0x$(RELEASE)
# the register names for the assembly in C sources (src/include/asm.h)
CFLAGS += -Wa,-I$(B)
ifdef DEBUG
CFLAGS += -DMS1_DEBUG_SERIAL
endif
comma := ,
LDFLAGS = -m68hcs12 -mshort -nostartfiles -T $(B)/ms1.ld -Wl,-Map,$(B)/ms1.map -Wl,--gc-sections -Wl,-L$(B)
# functions the original has but nothing calls: kept, so that difftest -c can check them
UNCALLED = str_skip_ctrl lcd_text_layout_left mfx_frame_cmd20 track_cmd_write_type2 \
	delay_long can_poll
LDFLAGS += $(addprefix -Wl$(comma)-u$(comma),$(UNCALLED))

SOURCES = \
	src/main.c \
	src/globals.c \
	src/lib/runtime.c \
	src/lib/str.c \
	src/hal/gpio_buttons.c \
	src/hal/serial_debug.c \
	src/hal/eeprom.c \
	src/sys/app_init.c \
	src/sys/sys_main.c \
	src/sys/eeprom_save.c \
	src/sys/ms_tick.c \
	src/sys/timer_init.c \
	src/sys/delay.c \
	src/sys/crt0.c \
	src/sys/vectors.c \
	src/can/can0.c \
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
	src/signal/signal_gen.c \
	src/signal/feedback_capture.c \
	src/lok/lokstack.c \
	src/lok/lok_functions.c \
	src/ui/lang.c \
	src/ui/lcd_tables.c \
	src/ui/lcd.c \
	src/ui/encoder.c \
	src/ui/ui_helpers.c \
	src/ui/lcd_text.c \
	src/ui/ui_control.c \
	src/ui/ui_mode.c \
	src/ui/status_icons.c \
	src/ui/screen_text.c \
	src/ui/lok_icons.c \

OBJS = $(patsubst src/%.c,$(B)/obj/%.o,$(SOURCES))

# flash page $3C (include/page3c.h, gcc/page3c.ld): its entry table and the code the
# application calls there; linked after the application, against its symbols
PAGE3C_SOURCES = \
	src/sys/page3c_entries.c \

PAGE3C_OBJS = $(patsubst src/%.c,$(B)/obj/%.o,$(PAGE3C_SOURCES))

# the flashable images, with a device dump's bootloader
ifneq ($(wildcard $(ORIGINAL)),)
FLASHABLE = $(foreach r,$(RELEASES),$(B)/$(r).s19) $(B)/ms1.s19
endif

all: $(B)/app.s19 $(B)/ms1.sym images

# the release images: build/<release>.bin is what a firmware update takes, .bci its update file
images: $(foreach r,$(RELEASES),$(B)/$(r).bin $(B)/$(r).bci) $(FLASHABLE)

# a change of flags here recompiles everything
$(OBJS) $(PAGE3C_OBJS): Makefile

$(B)/obj/%.o: src/%.c | $(B)/mc9s12d64.inc
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# the register names of gcc/mc9s12d64.h as assembler symbols (src/include/asm.h)
$(B)/mc9s12d64.inc: gcc/mc9s12d64.h
	@mkdir -p $(dir $@)
	sed -n 's/^#define \([A-Z0-9_]*\) *_REG\(8\|16\)(\(0x[0-9A-F]*\)).*/\t.equ\t\1,\3/p' $< > $@

# page $3C, linked against the application's symbols (its RAM variables, the fixed-flash
# functions it calls), but for the entry-table names the page defines itself
$(B)/app_symbols.elf: $(B)/ms1.elf $(B)/page3c_symbols.ld
	sed -n 's/^\([A-Za-z_][A-Za-z0-9_]*\) = .*/\1/p' $(B)/page3c_symbols.ld > $@.names
	if [ -s $@.names ]; then $(OBJCOPY) --strip-symbols=$@.names $< $@; else cp $< $@; fi
	rm $@.names

$(B)/page3c.elf: $(PAGE3C_OBJS) gcc/page3c.ld $(B)/app_symbols.elf
	$(CC) -m68hcs12 -mshort -nostdlib -T gcc/page3c.ld -Wl,--just-symbols=$(B)/app_symbols.elf \
		-Wl,-Map,$(B)/page3c.map -o $@ $(PAGE3C_OBJS)

$(B)/page3c.bin: $(B)/page3c.elf
	$(OBJCOPY) -O binary -j .page3c --gap-fill 0xff --pad-to 0x104000 $< $@

# the application's addresses of the page-$3C entry table
$(B)/page3c_symbols.ld: $(B)/obj/sys/page3c_entries.o tools/banks.py
	$(PYTHON) tools/banks.py entries $(OBJDUMP) $< > $@.tmp
	mv $@.tmp $@

# the identification area of each release (src/sys/ident.c; c0... the 60651's), linked on its own
$(B)/ident/%.o: src/sys/ident.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DMS1_ARTICLE=$(if $(filter c%,$*),60651,60652) -c $< -o $@

$(B)/ident/%.elf: $(B)/ident/%.o gcc/ident.ld
	$(CC) -m68hcs12 -mshort -nostdlib -T gcc/ident.ld -o $@ $<

$(B)/ident/%.s19: $(B)/ident/%.elf
	$(OBJCOPY) -O srec -j .ident_strings -j .ident $< $@

# the RAM placements (tools/ramlayout.py), then the code that must stay in fixed flash
# (tools/banks.py)
$(B)/ms1.ld: gcc/ms1.ld tools/ramlayout.py tools/releases.py tools/banks.py $(OBJS) $(PAGE3C_OBJS)
	$(OBJDUMP) -h $(OBJS) | $(PYTHON) tools/ramlayout.py --release $(RELEASE) gcc/ms1.ld > $@.ram
	$(PYTHON) tools/banks.py place $(OBJDUMP) $@.ram --page3c $(PAGE3C_OBJS) --app $(OBJS) > $@.tmp
	rm $@.ram
	mv $@.tmp $@

$(B)/ms1.elf: $(OBJS) $(B)/ms1.ld $(B)/page3c_symbols.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc
	$(SIZE) $@

$(B)/app.s19: $(B)/ms1.elf
	$(OBJCOPY) -O srec $< $@

# symbol table for ms1-emu's difftest
$(B)/ms1.sym: $(B)/ms1.elf
	$(NM) -n $< > $@

PARTS = $(B)/app.s19 $(B)/page3c.bin tools/mkimage.py tools/releases.py

$(B)/%.bin: $(PARTS) $(B)/ident/%.s19
	$(PYTHON) tools/mkimage.py --release $(RELEASE) --bin $@ $(B)/app.s19 $(B)/page3c.bin $(B)/ident/$*.s19

$(B)/%.s19: $(PARTS) $(B)/ident/%.s19 $(ORIGINAL)
	$(PYTHON) tools/mkimage.py --release $(RELEASE) --bootloader $(ORIGINAL) --s19 $@ $(B)/app.s19 $(B)/page3c.bin $(B)/ident/$*.s19

# the version's first release, flashable, for the emulator
$(B)/ms1.s19: $(B)/$(firstword $(RELEASES)).s19
	cp $< $@

# with the version's loader, which the update runs from RAM
$(B)/%.bci: $(B)/%.bin third_party/loader/$(RELEASE).s19 ../tools/bci.py
	$(PYTHON) -I ../tools/bci.py encode $< third_party/loader/$(RELEASE).s19 $@ > /dev/null

clean:
	rm -rf build

# keep what the pattern rules make on the way (the identification parts)
.SECONDARY:

.PHONY: all images clean

-include $(patsubst %.o,%.d,$(OBJS) $(PAGE3C_OBJS))
