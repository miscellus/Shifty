

	org 40960
GameStart:
	lxi h, LcdWaitReady
	mvi d, 21
	mvi e, 5
	di
	call DrawTile
.h:
	jmp .h

	ds 8
	ldax b

DrawTile:
; [hl] = Pointer to 10x8 tile
; [D] = Tile X [0; 23]
; [E] = Tile Y [0;  7]
	push h
	push d
	push b
	push psw

	push h
	call LcdGetBlockMask
	call LcdSelectBlock
	pop h
	call LcdCalcPageAndOffset
	call LcdWaitReady
	out 0xFE ; Set page and offset

	mvi c, 9
.WriteColumns:
	in 0xFE
	rlc ; shift busy bit out into carry bit
	jc .WriteColumns ; If cary set, LCD is busy, so keep looping	mov a, m
	mov a, m
	out 0xFF ; Write column to LCD memory
	inx h
	dcr c
	jnz .WriteColumns

	call LcdWaitReady
	mov a, m
	out 0xFF ; Write column to LCD memory

	pop psw
	pop b
	pop d
	pop h
	ret

; -----------------------------------------------------------
; Subroutine: LcdGetBlockMask
; Purpose:    Computes segment driver mask (1 << n) for PC-8201A
; Input:      D = TileX (0 to 23)
;             E = TileY (0 to 7)
; Output:     HL = 16-bit Driver Selection Mask (1 << n)
;             B  = Driver Index 'n' (0 to 9)
;             C  = Local TileX offset inside the driver (0 to 4)
; Destroys:   A, B, C, H, L, Flags
; -----------------------------------------------------------

LcdGetBlockMask:
	; BASE INDEX (TileX / 5)
	mov a, d      ; Load TileX into A
	mvi b, 0      ; Initialize B (will hold our driver index 'n')
	mvi c, 5      ; Load constant divisor (5)

.divLoop:
	cmp c         ; Compare working remainder with 5
	jc .checkY    ; If A < 5, division is done
	sub c         ; Subtract 5
	inr b         ; Increment driver index
	jmp .divLoop

.checkY:
	; HANDLE TOP/BOTTOM HALF
	mov c, a      ; SAVE REMAINDER: C is now the local TileX offset (0-4)!

	mov a, e      ; Load TileY into A
	ani 4         ; Check bit 2 (0000 0100). High if TileY >= 4.
	jz .shiftMask ; If zero, it's the top half. Index 'n' is ready.

	; If non-zero, it's the bottom half. Add 5 to the index.
	mov a, b
	adi 5
	mov b, a      ; B now holds final 'n' (5 to 9)

.shiftMask:
	; 16-BIT MASK (1 << n)
	lxi h, 1      ; Start with mask = 1 (16-bit)
	mov a, b      ; Use 'n' as our loop counter
	ora a         ; Is 'n' exactly 0?
	rz            ; If yes, Return immediately (HL is already correct)

.shiftLoop:
	dad h          ; HL = HL * 2. This shifts our 16-bit mask left natively!
	dcr a          ; Decrement shift counter
	jnz .shiftLoop ; Repeat until A is 0

	ret            ; Done. Mask in HL, Local Offset in C, Index in B.

LcdSelectBlock:
; [L] = LCD Block bitmask bits 0-7
; [H] = LCD Block bitmask bits 8-9
	mov a,l
	out 0xB9
	in 0xBA
	ani 0b11111100
	ora h
	out 0xBA
	ret

LcdWaitReady:
	push psw
.again:
	in 0xFE
	rlc ; shift busy bit out into carry bit
	jc .again ; If cary set, LCD is busy, so keep looping

	pop psw
	ret

; -----------------------------------------------------------
; Subroutine: LcdCalcPageAndOffset
; Purpose:    Computes the PP0OOOOO byte for HD44102CH LCD driver
; Input:      C = Local TileX offset (0 to 4)  <-- From CALC_LCD_MASK
;             E = TileY (0 to 7)
; Output:     A = Formatted Command Byte (Bits 7,6=Page, 5-0=Offset)
; Destroys:   A, B, D, Flags
; -----------------------------------------------------------

LcdCalcPageAndOffset:
	; Page (Bits 6,7)
	mov a, e      ; Load TileY (0 to 7)
	rrc           ; Rotate right once  (A = P000 000P)
	rrc           ; Rotate right twice (A = PP00 0000)
	ani 0b11000000; Mask out the top/bottom half bit
	mov b, a      ; Save the shifted Page mask in B

	; Offset (Local TileX * 10)
	mov a, c      ; Load Local TileX (0 to 4)
	add a         ; A = x * 2
	mov d, a      ; Save (x * 2) in D for later

	add a         ; A = x * 4
	add a         ; A = x * 8
	add d         ; A = (x * 8) + (x * 2) = x * 10
	              ; A now holds the pixel offset (0, 10, 20, 30, or 40)

	; --- 3. COMBINE PAGE AND OFFSET ---
	ora b         ; Bitwise OR the offset with the Page mask stored in B

	ret           ; Done. The formatted PP0OOOOO byte is ready in A.


SetInterruptMask_1d:
	di
	mvi a, 0x1d
	sim
	ei
	ret

SetInterruptMask_09:
	di
	mvi a, 0x09
	sim
	ei
	ret
