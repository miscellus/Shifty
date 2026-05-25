TileIndexMask       equ 0b00011110
TileIndexShift      equ 1
GroundTileIndexMask equ 0b00000011
NeedsRedrawMask     equ 0b00000001

DirectionRight equ 0
DirectionUp    equ 1
DirectionLeft  equ 2
DirectionDown  equ 3

TargetNec equ 1
; TargetT100 equ 1
	include "hardware.8085.asm"

	org MapRamBase
GameStart:
	call GameInit
GameLoop:
	call CheckStopKey
	rc

	call ReadInput
	jc GameLoop ; If none of the movement keys were pressed, jump back

	call PlayerMove
	jc GameLoop

	call Draw
	jmp GameLoop


TryGetNeigborPos:
; [A] = direction (0-3)
; [D] = X position
; [E] = Y position
; -> [D] = X neighbor position
; -> [E] = Y neighbor position
; -> [CF]	= 1 when out of bounds, 0 otherwise

	cpi DirectionRight
	jnz .skipRight
	inr d
	jmp IsPosOutOfBounds
.skipRight:

	cpi DirectionUp
	jnz .skipUp
	dcr e
	jmp IsPosOutOfBounds
.skipUp:

	cpi DirectionLeft
	jnz .skipLeft
	dcr d
	jmp IsPosOutOfBounds
.skipLeft:

	cpi DirectionDown
	rnz
	inr e
	;jmp IsPosOutOfBounds

IsPosOutOfBounds:
; [D] = X pos
; [E] = Y pos
; -> [CF] 1 if out of bounds
; Clobbers [A]
	mOv a, e ; check y
	adi -8
	rc
	mov a, d ; check x
	adi -24
	ret

PlayerMove:
	; [C] = Direction (0 -> right, 1 -> up, 2 -> right, 3 -> down)
	; This procedure pushes the pushable positions to the stack
	; -> [A] = the number of positions pushed to the stack
  lhld PlayerPos
	xchg ; [D] = x, [E] = y

	push d ; push our initial position
	mvi b, 1 ; our initial position count is 1
.loop:
	lda PlayerMoveDir
	; mov a, c ; [A] = direction
	call TryGetNeigborPos
	jc .cancelMove ; return with carry out of bounds and the move should be cancelled

  call TileAddressFromPos
  mov a, m

	; Check for solid
	rlc ; [CF] = 1 means solid [CF] = 0 means not solid
	jc .cancelMove

	; Check for pushables
	rlc ; [CF] = 1 means pushable, 0 means empty in this case because it was not solid
	jc .foundPushable

	; Check for hole
	ani GroundTileIndexMask << 2 ; because of previous two RLC instructions
	cpi TileHole_Index << 2
	jz .foundHole

	; Assume we found empty
	jmp .performMove

.foundHole:
	; If closest tile was the player,
	;   the hole acts as a solid wall, so cancel the move
	; Else if the closest tile was a stone,
	;   we are moving a stone into a hole,
	;   so clear both and end in

	mov a, b
	cpi 1
	jz .cancelMove ; The closest tile must be the player tile (i.e. not a stone), so cancel the move

	; [DE] = furthest tile pos
	; [HL] = furthest tile address

	xthl ; [TOS] = furthest tile address, [HL] = closest tile pos
	xchg ; [DE] = closest tile pos, [HL] = furthest tile pos

	; Get address of closest pos
  call TileAddressFromPos ; [HL] = closest tile address
	inx h
	mov a, m
	ani TileIndexMask
	cpi TileCrateStone_Index << TileIndexShift
	jnz .cancelMove ; The previous tile was not a stone, so cancel move

	; Stone is going into hole

	; Clear stone tile to ground
	mov a, m
	ani ~TileIndexMask
	ori NeedsRedrawMask
	mov m, a
	dcx h
	mov a, m
	ani GroundTileIndexMask
	mov m, a

	; Clear hole to ground
	pop h ; [HL] = furthest tile address (the hole)
	mvi m, 0
	inx h
	mov a, m
	ani ~TileIndexMask
	ori NeedsRedrawMask
	mov m, a

	push d ; [TOS] = closest tile pos (restore the position stack)

	jmp .performMove

.foundPushable:
	; it's a pushable, so push it (^;
	push d
	inr b ; increment position count

	jmp .loop

.cancelMove:
	; Cancel the move, since we found a solid
	; Unwind the stack
	pop h
	dcr b
	jnz .cancelMove
	stc ; return with carry to indicate that the move was blocked
	ret

.performMove:
	; Check if we are moving the player this iteration (B=1).
	; If B=1, DE currently holds the target coordinates for the player.
	mov a, b
	cpi 1
	jnz .skipPlayerPosUpdate
	xchg               ; [HL] = new player pos
	shld PlayerPos     ; Update player position in memory
	xchg               ; Restore [DE] to target position
.skipPlayerPosUpdate:
	pop h ; [HL] = closest pos from player (from the stack)

	push h ; Save the closest pos. This MUST become the furthest pos for the next iteration!
	push b ; save loop count

	; Get address of closest pos
	push d ; save furthest pos
	  xchg ; [DE] = closest pos
	  call TileAddressFromPos ; [HL] = addr of closest
	  mov b, h
	  mov c, l ; [BC] = addr of closest
	pop d ; [DE] = furthest pos

	; Get address of furthest pos
	call TileAddressFromPos ; [HL] = addr of furthest

	; Write from closest pos [BC] to furthest pos [HL]
	mov a, m
	ani GroundTileIndexMask
	mov d, a ; [D] = preserved ground tile index from furthest pos

	ldax b
	ani ~GroundTileIndexMask
	ora d ; This
	mov m, a
	inx b
	inx h

	ldax b
	ori NeedsRedrawMask
	mov m, a

	pop b ; restore loop count
	pop d ; [DE] = closest pos (this is the NEW target space for the next loop!)

	; Decrement and loop until B hits 0
	dcr b
	jnz .performMove

	; [DE] = original player position before the move
	; Clear foreground tile on the starting position, the player just moved away from this tile.
  call TileAddressFromPos ; [HL] = addr of closest
  inx h
  mov a, m
  ani ~TileIndexMask
  ori NeedsRedrawMask
  mov m, a

	ora a ; clear carry bit to indicate that the move was performed successfully
	ret

ReadInput:
; Output:
;  [B], [A] = KeyUp | KeyDown | KeyLeft | KeyRight
;  flags set according to ANI
	lda KeyboardRow6Down
	cma
	mov b, a ; Store NOT of the old row 6 in b
	call ReadArrowKeyRow
	sta KeyboardRow6Down
	ana b
	sta KeyboardRow6Pressed
	mov b, a ; [B] = key state
	stc
	rz ; Return with carry set if no movement key was pressed

	ani KeyRight
	jz .rightNotPressed
	mvi c, DirectionRight
.rightNotPressed:

	mov a, b ; restore pressed

	ani KeyUp
	jz .upNotPressed
	mvi c, DirectionUp
.upNotPressed:

	mov a, b ; restore pressed

	ani KeyLeft
	jz .leftNotPressed
	mvi c, DirectionLeft
.leftNotPressed:

	mov a, b ; restore pressed

	ani KeyDown
	jz .downNotPressed
	mvi c, DirectionDown
.downNotPressed:

	mov a, c
	sta PlayerMoveDir

	ora a
	ret

LoadLevel:
; [HL] = pointer to level
; Clobbers [A]
; TODO(jkk): add compression and decompression
	push b
	push h

	lxi b, Level
.loop:
	mov a, m
	stax b
	inx h
	inx b
	mov a, b
	cpi high(LevelEnd)
	jnz .loop
	mov a, c
	cpi low(LevelEnd)
	jnz .loop

	pop h
	pop b
	ret

GameInit:
	lxi h, Level2
	call LoadLevel

	lda Level.PlayerStartX
	sta PlayerTileX

	lda Level.PlayerStartY
	sta PlayerTileY

	call Draw
	ret

TileOffsetFromPos:
; [D] = X pos (0 - 23)
; [E] = Y pos (0 - 7)
; -> [BC] = tile offset
; Clobbers [A] [BC]
	mov a, d
	add a
	add a
	add a ; + X*8
	add e ; + Y
	add a ; * 2
	mov c, a
	mvi a, 0
	adc a
	mov b, a
	ret

TileAddressFromPos:
; [D] = X pos (0 - 23)
; [E] = Y pos (0 - 7)
; -> [HL] = tile address
	push b
	call TileOffsetFromPos
	lxi h, Level.TileData
	dad b
	pop b
	ret

TileDataFromPos:
; [D] = X pos
; [E] = Y pos
; -> [BC] = Tile data
; Clobbers [PSW] [A]
	push h
	call TileAddressFromPos
	mov c, m
	inx h
	mov b, m
	pop h
	ret

Draw:
	call SetInterruptMask_1d
	lxi h, Level.TileData
	mvi d, 0 ; [D] = X = 0
.drawLevelRows:
	mvi e, 0 ; [E] = Y = 0
.drawLevelCols:

	mov b, m ; Tile attrib + ground tile index
	inx h
	mov a, m ; tile index + redraw flag
	rrc ; TileIndexShift
	jc .needsRedraw
	inx h ; Skip to next tile
	jmp .continue
.needsRedraw:

	ani TileIndexMask >> TileIndexShift ; shift because of previous RRC
	jnz .skipGroundTileImage
	mov a, b
	ani GroundTileIndexMask
.skipGroundTileImage:

	push h ; Save level offset
	  call TilePtrFromIndex
	  mov b, h
	  mov c, l ; [BC] = TileIndex * 10

	  lxi h, Tiles
	  dad b
	  ; [D] = X
	  ; [E] = Y
	  ; [HL] = TileOffset = Tiles + TileIndex
	    ; lxi h, TileWallBrick
	  call DrawTile
	pop h ; restore level offset

.clearRedrawFlag:
	rlc
	ani ~NeedsRedrawMask
	mov m, a
	inx h

.continue:
	inr e ; next Y
	mov a, e
	cpi 8
	jc .drawLevelCols
	inr d
	mov a, d
	cpi 24
	jc .drawLevelRows

	call SetInterruptMask_09
	ret

CheckStopKey:
	push b
	in Port81C55B
	mov b,a
	ori 0b00000001
	out Port81C55B
	in Port81C55A
	mov c,a
	mvi a, 0b01111111
	out Port81C55A
	in PortKeyIn
	push psw
	mvi a, 0b11111111
	out Port81C55A
	mov a,b
	ani 0b11111110
	out Port81C55B
	in PortKeyIn
	rrc
	mov a,c
	out Port81C55A
	mov a,b
	out Port81C55B
	pop b
	mov a,b
	rar
	ani 0b11000000
	pop b
	rnz
	inr a
	stc
	ret

	ifdef TargetNec
KeyUp    equ (1 << 1)
KeyDown  equ (1 << 2)
KeyLeft  equ (1 << 3)
KeyRight equ (1 << 4)
	endif
	ifdef TargetT100
KeyUp    equ (1 << 6)
KeyDown  equ (1 << 7)
KeyLeft  equ (1 << 4)
KeyRight equ (1 << 5)
	endif

ReadArrowKeyRow:
	; Result: [A] = bit mask for key state of row given in [C]
	; 1 = pressed, 0 = released
	push b
	di
	in   Port81C55B
	ori  0b00000001
	out  Port81C55B
	in   Port81C55A
	mov  b, a ; save old Port81C55A value
	ifdef TargetNec
		mvi  a, 0b10111111 ; Strobe on row 6
	endif
	ifdef TargetT100
		mvi  a, 0b11011111 ; Strobe on row 5
	endif
	out  Port81C55A
	in   PortKeyIn ; read keyboard bits (0 = pressed)
	mov  c, a

	mov  a, b ; restore Port81C55A
	out  Port81C55A

	mov  a, c
	ei
	pop  b
	cma ; Make 1 mean pressed and 0 mean NOT pressed
	ret

TilePtrFromIndex:
	; [A] = tile_index
	; <- [HL] = Tiles + tile_index * 10
	; <- [BC] = TileIndex * 2
	mvi h, 0
	mov l, a ; [HL] = TileIndex
	dad h ; * 2
	mov b, h
	mov c, l ; [BC] = TileIndex * 2
	dad h ; * 4
	dad h ; * 8
	dad b ; [HL] = TileIndex * 8 + TileIndex * 2 = TileIndex * 10
	ret

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
	out PortLcdCmd ; Set page and offset

	mvi c, 10
.WriteColumns:
	in PortLcdStat
	rlc ; shift busy bit out into carry bit
	jc .WriteColumns ; If cary set, LCD is busy, so keep looping	mov a, m
	mov a, m
	out PortLcdData ; Write column to LCD memory
	inx h
	dcr c
	jnz .WriteColumns

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
	out Port81C55A
	in Port81C55B
	ani 0b11111100
	ora h
	out Port81C55B
	ret

LcdWaitReady:
	push psw
.again:
	in PortLcdStat
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

;=======================================
; Tile images
Tiles:
  include "tiles.8085.asm"

;=======================================
; Levels
  include "levels.8085.asm"

;=======================================
; Game data

PlayerPos:
PlayerTileY: ds 1
PlayerTileX: ds 1

PlayerMoveDir: ds 1

; PlayerDeltaPos:
; PlayerDeltaY: ds 1
; PlayerDeltaX: ds 1

KeyboardRow6Down: ds 1
KeyboardRow6Pressed: ds 1

Level:
.PlayerStartY: ds 1
.PlayerStartX: ds 1
.TileData: ds 8*24*2
LevelEnd: