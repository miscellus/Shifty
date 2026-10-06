TargetNec equ 1
	include "hardware.8085.asm"

	org 50000

;	call Buzzer_PlayChirpUp
;
;	lxi b, 10000       ; Delay speed
;	call Delay
;
;	call Buzzer_PlayFanfare
;
;	lxi b, 10000       ; Delay speed
;	call Delay
;
;	call Buzzer_PlayChirpDown
;
;	lxi b, 10000       ; Delay speed
;	call Delay
;
;	call Buzzer_PlayChirpDown
;
;	lxi b, 10000
;	call Delay
;	
;	lxi h, Melody_ZeldaChest
;	call Buzzer_PlayMelody

	lxi h, Melody
	call Buzzer_PlayPacked

	ret

; ======================================================
; FULL CHROMATIC NOTE DEFINITIONS (Indices 00 - 31)
; ======================================================
RST  equ 0      ; Rest / Silence

; Octave 3
a3   equ 1
A3   equ 2
b3   equ 3

; Octave 4
c4   equ 4
C4   equ 5
d4   equ 6
D4   equ 7
e4   equ 8
f4   equ 9
F4   equ 10
g4   equ 11
G4   equ 12
a4   equ 13
A4   equ 14
b4   equ 15

; Octave 5
c5   equ 16
C5   equ 17
d5   equ 18
D5   equ 19
e5   equ 20
f5   equ 21
F5   equ 22
g5   equ 23
G5   equ 24
a5   equ 25
A5   equ 26
b5   equ 27

; Octave 6
c6   equ 28
C6   equ 29
d6   equ 30
D6   equ 31

t1 equ 1 << 5
t2 equ 2 << 5
t3 equ 3 << 5
t4 equ 4 << 5
t5 equ 5 << 5
t6 equ 6 << 5
t7 equ 7 << 5

Melody:
	; --- Part 1 ---
	db t2|e5, t1|b4, t1|c5, t2|d5, t1|c5, t1|b4  ; Bar 1 (8 ticks)
	db t2|a4, t1|a4, t1|c5, t2|e5, t1|d5, t1|c5  ; Bar 2 (8 ticks)
	db t3|b4, t1|c5, t2|d5, t2|e5               ; Bar 3 (8 ticks: B4 is dotted quarter)
	db t2|c5, t2|a4, t4|a4                      ; Bar 4 (8 ticks)

	; --- Part 2 ---
	db t3|d5, t1|f5, t2|a5, t1|g5, t1|f5        ; Bar 5 (8 ticks: D5 is dotted quarter)
	db t3|e5, t1|c5, t2|e5, t1|d5, t1|c5        ; Bar 6 (8 ticks: E5 is dotted quarter)
	db t3|b4, t1|c5, t2|d5, t2|e5               ; Bar 7 (8 ticks)
	db t2|c5, t2|a4, t4|a4                      ; Bar 8 (8 ticks)

	db 0                                        ; End Marker


; ======================================================
; EXAMPLES OF HOW TO CALL THE PARAMETERIZED ROUTINE
; ======================================================
Buzzer_PlayChirpUp:
	lxi d, 7000         ; Start pitch
	lxi b, -100         ; Pitch delta (Negative = Pitch goes UP)
	mvi l, 30           ; Number of steps
	call Buzzer_Sweep
	ret

Buzzer_PlayChirpDown:
	lxi d, 3000         ; Start pitch
	lxi b, 100          ; Pitch delta (Positive = Pitch goes DOWN)
	mvi l, 30           ; Number of steps
	call Buzzer_Sweep
	ret

; ======================================================
; UNIFIED SWEEP ROUTINE
; [DE] = Start frequency divisor
; [BC] = Frequency delta per step
; [L]  = Number of steps
; ======================================================
Buzzer_Sweep:
	di
	call Buzzer_SetFreq
	call Buzzer_On

.sweepLoop:
	push h              ; Save loop counter (L)
	push d              ; Save current pitch
	push b              ; Save pitch delta

	lxi b, 0x0115       ; Delay speed
	call Delay

	pop b               ; Restore pitch delta (BC)
	pop h               ; POP H restores the saved D/E values into H/L!
	
	dad b               ; HL = HL + BC (Applies the positive or negative delta)
	xchg                ; Swap HL back into DE for the frequency update
	
	call Buzzer_SetFreq
	
	pop h               ; Restore original loop counter (L)
	dcr l
	jnz .sweepLoop

	call Buzzer_Off
	ei
	ret

Buzzer_PlayPacked:
	di
	; (No need to turn the buzzer on here anymore)

.readPacked:
	mov a, m            ; Read the packed byte
	inx h
	ora a               ; Is it exactly 0x00? (End marker)
	jz .endMelody
	
	push h              ; Save melody pointer
	mov c, a            ; Save a copy of the packed byte in C

	; --- 1. Decode Frequency or Rest ---
	ani 0x1f            ; Mask out duration, leave 000FFFFF
	jz .playRest        ; *** NEW: If index is 0, it's a Rest! ***
	
	; Normal note setup
	add a
	mov e, a
	mvi d, 0
	lxi h, Freq_LUT
	dad d               ; HL points to the frequency in the LUT
	
	mov a, m
	inx h
	mov d, m
	mov e, a
	call Buzzer_SetFreq 
	call Buzzer_On      ; Ensure the buzzer is ON (in case previous was a rest)
	jmp .decodeDuration

.playRest:
	call Buzzer_Off     ; Mute the speaker for this step

.decodeDuration:
	; --- 2. Decode Duration ---
	mov a, c
	rlc
	rlc
	rlc
	ani 0x07
	mov d, a            ; D = Duration multiplier
	
.durationLoop:
	push d
	lxi b, 0x0A1D       ; Base tick speed
	call Delay
	pop d
	dcr d
	jnz .durationLoop

	; --- 3. Loop to next note ---
	pop h               ; Restore melody pointer
	jmp .readPacked

.endMelody:
	call Buzzer_Off
	ei
	ret

Delay:
; [BC] = Delay
	mov a,c
.outerLoop:
	push b
	mvi c, 72
.innerLoop:
	dcr c
	jnz .innerLoop
	pop b
	dcr a
	jnz .outerLoop
	dcr b
	jnz Delay
	ret

Buzzer_On:
; clobbers [A]
	mvi a, 0xc3
	out Port81C55Cmd
	in Port81C55B
	ani 0xf8
	ori 0x20
	out Port81C55B
	ret

Buzzer_SetFreq:
; [DE] = frequency
; clobbers [A]
	mov a, e
	out Port81C55TimerLo
	mov a, d
	ori 1 << 6
	out Port81C55TimerHi
	mvi a, 0b11000011
	out Port81C55Cmd
	ret

Buzzer_Off:
; clobbers [A]
	in Port81C55B
	ori 1 << 2
	out Port81C55B
	ret


; ======================================================
; 32-Note A Minor Scale (A, B, C, D, E, F, G)
; Range: A2 (110 Hz) to D7 (2349 Hz)
; Formula: 1228800 / Frequency
; ======================================================
Freq_LUT:
	dw 0        ; 00: REST (Silence)

	; --- Octave 3 ---
	dw 5585*2     ; 01: A3  (220.0 Hz)
	dw 5272*2     ; 02: A#3 (233.1 Hz)
	dw 4976*2     ; 03: B3  (246.9 Hz)

	; --- Octave 4 ---
	dw 4697*2     ; 04: C4  (261.6 Hz)
	dw 4433*2     ; 05: C#4 (277.2 Hz)
	dw 4184*2     ; 06: D4  (293.7 Hz)
	dw 3949*2     ; 07: D#4 (311.1 Hz)
	dw 3728*2     ; 08: E4  (329.6 Hz)
	dw 3519*2     ; 09: F4  (349.2 Hz)
	dw 3321*2     ; 10: F#4 (370.0 Hz)
	dw 3135*2     ; 11: G4  (392.0 Hz)
	dw 2959*2     ; 12: G#4 (415.3 Hz)
	dw 2793*2     ; 13: A4  (440.0 Hz)
	dw 2636*2     ; 14: A#4 (466.2 Hz)
	dw 2488*2     ; 15: B4  (493.9 Hz)

	; --- Octave 5 ---
	dw 2348*2     ; 16: C5  (523.3 Hz)
	dw 2217*2     ; 17: C#5 (554.4 Hz)
	dw 2092*2     ; 18: D5  (587.3 Hz)
	dw 1974*2     ; 19: D#5 (622.3 Hz)
	dw 1864*2     ; 20: E5  (659.3 Hz)
	dw 1759*2     ; 21: F5  (698.5 Hz)
	dw 1661*2     ; 22: F#5 (740.0 Hz)
	dw 1567*2     ; 23: G5  (784.0 Hz)
	dw 1479*2     ; 24: G#5 (830.6 Hz)
	dw 1396*2     ; 25: A5  (880.0 Hz)
	dw 1318*2     ; 26: A#5 (932.3 Hz)
	dw 1244*2     ; 27: B5  (987.8 Hz)

	; --- Octave 6 ---
	dw 1174*2     ; 28: C6  (1046.5 Hz)
	dw 1108*2     ; 29: C#6 (1108.7 Hz)
	dw 1046*2     ; 30: D6  (1174.7 Hz)
	dw 987 *2     ; 31: D#6 (1244.5 Hz)

 end


	lxi d, 8000
	mvi b, 1
	call Sound_Tone
	lxi d, 6000
	call Sound_Tone
	lxi d, 4000
	call Sound_Tone
	ret

Sound_Tone:
; [DE] = frequency
; [B] = duration in ???
	di
	mov a,e
	out Port81C55TimerLo
	mov a,d
	ori 0x40
	out Port81C55TimerHi
	mvi a,0xc3
	out Port81C55Cmd
	in Port81C55B
	ani 0xf8
	ori 0x20
	out Port81C55B

.delayLoop:
	push b
	lxi b, 0x12f

	call Delay
	
	pop b
	dcr b
	jnz .delayLoop

	in Port81C55B
	ori 0x4
	out Port81C55B
	ei
	ret

Delay:
; [BC] = iterations
	mov a,c
.outerLoop:
	push b
	mvi c, 72
.innerLoop:
	dcr c
	jnz .innerLoop
	pop b
	dcr a
	jnz .outerLoop
	dcr b
	jnz Delay
	ret
