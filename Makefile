# QSO-Zero build
#   make                 native build (on the Cardputer Zero itself)
#   make CROSS=aarch64-linux-gnu-   cross build from a PC
#   make test            run DSP loopback tests on the build host

CROSS   ?=
CC      := $(CROSS)gcc
ARCH    := $(shell $(CC) -dumpmachine)

OPT     ?= -O2
ifneq (,$(findstring aarch64,$(ARCH)))
  # Cortex-A53 (RP3A0 / BCM2710A1)
  OPT   += -mcpu=cortex-a53 -ffast-math -ftree-vectorize
else ifneq (,$(findstring arm-linux-gnueabihf,$(ARCH)))
  # 32-bit Raspberry Pi OS on the A53: ARMv8 NEON in AArch32
  OPT   += -mcpu=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard -ffast-math -ftree-vectorize
else
  OPT   += -ffast-math
endif

CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
           -Wno-missing-field-initializers -Wno-format-truncation $(OPT) -Isrc -Ithird_party -MMD
LDLIBS  += -lm -lpthread -ldl

FT8_SRC := third_party/ft8/constants.c third_party/ft8/crc.c third_party/ft8/decode.c \
           third_party/ft8/encode.c third_party/ft8/ldpc.c third_party/ft8/message.c \
           third_party/ft8/text.c third_party/common/monitor.c
FFT_SRC := third_party/fft/kiss_fft.c third_party/fft/kiss_fftr.c
WSPRD_SRC := third_party/wsprd/wsprd.c third_party/wsprd/fano.c third_party/wsprd/nhash.c \
           third_party/wsprd/tab.c third_party/wsprd/wsprd_utils.c third_party/wsprd/wsprsim_utils.c
DSP_SRC := src/dsp/js8.c src/dsp/js8_varicode.c src/dsp/ft8w.c src/dsp/wspr.c src/dsp/wsprrx.c
CORE_SRC:= $(DSP_SRC) $(FT8_SRC) $(FFT_SRC) $(WSPRD_SRC)

APP_SRC := src/main.c src/core/app.c src/core/devices.c src/core/battery.c src/core/log.c src/core/bench.c src/core/config.c src/core/clock.c src/core/scheduler.c src/core/js8auto.c \
           src/rig/rigctl.c src/rig/radios.c src/net/uplink.c src/gps/gps.c src/audio/audio_alsa.c src/audio/mixer.c \
           src/ui/fb.c src/ui/gfx.c src/ui/font.c src/ui/input.c src/ui/theme.c src/ui/views.c

VERSION ?= 0.2.5
CFLAGS  += -DQSOZERO_VERSION=\"$(VERSION)\"
# 32-bit ARM: 64-bit inode numbers/offsets, or readdir() fails with EOVERFLOW
CFLAGS  += -D_FILE_OFFSET_BITS=64

BUILD   := build
CORE_OBJ:= $(CORE_SRC:%.c=$(BUILD)/%.o)
APP_OBJ := $(APP_SRC:%.c=$(BUILD)/%.o)

all: $(BUILD)/qso-zero

$(BUILD)/qso-zero: $(APP_OBJ) $(CORE_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c -o $@ $<

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

TESTS := $(BUILD)/test_js8 $(BUILD)/test_ft8 $(BUILD)/test_wspr $(BUILD)/test_wsprrx $(BUILD)/test_varicode $(BUILD)/test_engine $(BUILD)/test_uplink $(BUILD)/test_input $(BUILD)/test_qsy $(BUILD)/test_devices $(BUILD)/test_modes $(BUILD)/test_battery $(BUILD)/test_mixer $(BUILD)/test_log
ENGINE_OBJ := $(filter-out $(BUILD)/src/main.o $(BUILD)/src/ui/fb.o $(BUILD)/src/ui/input.o,$(APP_OBJ))
$(BUILD)/test_input: tests/test_input.c $(BUILD)/src/ui/input.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_uplink: tests/test_uplink.c $(BUILD)/src/net/uplink.o $(BUILD)/src/core/clock.o $(BUILD)/src/core/log.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_engine $(BUILD)/test_qsy $(BUILD)/test_modes $(BUILD)/test_battery: $(BUILD)/test_%: tests/test_%.c $(ENGINE_OBJ) $(CORE_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_log: tests/test_log.c $(BUILD)/src/core/log.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_mixer: tests/test_mixer.c $(BUILD)/src/audio/mixer.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_devices: tests/test_devices.c $(BUILD)/src/core/devices.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/test_%: tests/test_%.c $(CORE_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test: $(TESTS)
	@for t in $(TESTS); do echo "### $$t"; $$t || exit 1; done

$(BUILD)/preview: tools/preview.c $(filter-out $(BUILD)/src/main.o $(BUILD)/src/ui/fb.o $(BUILD)/src/ui/input.o,$(APP_OBJ)) $(CORE_OBJ)
	$(CC) $(CFLAGS) -DQSOZERO_PREVIEW -o $@ $^ $(LDLIBS)

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
.PHONY: all test clean

PREFIX ?= /usr/local
install: $(BUILD)/qso-zero
	install -D -m 755 $(BUILD)/qso-zero $(DESTDIR)$(PREFIX)/bin/qso-zero
	install -D -m 644 data/js8.jsc $(DESTDIR)$(PREFIX)/share/qso-zero/js8.jsc
	install -D -m 644 dist/qso-zero.conf.example $(DESTDIR)$(PREFIX)/share/qso-zero/qso-zero.conf.example

install-service: install
	install -D -m 644 dist/qso-zero@.service /etc/systemd/system/qso-zero@.service
	systemctl daemon-reload
	@echo "enable with: sudo systemctl enable --now qso-zero@$$USER"

deb: $(BUILD)/qso-zero
	VERSION=$(VERSION) packaging/build-deb.sh

.PHONY: install install-service deb
