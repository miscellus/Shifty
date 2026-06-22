
SHIFTY := build/shifty.co

WEB_SHIFTY_WASM := build/web/web_shifty.wasm

ASMDIR := tools/asm8085
ASMNAME := asm8085.exe
ASM := $(ASMDIR)/$(ASMNAME)

.PHONY: all clean

all: build/web

build:
	mkdir -p build

build/web: build web/web_shifty.wasm web/web_shifty.js web/web_shifty.html $(SHIFTY)
	mkdir -p build/web
	cp web/web_shifty.wasm web/web_shifty.js web/web_shifty.html web/debug.json web/co_file.inc build/web

$(SHIFTY): build src/shifty.8085.asm src/tiles.8085.asm src/levels.8085.asm Makefile $(ASM)
	$(ASM) -c -o $(SHIFTY) -d web/debug.json src/shifty.8085.asm
	python tools/co2bas.py $(SHIFTY) -o $(SHIFTY).bas
	python tools/bin2cints.py $(SHIFTY) -o web/co_file.inc

web/web_shifty.wasm: web/web_shifty.c $(SHIFTY)
	clang --target=wasm32 \
      -O3 \
      -DTARGET_WEB \
      -nostdlib \
      -Wl,--no-entry \
      -Wl,--export-all \
      -o web/web_shifty.wasm web/web_shifty.c

src/tiles.8085.asm: $(wildcard assets/tile_images/*.png) tools/png2asm.py Makefile
	python tools/png2asm.py assets/tile_images src/tiles.8085.asm

src/levels.8085.asm: assets/levels.txt src/tiles.8085.asm tools/levels2asm.py Makefile
	python tools/levels2asm.py assets/levels.txt src/tiles.8085.asm src/levels.8085.asm

$(ASM):
	$(MAKE) -C $(ASMDIR) ASM=$(ASMNAME) asm8085

clean:
	$(MAKE) -C $(ASMDIR) clean
	rm -f $(ASM)
	rm -rf build

.PHONY: run
run: $(SHIFTY)
	tools/Slappy/slappy.exe -run-co-file $(SHIFTY)