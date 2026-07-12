TargetNec equ 1
; TargetT100 equ 1
	include "hardware.8085.asm"

;
; How tiles are stored in the loaded level:
;
PushableMask        equ 0b10000000 ; bit 7 of tile
NeedsRedrawMask     equ 0b01000000 ; bit 6 of tile
TileIndexMask       equ 0b00011111 ; bits 0-5 of tile

; Direction encoding:
; bit 0: Axis (0: X, 1: Y)
; bit 1: Sign of direction along axis (0: positive, 1: negative)
DirectionSignBit equ 0b10
DirectionAxisBit equ 0b01
DirectionRight equ 0b00
DirectionUp    equ 0b01
DirectionLeft  equ 0b10
DirectionDown  equ 0b11

	org ProgramBaseAddr
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


TryGetNeigborAddr:
; [A] = direction
; [HL] = address
; -> [HL] = neighbor address
; -> [CY] = (1: out of bounds, 0: address valid)

	rrc
	jc .vertical

.horizontal:
	rrc
	mov a, l
	jc .left
.right:
	adi 8
	mov l, a
	cpi 8*24
	cmc
	ret
.left:
	sui 8
	mov l, a
	ret

.vertical:
	rrc
	mov a, l
	jc .down
.up:
	ani 7
	dcr a
	rlc
	dcr l
	ret
.down:
	ani 7
	cpi 7
	cmc
	inr l
	ret

PlayerMove:
; [C] = Direction (0 -> right, 1 -> up, 2 -> right, 3 -> down)
; This procedure pushes the pushable positions to the stack
; -> [A] = the number of positions pushed to the stack
	; xra a
	; sta PlayerMoveStoneStackIndex

	lda PlayerPos
	mvi h, high(Level)
	mov l, a

	push h ; push our initial position
	mvi b, 1 ; our initial position count is 1
.loop:
	lda PlayerMoveDir
	; mov a, c ; [A] = direction
	call TryGetNeigborAddr
	jc .foundSolid ; return with carry out of bounds and the move should be cancelled

	; call TileAddressFromPos
	mov a, m

	; Check for pushables
	cpi PushableMask
	jnc .foundPushable

	ani TileIndexMask

	; Check for wall
	cpi TileWallBrick_Index
	jz .foundSolid

	cpi TileDoorClosed_Index
	jz .foundSolid

	cpi TileDoorOpen_Index
	jz .foundOpenDoor

	; Check for hole
	cpi TileHole_Index
	jz .foundHole

	; If goal acted as a "pickup"
	; cpi TileGoal_Index
	; jz .foundGoal

	; Block the move if search has looped around and is trying to push into current player position
	xri TileBoxKidRight_Index
	cpi 4
	jc .foundSolid

	; Assume we found empty
	jmp .performMove

.foundOpenDoor:
	mov a, b
	cpi 1
	jnz .foundSolid

	; TODO(jkk): What if we have the following?
	; ..###    ..###
	; .@>D# or ..#D#
	; ..###    ..@^#

	pop h ; empty search stack
	lxi h, CurrentLevelIndex
	inr m
	mov a, m
	call GotoLevel
	ora a
	ret


.foundHole:
	; First we must find the head pushable tile.
	; Because the train of pushables could have turns signified by 0xFF sentinels,
	; we need to keep popping the stack as long as the top is a 0xFF turn sentinel.

.skipDirectionChangeSentinelsLoop:
	; We need to follow the arrows
	pop d
	dcr b
	jz .returnMoveBlocked
	mov a, d
	cpi 0xff
	jz .skipDirectionChangeSentinelsLoop

	; Now we are on the first non-direction change tile
	; If it is pushable, it should go in the hole

	ldax d
	ora a
	jp .cancelMove ; If bit 7 (sign bit) is 0, head is _NOT_ pushable, so cancel move

	; Head was a pushable, so it should go in the hole (remove both)

	; Test if head was a goal
	ani TileIndexMask
	cpi TileGoal_Index
	cz RemoveGoal

	; [HL] = current tile (the hole)
	; [DE] = head tile
	xchg
	mvi a, TileEmpty_Index | NeedsRedrawMask
	mov m, a
	stax d
	jmp .performMove

.foundPushable:
	; it's a pushable, so push it (^;
	push h
	inr b ; increment position count

	; ; If the pushable is a stone, then store the stack index of the stone
	; ; This stack index will be used when we encounter a hole.
	; inx h
	; mov a, m
	; ani TileIndexMask
	; cpi TileCrateStone_Index
	; jnz .loop
	;   mov a, b
	;   sta PlayerMoveStoneStackIndex
	jmp .loop

.foundSolid:
	; Go backwards through the stack and find the first arrow pointing
	; at a right angle to the current direction of movement.
	; If such a perpendicular arrow is found:
	; return from here and continue searching for solids from that arrow in the direction dictated by that arrow.
	;
	; i.e. the arrow changes the direction of search
	;
	; If during this search we get all the way back to the player, the move can't be performed.
	;

	; [B] = the number of places pushed to the stack so far
	; [SP] = the top of the stack, currently pointing to the last pushed thing
.perpArrowSearchLoop:
	pop h
	dcr b
	jz .returnMoveBlocked

	; We must check if this is a real position or a search direction change
	mov a, h
	cpi 0xFF
	jnz .notDirectionChangeSentinel
	mov a, l ; [A] = Restored search direction
	sta PlayerMoveDir
	jmp .perpArrowSearchLoop

.notDirectionChangeSentinel:

	mov a, m ; Get tile index
	ani TileIndexMask

	; Test for goal
	cpi TileGoal_Index
	jnz .foundSolid_notGoal
	  call RemoveGoal
	  jmp .performMove
.foundSolid_notGoal:

	xri TileRightArrow_Index
	cpi 4
	jnc .perpArrowSearchLoop ; Not an arrow

	; At this point, it is an arrow
	mov c, a ; [C] = Arrow direction

	; If along the same movement axis, keep looping
	lda PlayerMoveDir
	mov e, a ; [E] = Current search direction
	xra c
	rrc ; [CY] = PlayerMoveDir.Axis XOR arrow.Axis
	jnc .perpArrowSearchLoop ; Keep searching, this arrow is pointing along current movement axis, we need to find a perpendicular arrow

	; The found arrow is perpendicular

	; Before updating the search direction, we first push a
	; "revert search direction"-word to the stack
	; to handle this scenario found by Eydi av Hamri.
	;                                  Thanks friend. (^:
	;         @
	;         >#
	;         ^#
	;         ##

	mvi d, 0xff ; Sentinel that we can tell apart from a position
	push d ; [DE] = 0xFF<Current search direction>
	inr b ; account for added stack entry

	; Now, change the PlayerMoveDir
	mov a, c ; [A] = Arrow direction/
	sta PlayerMoveDir

	jmp .loop ; Continue main loop

.cancelMove:
	; Cancel the move, since we found a solid
	; Unwind the stack
	pop h
	dcr b
	jnz .cancelMove
.returnMoveBlocked:
	stc ; return with carry to indicate that the move was blocked
	ret

.performMove:
	; [HL] = furthest tile from player

	; Check if we are moving the player this iteration (B=1).
	; If B=1, DE currently holds the target coordinates for the player.
	mov a, b
	cpi 1
	jnz .skipPlayerPosUpdate
	; [HL] = new player pos
	mov a, l
	sta PlayerPos     ; Update player position in memory
.skipPlayerPosUpdate:
	pop d
	mov a, d
	cpi 0xFF ; Detect search direction sentinel
	jz .performMoveDecrementAndLoop

	; [DE] = closest tile from player (from the stack)
	; [HL] = furthest tile from player

	; Write from closest pos [DE] to furthest pos [HL]
	ldax d
	ori NeedsRedrawMask
	mov m, a

	xchg ; [HL] = closest tile from player

.performMoveDecrementAndLoop:
	; Decrement and loop until B hits 0
	dcr b
	jnz .performMove

	; [HL] = original player position before the move
	; Clear foreground tile on the starting position, the player just moved away from this tile.
	;xchg ; [HL] = losest tile from player
	mvi m, TileEmpty_Index | NeedsRedrawMask

	ora a ; clear carry bit to indicate that the move was performed successfully
	ret


RemoveGoal:
	push h
	mvi a, TileEmpty_Index
	mov m, a

	lxi h, MissingTargets
	dcr m
	jnz .end

	lxi h, Level | 8*24; [HL] => Level base
.openDoorsLoop:
	dcr l
	mov a, m
	ani TileIndexMask
	cpi TileDoorClosed_Index
	jnz .notClosedDoor
	; Open the closed door
	mvi m, TileDoorOpen_Index | NeedsRedrawMask
.notClosedDoor:
	mov a, l
	ora a
	jnz .openDoorsLoop
.end:
	pop h
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

	mov a, b ; restore pressed
	ani KeyRestartLevel
	lda CurrentLevelIndex
	jz .restartLevelNotPressed
	call GotoLevel
	call Draw
	stc
	ret
.restartLevelNotPressed:
	mov a, c
	sta PlayerMoveDir

	ora a
	ret

GotoLevel:
; [A] = Level index to go to
	add a ; 2 * level index
	lxi b, LevelLookupTable
	add c
	mov c, a
	mvi a, 0
	adc b
	ldax b
	mov l, a
	inx b
	ldax b
	mov h, a
	; jmp LoadLevel

	; Intented fallthrough

LoadLevel:
; [HL] = pointer to compressed level data
; Clobbers [A]
; Returns: Level buffer filled, PlayerStartY/X set
	lxi b, Level ; [BC] = Destination pointer for RAM buffer

; Compressed byte format: CCC TTTTT
; CCC = run length 1 - 8 (000 maps to 1, 111 maps to 8)
; TTTTT = tile index
.readCompressed:
	mov a, m
	inx h
	mov d, a ; [D] = compressed packet

	ani 0x1f ; [A] = tile index 0-31

	; Lookup in tile lookup table that maps tile index to tile index + attributes
	push h ; Save read ptr
	  lxi h, TileInfoFromTileIndexMap
	  add l
	  mov l, a
	  mvi a, 0
	  adc h
	  mov h, a ; [HL] = &TileInfoFromTileIndexMap[TileID]
	  mov e, m ; [E] = Decompressed tile info
	pop h ; Restore read ptr

	mov a, d
	rlc
	rlc
	rlc
	ani 7
	inr a
	mov d, a ; [D] = run count (1 to 8)

.writeRun:
	mov a, e
	stax b ; Write tile info to loaded level buffer
	inx b ; Advance write ptr
	dcr d
	jnz .writeRun

	; Check if we are done decompressing the level data
	mov a, b
	cpi high(LevelEnd)
	jnz .readCompressed
	mov a, c
	cpi low(LevelEnd)
	jnz .readCompressed

	; [HL] = Points at player starting position for level
	mov a, m
	sta PlayerPos

	; [HL] = Points at number of targets in level
	inx h
	mov a, m
	sta MissingTargets

	ret

GameInit:
	mvi a, 0
	sta CurrentLevelIndex
	call GotoLevel
	call Draw
	ret

TileAddressFromPos:
; [D] = X pos (0 - 23)
; [E] = Y pos (0 - 7)
; -> [HL] = tile address
	lxi h, Level
	mov a, d
	add a
	add a
	add a ; + X*8
	add e ; + Y

	; [A] = Tile Offset

	add l
	mov l, a
	mvi a, 0
	adc h
	mov h, a
	ret

Draw:
	call SetInterruptMask_1d
	lxi h, Level
	mvi d, 0 ; [D] = X = 0
.drawLevelRows:
	mvi e, 0 ; [E] = Y = 0
.drawLevelCols:

	mov a, m ; [A] Tile Info
	rlc
	rlc
	jnc .continue

	rrc
	rrc
	ani TileIndexMask

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
	mov a, m
	ani ~NeedsRedrawMask & 0xff
	mov m, a

.continue:
	inx h
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
KeyRestartLevel equ (1 << 7)
	endif
	ifdef TargetT100
KeyUp    equ (1 << 6)
KeyDown  equ (1 << 7)
KeyLeft  equ (1 << 4)
KeyRight equ (1 << 5)
KeyRestartLevel equ ???
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
	jc .WriteColumns ; If cary set, LCD is busy, so keep looping
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
VariablesStart:

PlayerPos: ds 1

PlayerMoveDir: ds 1

MissingTargets: ds 1

CurrentLevelIndex: ds 1
; PlayerMoveStoneStackIndex: ds 1

KeyboardRow6Down: ds 1
KeyboardRow6Pressed: ds 1

VariablesEnd:

; Align level to 256 offset
; This eneables us to translate easily between
; Y coordinates and the 3 least significant
; bits of the tile address.
Level equ ($ + 0xff) & 0xff00
LevelEnd equ Level + 8*24
	assert $ < ProgramLimitAddr
	assert Level - VariablesEnd < 250
