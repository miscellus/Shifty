# Shifty

A small Sokoban game written in 8085 assembly language for the NEC PC-8201A (and Tandy TRS-80 Model 100, Olivetti M10, and the OG, the Kyocera KC-85).

## Overview
Shifty is a retro game project developed to explore homebrew development on the Kyotronic 85 family of portable computers. It is written purely in assembly and relies on custom tooling to handle asset conversion and build steps.

## Tools
- **The Assembler**: A fork of [marinuso/asm8085](https://github.com/marinuso/asm8085). I made a few modifications for this project:
  - Fixed a bug relating to binary output.
  - Improved error handling to report full file paths and line numbers (making it easier for code editors to jump to errors).
  - Added a new command-line option to automatically prepend the 6-byte KC-85 family `.CO` file header to the output binary.
- **The Emulator**: A fork of the awesome [VirtualT emulator](https://github.com/McNeight/VirtualT), tweaked for rapid development. I added a command-line option to boot right into the game. I have also (a bit prematurely perhaps) switched the platform abstraction from FLTK to SDL3, as I wish to compile the game for WebAssembly eventually.
- **Various Python Scripts**:
  - `bin2bas.py`: Converts the output binary to an N82-basic file to transfer to and bootstrap the NEC PC-8201A.
  - `levels2asm.py`: Converts the custom text-based level format in `assets/levels.txt` to `src/levels.8085.asm`.
  - `png2asm.py`: Converts the png tile images in `assets/tile_images` to `src/tiles.8085.asm`.

## Repository Structure
* `/src` - The raw 8085 assembly source code.
* `/tools` - Tools used for building the game and converting assets.
* `/assets` - Game graphics and data.

## Getting the Code
Because this project relies on custom forks of the assembler and emulator, they are included as git submodules. **Clone the repository with submodules enabled:**

```bash
git clone --recurse-submodules https://github.com/miscellus/Shifty.git
```

*If you already cloned it normally, you can pull the submodules by running `git submodule update --init --recursive` inside the project folder.*

## Building

You will need Python 3 installed for the asset build scripts. Because you cloned with submodules, my custom assembler will be used automatically.

Simply run:

```bash
make
```
