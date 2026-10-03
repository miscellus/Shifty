TargetNec equ 1
	include "hardware.8085.asm"

	org 50000

	call Buzzer_PlayChirpUp

	lxi b, 10000       ; Delay speed
	call Delay

	call Buzzer_PlayFanfare

	lxi b, 10000       ; Delay speed
	call Delay

	call Buzzer_PlayChirpDown

	lxi b, 10000       ; Delay speed
	call Delay

	call Buzzer_PlayChirpDown

	lxi b, 10000
	call Delay
	
	lxi h, Melody_ZeldaChest
	call Buzzer_PlayMelody

	ret

; ======================================================
; MELODY DATA
; Format: dw Pitch_Divisor, Duration_Seed
; ======================================================
Melody_ZeldaChest:
    dw 2400, 0x082f     ; Note 1 (Low, short)
    dw 2100, 0x082f     ; Note 2
    dw 1900, 0x082f     ; Note 3
    dw 1400, 0x202f     ; Note 4 (High, long)
    dw 0, 0             ; END MARKER (Pitch = 0)

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

; ======================================================
; PLAY MELODY FROM DATA
; [HL] = Pointer to the melody data table
; ======================================================
Buzzer_PlayMelody:
    di
    call Buzzer_On

.readNote:
    ; 1. Read Pitch into DE
    mov e, m            ; Load lower byte of pitch
    inx h
    mov d, m            ; Load upper byte of pitch
    inx h

    ; 2. Check if Pitch is 0 (End of melody)
    mov a, d
    ora e               ; Logical OR of D and E. Result is 0 only if both are 0.
    jz .endMelody
    
    call Buzzer_SetFreq ; Update hardware with new pitch

    ; 3. Read Duration into BC
    mov c, m            ; Load lower byte of duration
    inx h
    mov b, m            ; Load upper byte of duration
    inx h

    ; 4. Play the note
    push h              ; Protect our data pointer on the stack
    call Delay
    pop h               ; Restore pointer
    
    jmp .readNote       ; Loop to the next note

.endMelody:
    call Buzzer_Off
    ei
    ret

Buzzer_PlayFanfare:
	di
	call Buzzer_On

	lxi d, 5000
	call Buzzer_SetFreq
	lxi b, 400       ; Duration (0x08 is ~160ms)
	call Delay

	lxi d, 4000
	call Buzzer_SetFreq
	lxi b, 400       ; Duration (~160ms)
	call Delay

	lxi d, 3000
	call Buzzer_SetFreq
	lxi b, 900       ; Duration (~160ms)
	call Delay

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
	ori 0x40
	out Port81C55TimerHi
	mvi a, 0xc3
	out Port81C55Cmd
	ret

Buzzer_Off:
; clobbers [A]
	in Port81C55B
	ori 0x04
	out Port81C55B
	ret

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
