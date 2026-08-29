# VoiceVector for Linux — C, GTK4/libadwaita, system libraries only.
#   make            build/voicevector (full app)
#   make core       build/voicevector-core (CLI: --self-test) — no GTK needed
#   make test       run the self-test
CC      ?= gcc
BUILD   ?= build
CFLAGS  ?= -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing
CFLAGS  += -Isrc -I$(BUILD)
VERSION ?= $(shell git -C ../.. describe --tags --match 'v*' 2>/dev/null | sed 's/^v//' || echo 0.0.0-dev)
CFLAGS  += -DVV_VERSION=\"$(VERSION)\"

CORE_PKGS := glib-2.0 gio-2.0 libcurl
UI_PKGS   := gtk4 libadwaita-1 libsecret-1 libpulse-simple libpulse

CORE_SRC := $(wildcard src/core/*.c)
PLAT_SRC := $(wildcard src/platform/*.c)
UI_SRC   := $(wildcard src/ui/*.c)

PROMPTS  := ../../shared/prompts/cleanup-rich.txt ../../shared/prompts/cleanup-light.txt ../../shared/prompts/review.txt

all: $(BUILD)/voicevector
core: $(BUILD)/voicevector-core

$(BUILD)/prompts.h: $(PROMPTS) scripts/embed-prompts.sh
	@mkdir -p $(BUILD)
	sh scripts/embed-prompts.sh $(PROMPTS) > $@

$(BUILD)/voicevector-core: $(BUILD)/prompts.h $(CORE_SRC) src/main_core.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(CORE_SRC) src/main_core.c $(shell pkg-config --cflags --libs $(CORE_PKGS)) -lm

$(BUILD)/voicevector: $(BUILD)/prompts.h $(CORE_SRC) $(PLAT_SRC) $(UI_SRC) src/main.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(CORE_SRC) $(PLAT_SRC) $(UI_SRC) src/main.c $(shell pkg-config --cflags --libs $(CORE_PKGS) $(UI_PKGS)) -lm
	@ls -la $@ | awk '{print "✓ " $$9 " (" $$5 " bytes)"}'

test: $(BUILD)/voicevector-core
	cd ../.. && apps/linux/$(BUILD)/voicevector-core --self-test

clean:
	rm -rf $(BUILD)

.PHONY: all core test clean
