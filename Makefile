WASM_CC := clang
PYTHON  := python3

BUILD_DIR := build

SHIFTY       := $(BUILD_DIR)/shifty.co
WEB_WASM     := $(BUILD_DIR)/pc8201.wasm
WEB_JS       := $(BUILD_DIR)/shifty-co.js
WEB_HTML     := $(BUILD_DIR)/shifty-co.html
WEB_ASSETS   := $(WEB_WASM) $(WEB_JS) $(WEB_HTML) $(SHIFTY)

ASMDIR       := tools/asm8085
ASMNAME      := asm8085
ASM          := $(ASMDIR)/$(ASMNAME)

WASM_SOURCE  := web/pc8201.c
WASM_FLAGS   := -O3 -DTARGET_WEB -nostdlib
WASM_LDFLAGS := -Wl,--no-entry -Wl,--export-all

.PHONY: all clean asm

all: $(WEB_ASSETS)

$(BUILD_DIR):
	mkdir -p $@

$(WEB_WASM): $(WASM_SOURCE) | $(BUILD_DIR)
	$(WASM_CC) --target=wasm32 \
		$(WASM_FLAGS) \
		$(WASM_LDFLAGS) \
		-o $@ $<

$(WEB_JS): web/shifty-co.js | $(BUILD_DIR)
	cp $< $@

$(WEB_HTML): web/shifty-co.html | $(BUILD_DIR)
	cp $< $@

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
