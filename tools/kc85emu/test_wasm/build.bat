@echo off
cl.exe -DTARGET_DEBUG -Zi -EHsc -Od mvp_8085_wasm.c -link -DEBUG -OUT:mvp_8085_debug.exe
