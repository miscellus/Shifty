
SHIFTY := build/shifty.co

ASMDIR := tools/asm8085
ASMNAME := asm8085.exe
ASM := $(ASMDIR)/$(ASMNAME)

.PHONY: all clean

all: run

.PHONY: send2emu
send2emu: build/shifty.co
	python tools/send2emu.py $(SHIFTY) --host localhost --port 9001

$(SHIFTY): $(ASM) build src/shifty.s8085 src/tiles.s8085 src/levels.s8085 Makefile
	$(ASM) -c -o $(SHIFTY) src/shifty.s8085
	$(ASM) -o $(SHIFTY).bin src/shifty.s8085
	python tools/bin2bas.py $(SHIFTY).bin -o $(SHIFTY).bas

src/tiles.s8085: $(wildcard assets/tile_images/*.png) tools/png2asm.py
	python tools/png2asm.py assets/tile_images src/tiles.s8085

src/levels.s8085: assets/levels.txt src/tiles.s8085 tools/levels2asm4.py Makefile
	python tools/levels2asm4.py assets/levels.txt src/tiles.s8085 > src/levels.s8085

build:
	mkdir build

$(ASM):
	$(MAKE) -C $(ASMDIR) ASM=$(ASMNAME)

clean:
	$(MAKE) -C $(ASMDIR) clean
	rm -f $(ASM)
	rm -rf build

.PHONY: run
run: $(SHIFTY)
	tools/Slappy/slappy.exe -run-co-file $(SHIFTY)