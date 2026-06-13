#!/usr/bin/bash

clang --target=wasm32 \
      -O3 \
      -DTARGET_WEB \
      -nostdlib \
      -Wl,--no-entry \
      -Wl,--export-all \
      -o web_shifty.wasm web_shifty.c

