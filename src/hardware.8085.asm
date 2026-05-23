; ==================================================================
; HARDWARE.INC
; Universal Hardware Definitions for Kyotronic 85 Sister Machines
; (NEC PC-8201A, Tandy TRS-80 Model 100, Olivetti M10, KC-85)
; ==================================================================

; ------------------------------------------------------------------
; LCD PORTS (Universal across all models)
; ------------------------------------------------------------------
PortLcdCmd   equ   0xFE        ; LCD Command/Status Port
PortLcdStat  equ   PortLcdCmd  ; LCD Command/Status Port
PortLcdData  equ   0xFF        ; LCD Data I/O Port

; ------------------------------------------------------------------
; TARGET: NEC PC-8201A
; ------------------------------------------------------------------
             ifdef TargetNec

Port81C55Cmd equ   0xB8        ; 81C55 Command / Status Port
Port81C55A   equ   0xB9        ; 81C55 Port A (LCD 1-8 / Key Strobe)
Port81C55B   equ   0xBA        ; 81C55 Port B (LCD 9-10 / Bell / Power)
PortKeyIn    equ   0xE8        ; Keyboard Data IN Port

HookTimer    equ   0xF38F      ; RST 7.5 Timer Hook Address
MapRamBase   equ   0xA000      ; Safe execution / map RAM origin

             endif

; ------------------------------------------------------------------
; TARGET: Tandy TRS-80 Model 100
; ------------------------------------------------------------------
             ifdef TargetT100

Port81C55Cmd equ   0xB0        ; 81C55 Command / Status Port
Port81C55A   equ   0xB1        ; 81C55 Port A (LCD 1-8 / Key Strobe)
Port81C55B   equ   0xB2        ; 81C55 Port B (LCD 9-10 / Bell / Power)
PortKeyIn    equ   0xE0        ; Keyboard Data IN Port

HookTimer    equ   0xFCBB      ; RST 7.5 Timer Hook Address
MapRamBase   equ   0xA000      ; Safe execution / map RAM origin

             endif

; ------------------------------------------------------------------
; TARGET: Olivetti M10
; ------------------------------------------------------------------
             ifdef TargetM10

; Uses same I/O offsets as T100/KC85
Port81C55Cmd equ   0xBA
Port81C55A   equ   0xBB
Port81C55B   equ   0xBC
PortKeyIn    equ   0xE8

; NOTE: Hook address may vary slightly from T100 based on ROM revision
HookTimer    equ   0xFCBB      ; Pending exact M10 ver                  if ication
MapRamBase   equ   0xA000

             endif

; ------------------------------------------------------------------
; TARGET: Kyocera Kyotronic 85 (KC-85)
; ------------------------------------------------------------------
             ifdef TargetKc85

; Uses original Kyocera reference I/O offsets
Port81C55Cmd equ   0xBA
Port81C55A   equ   0xBB
Port81C55B   equ   0xBC
PortKeyIn    equ   0xE8

; NOTE: Hook address may vary slightly based on reference ROM
HookTimer    equ   0xFCBB      ; Pending exact KC85 ver                 if ication
MapRamBase   equ   0xA000

             endif