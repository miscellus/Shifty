
SHIFTY := build/shifty.co

WEB_SHIFTY_WASM := build/web/pc8201.wasm

ASMDIR := tools/asm8085
ASMNAME := asm8085
ASM := $(ASMDIR)/$(ASMNAME)

.PHONY: all clean

# all: tools/serild.co

all: build/web $(SHIFTY)

build:
	mkdir -p build

build/web: build web/pc8201.wasm web/shifty-co.js web/shifty-co.html
	mkdir -p build/web
	cp  web/pc8201.wasm \
		web/shifty-co.js \
		web/shifty-co.html \
		build/web

$(SHIFTY): build/web src/shifty.8085.asm src/tiles.8085.asm src/levels.8085.asm src/splash.8085.asm Makefile $(ASM)
	$(ASM) -c -o $(SHIFTY) -d build/web/debug.json src/shifty.8085.asm
	cp $(SHIFTY) build/web

web/pc8201.wasm: web/pc8201.c
	clang --target=wasm32 \
      -O3 \
      -DTARGET_WEB \
      -nostdlib \
      -Wl,--no-entry \
      -Wl,--export-all \
      -o web/pc8201.wasm web/pc8201.c

src/tiles.8085.asm: $(wildcard assets/tile_images/*.png) tools/png2asm.py Makefile
	python tools/png2asm.py assets/tile_images src/tiles.8085.asm

src/levels.8085.asm: assets/levels.txt src/tiles.8085.asm tools/levels2asm.py Makefile
	python tools/levels2asm.py assets/levels.txt src/tiles.8085.asm src/levels.8085.asm

src/splash.8085.asm: tools/splash2asm.py assets/title_screen_240x64.png Makefile
	python tools/splash2asm.py assets/title_screen_240x64.png src/splash.8085.asm

tools/serild.co: tools/serild.8085.asm
	$(ASM) -c -o tools/serild.co tools/serild.8085.asm

$(ASM):
	$(MAKE) -C $(ASMDIR) ASM=$(ASMNAME) asm8085

clean:
	$(MAKE) -C $(ASMDIR) clean
	rm -f $(ASM)
	rm -rf build

.PHONY: run
run: $(SHIFTY)
	tools/Slappy/slappy.exe -run-co-file $(SHIFTY)