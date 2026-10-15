WASM_CC := clang
EMULATOR_CC ?= gcc
PYTHON  := python3

BUILD_DIR := build

SHIFTY       := $(BUILD_DIR)/shifty.co
WEB_WASM     := $(BUILD_DIR)/pc8201.wasm
WEB_JS       := $(BUILD_DIR)/shifty-co.js
WEB_HTML     := $(BUILD_DIR)/shifty-co.html
WEB_ASSETS   := $(WEB_WASM) $(WEB_JS) $(WEB_HTML) $(SHIFTY)

EMULATOR_BIN := $(BUILD_DIR)/emulator

ASMDIR       := tools/asm8085
ASMNAME      := asm8085
ASM          := $(ASMDIR)/$(ASMNAME)

WASM_SOURCE  := emulator/pc8201.c
WASM_FLAGS   := -O3 -DTARGET_WEB -nostdlib
WASM_LDFLAGS := -Wl,--no-entry -Wl,--export-all

# RGFW Native Linker Flags Setup
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
	PLATFORM = LINUX
endif
ifeq ($(UNAME_S),Darwin)
	PLATFORM = MACOS
endif
ifneq (,$(findstring MINGW,$(UNAME_S)))
	PLATFORM = WIN32
endif

BUILD_TYPE ?= DEBUG

EMULATOR_CFLAGS_LINUX_DEBUG := -O0 -g
EMULATOR_CFLAGS_LINUX_RELEASE := -O3

EMULATOR_CFLAGS_MACOS_DEBUG := # TODO
EMULATOR_CFLAGS_MACOS_RELEASE := # TODO

EMULATOR_CFLAGS_WIN32_DEBUG := # TODO
EMULATOR_CFLAGS_WIN32_RELEASE := # TODO

EMULATOR_CFLAGS := -Wall -Wextra
EMULATOR_CFLAGS += ${EMULATOR_CFLAGS_${PLATFORM}_${BUILD_TYPE}}

EMULATOR_LDFLAGS_LINUX := -lSDL3 -lOpenGL -lm
EMULATOR_LDFLAGS_MACOS := # TODO
EMULATOR_LDFLAGS_WIN32 := # TODO
EMULATOR_LDFLAGS := ${EMULATOR_LDFLAGS_${PLATFORM}}

.PHONY: all clean asm serve emulator

all: $(WEB_ASSETS) emulator build/sndtes.co

build/sndtes.co: src/test_sound.8085.asm | $(BUILD_DIR)
	$(ASM) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $@

# --- Web Assembly Target ---
$(WEB_WASM): $(WASM_SOURCE) | $(BUILD_DIR)
	$(WASM_CC) --target=wasm32 \
		$(WASM_FLAGS) \
		$(WASM_LDFLAGS) \
		-o $@ $<

$(WEB_JS): emulator/shifty-co.js | $(BUILD_DIR)
	cp $< $@

$(WEB_HTML): emulator/shifty-co.html | $(BUILD_DIR)
	cp $< $@

emulator: $(EMULATOR_BIN)

$(EMULATOR_BIN): $(WASM_SOURCE) | $(BUILD_DIR)
	$(EMULATOR_CC) -DTARGET_DESKTOP $(EMULATOR_CFLAGS) -o $@ $< $(EMULATOR_LDFLAGS)


$(SHIFTY): \
		src/shifty.8085.asm \
		src/tiles.8085.asm \
		src/levels.8085.asm \
		src/splash.8085.asm \
		$(ASM) \
		| $(BUILD_DIR)
	$(ASM) -c -o $@ -d $(BUILD_DIR)/debug.json $<

# Generated assembly sources
src/tiles.8085.asm: \
		$(wildcard assets/tile_images/*.png) \
		tools/png2asm.py \
		#
	$(PYTHON) tools/png2asm.py assets/tile_images $@

src/levels.8085.asm: \
		assets/levels.txt \
		src/tiles.8085.asm \
		tools/levels2asm.py \
		#
	$(PYTHON) tools/levels2asm.py \
		assets/levels.txt \
		src/tiles.8085.asm \
		$@

src/splash.8085.asm: \
		tools/splash2asm.py \
		assets/title_screen_240x64.png \
		#
	$(PYTHON) tools/splash2asm.py \
		assets/title_screen_240x64.png \
		$@

$(ASM):
	$(MAKE) -C $(ASMDIR) ASM=$(ASMNAME) $(ASMNAME)

tools/serild.co: tools/serild.8085.asm asm
	$(ASM) -c -o $@ $<

clean:
	$(MAKE) -C $(ASMDIR) clean
	rm -rf $(BUILD_DIR)
	rm -f $(ASM)

serve: $(WEB_ASSETS)
	cd $(BUILD_DIR) && python -m http.server