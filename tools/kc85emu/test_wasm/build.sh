#!/usr/bin/bash

clang --target=wasm32 \
      -O3 \
      -nostdlib \
      -Wl,--no-entry \
      -Wl,--export-all \
      -o mvp_8085_wasm.wasm mvp_8085_wasm.c