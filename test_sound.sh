#!/usr/bin/env bash

set -e

tools/asm8085/asm8085 -c -o build/sndtes.co src/test_sound.8085.asm
tools/serisend.py -d 3 -b 1200 build/sndtes.co 
