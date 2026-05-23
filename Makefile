
SHIFTY := build/shifty.co

ASMDIR := tools/asm8085
ASMNAME := asm8085.exe
ASM := $(ASMDIR)/$(ASMNAME)

.PHONY: all clean

all: run

.PHONY: send2emu
send2emu: build/shifty.co
	python tools/send2emu.py $(SHIFTY) --host localhost --port 9001

$(SHIFTY): build src/shifty.8085.asm src/tiles.8085.asm src/levels.8085.asm Makefile
	$(ASM) -c -o $(SHIFTY) src/shifty.8085.asm
	$(ASM) -o $(SHIFTY).bin src/shifty.8085.asm
	python tools/bin2bas.py $(SHIFTY).bin -o $(SHIFTY).bas

src/tiles.8085.asm: $(wildcard assets/tile_images/*.png) tools/png2asm.py Makefile
	python tools/png2asm.py assets/tile_images src/tiles.8085.asm

src/levels.8085.asm: assets/levels.txt src/tiles.8085.asm tools/levels2asm.py Makefile
	python tools/levels2asm.py assets/levels.txt src/tiles.8085.asm src/levels.8085.asm

build:
	mkdir -p build

$(ASM):
	$(MAKE) -C $(ASMDIR) ASM=$(ASMNAME)

clean:
	$(MAKE) -C $(ASMDIR) clean
	rm -f $(ASM)
	rm -rf build

.PHONY: run
run: $(SHIFTY)
	tools/Slappy/slappy.exe -run-co-file $(SHIFTY)