TargetNec equ 1
; TargetT100 equ 1
	include "hardware.8085.asm"

;
; How tiles are stored in the loaded level:
;
PushableMask        equ 0b10000000 ; bit 7 of tile
NeedsRedrawMask     equ 0b01000000 ; bit 6 of tile
ActiveTileMask      equ 0b00100000 ; bit 5 of tile
TileIndexMask       equ 0b00011111 ; bits 0-4 of tile

; Direction encoding:
; bit 0: Axis (0: X, 1: Y)
; bit 1: Sign of direction along axis (0: positive, 1: negative)
DirectionSignBit equ 0b10
DirectionAxisBit equ 0b01
DirectionRight equ 0b00
DirectionUp    equ 0b01
DirectionLeft  equ 0b10
DirectionDown  equ 0b11

; Normalized Virtual Gamepad Bits
PadRight   equ (1 << 0)
PadUp      equ (1 << 1)
PadLeft    equ (1 << 2)
PadDown    equ (1 << 3)
PadUndo    equ (1 << 4)
PadRestart equ (1 << 5)
PadExit    equ (1 << 6)

	org ProgramBaseAddr

	call ShowTitle
GameStart:
	call ReadInput
	jc GameStart
	call GameInit

GameLoop:
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

	lda PlayerPos
	mvi h, high(Level)
	mov l, a

	push h ; push our initial position
	mvi b, 1 ; our initial position count is 1
Move_SearchLoop:

	lda PlayerMoveDir
	call TryGetNeigborAddr
	jc Move_FoundSolid ; return with carry out of bounds and the move should be cancelled

	mov a, m ; [A] = tile info

	; Check for pushables
	cpi PushableMask
	jnc Move_FoundPushable

	ani TileIndexMask

	; Check for wall
	cpi TileWallBrick_Index
	jz Move_FoundSolid

	cpi TileDoorClosed_Index
	jz Move_FoundSolid

	cpi TileDoorOpen_Index
	jz Move_FoundOpenDoor

	; Check for hole
	cpi TileHole_Index
	jz Move_FoundHole

	; Block the move if search has looped around and is trying to push into current player position
	xri TileBoxKidRight_Index
	cpi 4
	jc Move_FoundSolid

	; Assume we found empty
	jmp Move_Perform

Move_FoundOpenDoor:
	mov a, b
	cpi 1
	jnz Move_FoundSolid

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


Move_FoundHole:
	; First we must find the head pushable tile.
	; Because the train of pushables could have turns signified by 0xFF sentinels,
	; we need to keep popping the stack as long as the top is a 0xFF turn sentinel.

.skipDirectionChangeSentinelsLoop:
	; We need to follow the arrows
	pop d
	dcr b
	jz SetCarryAndReturn
	mov a, d
	cpi 0xff
	jz .skipDirectionChangeSentinelsLoop

	; Now we are on the first non-direction change tile
	; If it is pushable, it should go in the hole

	ldax d
	ora a
	jp Move_Cancel ; If bit 7 (sign bit) is 0, head is _NOT_ pushable, so cancel move

	; Head was a pushable, so it should go in the hole (remove both)

	; Test if head was a goal
	ani TileIndexMask
	cpi TileGoal_Index
	cz RemoveGoal

	; [HL] = current tile (the hole)
	; [DE] = head tile
	call Undo_SaveTile
	xchg
	call Undo_SaveTile
	mvi a, TileEmpty_Index | NeedsRedrawMask
	mov m, a
	stax d
	jmp Move_Perform

Move_FoundPushable:
	; it's a pushable, so push it (^;
	push h
	inr b ; increment position count
	jmp Move_SearchLoop

Move_FoundSolid:
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
	jz SetCarryAndReturn ; TODO(jkk): Get rid of any highlight added to perp-arrows

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
	jnz .notGoal
	  call RemoveGoal
	  jmp Move_Perform
.notGoal:

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
	mov a, m
	ori ActiveTileMask | NeedsRedrawMask
	mov m, a

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

	jmp Move_SearchLoop ; Continue main loop

Move_Cancel:
	; Cancel the move, since we found a solid
	; Unwind the stack
	mov a, m
	ani ~(ActiveTileMask | NeedsRedrawMask)
	mov m, a

	pop h
	dcr b
	jnz Move_Cancel
SetCarryAndReturn:
	stc ; return with carry to indicate that the move was blocked
	ret

Move_Perform:
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
	jz .decrementAndLoop

	; [DE] = closest tile from player (from the stack)
	; [HL] = furthest tile from player

	; Write from closest pos [DE] to furthest pos [HL]
	ldax d ; [A] = closest tile
	mov c, m ; [C] = furthest tile (overwritten)
	cmp c
	jz .tileDidntChange ; Skip writing tile that didn't change

	call Undo_SaveTile

	ori NeedsRedrawMask
	ani ~ActiveTileMask
	mov m, a

.tileDidntChange:
	xchg ; [HL] = closest tile from player

.decrementAndLoop:
	; Decrement and loop until B hits 0
	dcr b
	jnz Move_Perform

	; [HL] = original player position before the move
	; Clear foreground tile on the starting position, the player just moved away from this tile.
	;xchg ; [HL] = losest tile from player
	call Undo_SaveTile
	mvi m, TileEmpty_Index | NeedsRedrawMask

	call Undo_EndMoveRecord

	ora a ; clear carry bit to indicate that the move was performed successfully
	ret

Undo_EndMoveRecord:
	push b
	push h

	lda UndoBufferAt
	mvi h, high(UndoBuffer)
	mov l, a
	lda UndoEntryCount
	mov m, a
	inr l
	mvi m, 0xff ; sentinel
	inr l
	mov a, l
	sta UndoBufferAt

	; Search ahead to see if we truncated the oldest move record,
	; and if we did, disable that move record by clearing the FF sentinel
	mvi c, 0 ; [C] = search distance
	mov a, m
.searchSentinel:
	cpi 0xff
	jz .foundSentinel
	inr c
	jz .oldestRecordNotTruncated
	inr l
	mov a, m
	jmp .searchSentinel

.foundSentinel:
	; [L] points at oldest move record sentinel
	dcr l
	; [L] points at oldest move record entry count

	; If oldest move record entry count exceeds the search distance
	mov a, m
	add a ; *2 to get byte count
	cmp c
	jc .oldestRecordNotTruncated

	; The oldest record has been truncated, so we must clear its
	; sentinel to 0 to disable it.
	inr l
	mvi m, 0

.oldestRecordNotTruncated:
	lxi h, UndoEntryCount
	mvi m, 0

	pop h
	pop b
	ret

Undo_SaveTile:
; [L] = Tile position
; Clobbers: None
	push psw
	push d
	push h

	mov d, l ; [d] = tile pos
	mov e, m ; [e] = tile info

	mvi h, high(UndoBuffer)
	lda UndoBufferAt
	mov l, a

	mov m, d ; tile pos first
	inr l
	mov m, e ; then tile
	inr l

	mov a, l
	sta UndoBufferAt

	lxi h, UndoEntryCount
	inr m

	pop h
	pop d
	pop psw
	ret

RemoveGoal:
	call Undo_SaveTile
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
	call Undo_SaveTile
	mvi m, TileDoorOpen_Index | NeedsRedrawMask
.notClosedDoor:
	mov a, l
	ora a
	jnz .openDoorsLoop
.end:
	pop h
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

	call InitLevelVariables

Undo_Clear:
	push h
	xra a
	sta UndoEntryCount
	sta UndoBufferAt
	assert low(UndoBuffer) == 0
	lxi h, UndoBuffer
.loop:
	mov m, a
	dcr l
	jnz .loop
	pop h
	ret

InitLevelVariables:
; Scans the loaded level and sets the following variables:
; - PlayerPos
; - MissingTargets
	push b
	push h
	assert low(Level) == 0
	mvi h, high(Level)
	xra a
	mov l, a
	mov c, a
.loop:
	mov a, m
	inr l
	jz .end

	ani TileIndexMask
	cpi TileGoal_Index
	jnz .notTarget
	  inr c
.notTarget:
	xri TileBoxKidRight_Index
	cpi 4
	jnc .notThePlayer
	  mov a, l
	  dcr a
	  sta PlayerPos
.notThePlayer:
	jmp .loop
.end:
	mov a, c
	sta MissingTargets

	pop h
	pop b
	ret

Undo:
	push b
	push d
	push h

	lda UndoBufferAt
	mvi h, high(UndoBuffer)
	mov l, a

	dcr l
	mov a, m
	cpi 0xff
	jnz .end ; Undo buffer empty

	mvi m, 0 ; Clear move record being un-done

	; [HL] = UndoBufferAt
	dcr l
	mov c, m ; [C] = entry count

	mvi d, high(Level)
.undoLoop:
	dcr l
	mov a, m ; [A] = tile info
	ori NeedsRedrawMask
	dcr l
	mov e, m ; [E] = tile pos
	stax d ; Restores the tile from the undo

	dcr c
	jnz .undoLoop

	mov a, l
	sta UndoBufferAt

	call InitLevelVariables

.end:
	pop h
	pop d
	pop b
	ret

ShowTitle:
	di
	mvi l, 0
	lxi d, .splash
.loop:
	call DrawTile
	inr l
	mvi a, 191
	cmp l
	jnc .loop
	ei
	ret
.splash:
 include "splash.8085.asm"

GameInit:
	mvi a, 0
	sta CurrentLevelIndex
	call GotoLevel
	call Draw
	ret

Draw:
	call SetInterruptMask_1d
	lxi h, Level
.nextTile:
	mov a, m
	mov c, a ; [C] = Tile Info
	rlc
	rlc
	jnc .continue

	mov a, c
	mvi b, 0
	ani ActiveTileMask
	jz .drawTileNotActive
	dcr b
.drawTileNotActive:

	mov a, c
	ani TileIndexMask

	call TilePtrFromIndex
	call DrawTile

.clearRedrawFlag:
	mov a, m
	ani ~NeedsRedrawMask
	mov m, a

.continue:
	inr l
	mvi a, 191
	cmp l
	jnc .nextTile

	call SetInterruptMask_09
	ret

ReadInput:
; Output:
;  [B], [A] = Normalized newly pressed keys
;  Carry flag set if no movement key was pressed

	call VirtualPad_ReadStable
	mov d, a                    ; [D] = Current stable virtual state

	; Calculate newly pressed keys (Edge detection)
	lda VirtualPadDown          ; [A] = Old virtual state
	cma                         ; [A] = NOT Old
	ana d                       ; [A] = (NOT Old) AND Current = Newly pressed
	sta VirtualPadPressed
	mov b, a                    ; [B] = Newly pressed keys

	; Update state for next frame
	mov a, d
	sta VirtualPadDown

	; Check if any key was pressed
	mov a, b
	stc
	rz                          ; Return with carry set if nothing was pressed

	ani PadRight
	jz .rightNotPressed
	mvi c, DirectionRight
.rightNotPressed:

	mov a, b
	ani PadUp
	jz .upNotPressed
	mvi c, DirectionUp
.upNotPressed:

	mov a, b
	ani PadLeft
	jz .leftNotPressed
	mvi c, DirectionLeft
.leftNotPressed:

	mov a, b
	ani PadDown
	jz .downNotPressed
	mvi c, DirectionDown
.downNotPressed:

	mov a, b
	ani PadUndo
	jz .undoNotPressed
	call Undo
	call Draw
	stc
	ret
.undoNotPressed:

	mov a, b
	ani PadRestart
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


VirtualPad_ReadStable:
	push d

	call VirtualPad_ReadRaw
	mov d, a ; [D] = previous row state

.debounceWait:
	lxi h, 1024 ; ~10ms delay at 2.4576 MHz
.delayLoop:
	dcx h
	mov a, h
	ora l
	jnz .delayLoop

	call VirtualPad_ReadRaw

	; compare new state [A] with previous state [D]
	cmp d
	mov d, a ; new state -> old state
	jnz .debounceWait ; still bouncing

	pop d
	ret

GET_KEY_ROW	macro row
	lxi b, 0x01FF & ~(1 << #row)
	call Keyboard_ReadRow_NoRestore
	endm

GET_KEY	macro bit, padflag
	lxi h, ((1 << #bit) << 8) | #padflag
	call __GetKey
	endm
__GetKey:
	mov a, b
	ana  h
	rz
	mov  a, d
	ora  l
	mov  d, a
	ret

VirtualPad_ReadRaw:
	push b
	push d
	di
	in Port81C55A
	mov d, a
	in Port81C55B
	mov e, a
	push d

	mvi d, 0 ; [D] = VirtualPad

  ifdef TargetNec
	GET_KEY_ROW 6
	GET_KEY 4, PadRight
	GET_KEY 1, PadUp
	GET_KEY 3, PadLeft
	GET_KEY 2, PadDown
	GET_KEY 0, PadUndo
	GET_KEY 7, PadRestart

	GET_KEY_ROW 0
	GET_KEY 0, PadUndo ; Z

	GET_KEY_ROW 2
	GET_KEY 1, PadUp ; W

	GET_KEY_ROW 1
	GET_KEY 0, PadLeft ; A
	GET_KEY 1, PadDown ; S
	GET_KEY 2, PadRight ; D

	GET_KEY_ROW 7
	GET_KEY 7, PadExit ; STOP Key
  endif
  ifdef TargetT100
  assert false ; todo
  endif

	pop b
	mov a, c
	out Port81C55B
	mov a, b
	out Port81C55A
	ei

	mov a, d
	pop d
	pop b
	ret


Keyboard_ReadRow_NoRestore:
; [BC] = row strobe index mask
; [B] = read row in (active high)
	mov a, e
	ani  0b11111110
	ora  b
	out  Port81C55B

	; Set strobe for rows 0 - 7
	mov  a, c ; [C] = strobe for rows 0 - 7
	out  Port81C55A

	; read keyboard bits (0 = pressed)
	in   PortKeyIn
	cma
	mov b, a
	ret

TilePtrFromIndex:
	; [A] = tile_index
	; <- [DE] = Tiles + tile_index * 10
	push h
	mvi h, 0
	mov l, a ; [HL] = TileIndex
	dad h ; * 2
	mov d, h
	mov e, l ; [BC] = TileIndex * 2
	dad h ; * 4
	dad h ; * 8
	dad d ; [HL] = TileOffset = TileIndex * 8 + TileIndex * 2 = TileIndex * 10
	lxi d, Tiles
	dad d
	xchg ; [DE] = Tile ptr = Tiles + TileOffset
	pop h
	ret

DrawTile:
; [de] = Pointer to 10x8 tile
; [l] = Tile position (XXXXXYYY - bits 0-2: Y - bits 3-7: X)
; [b] = XOR mask for tile image (can be used to invert tile pixels)
; -> [de] Pointer to next 10x8 tile (input ptr + 10)
	push psw
	push b
	push h

	call LCD_SelectDriver
	;call LCD_SetPageAndOffset

	mvi c, 10
.WriteColumns:
	in PortLcdStat
	rlc ; shift busy bit out into carry bit
	jc .WriteColumns ; If cary set, LCD is busy, so keep looping
	ldax d
	xra b
	out PortLcdData ; Write column to LCD memory
	inx d
	dcr c
	jnz .WriteColumns

	pop h
	pop b
	pop psw
	ret

; -----------------------------------------------------------
; Subroutine: LCD_SelectDriver
; Purpose:    Computes segment driver mask (1 << n) for PC-8201A
; Output:     HL = 16-bit Driver Selection Mask (1 << n)
;             B  = Driver Index 'n' (0 to 9)
;             C  = Local TileX offset inside the driver (0 to 4)
; Destroys:   A, B, C, H, L, Flags
; -----------------------------------------------------------

LCD_SelectDriver:
; [L] = Tile Pos = TileX << 5 | TileY
; -> [C] = TileX % 5 (0-4)
	push h

	; Extract TileX
	mov a, l
	rrc
	rrc
	rrc
	ani 31
	mov c, a ; [A] = TileX

	; The LCD of the KC-85 family is controlled by
	; 10 separate LCD drivers indexed from 0-9.
	; Their index correspond to this physical layout of the display:
	;
	;   +-------+-------+-------+-------+-----+ +
	;   |       |       |       |       |     |   <-- Drivers 4 and 9 are "cropped"
	;   |   0   |   1   |   2   |   3   |   4 | |     i.e. you can address pixels
	;   |       |       |       |       |     |       outside the physical display
	;   +-------+-------+-------+-------+-----+ +
	;   |       |       |       |       |     |
	;   |   5   |   6   |   7   |   8   |   9 | |
	;   |       |       |       |       |     |
	;   +-------+-------+-------+-------+-----+ +
	;
	; The LCD drivers are enabled by a selection bit mask
	; where a 1 bit in position N enables the LCD controller
	; with index N.

	; Compute [HL] = LCD driver chip selection mask
	; Start mask at 0000000001 (corresponding to Driver 0)
	; Unless TileY >= 4, then
	; start mask at 0000100000 (corresponding to Driver 5)
	mov a, l
	lxi h, 1 << 0
	ani (1 << 2)
	jz .topHalf
	mvi l, 1 << 5
.topHalf:

	; Figure out which LCD driver column we are in by
	; dividing TileX with 5 (by repeated subtraction)
	mov a, c ; [A] = TileX
	mvi c, 5 ; [C] = divisor
.divLoop:
	cmp c
	jc .endDivLoop ; If A < 5, division is done
	sub c ; -= 5
	dad h ; shift HL left by 1
	jmp .divLoop

.endDivLoop:
	mov c, a ; [C] = TileX % 5 (0-4)

	; Apply the LCD driver chip selection mask
	; [L] = LCD Block bitmask bits 0-7
	; [H] = LCD Block bitmask bits 8-9
	mov a,l
	out Port81C55A
	in Port81C55B
	ani 0b11111100
	ora h
	out Port81C55B

	pop h
	;ret


LCD_SetPageAndOffset:
; Computes the PP0OOOOO byte for HD44102CH LCD driver
; [C] = Local TileX (0 to 4)
; [L] = Tile position (Tile Y in bits 0-3)
	; Page (Bits 6,7)
	push b

	mov a, l      ; Load TileY (0 to 7)
	rrc           ; Rotate right once  (A = P000 000P)
	rrc           ; Rotate right twice (A = PP00 0000)
	ani 0b11000000; Mask out the top/bottom half bit
	mov b, a      ; Save the shifted Page mask in B

	; Offset (Local TileX * 10)
	mov a, c      ; Load Local TileX (0 to 4)
	add a         ; A = x * 2
	mov c, a      ; Save (x * 2) in D for later

	add a         ; A = x * 4
	add a         ; A = x * 8
	add c         ; A = (x * 8) + (x * 2) = x * 10
	              ; A now holds the pixel offset (0, 10, 20, 30, or 40)

	; --- 3. COMBINE PAGE AND OFFSET ---
	ora b         ; Bitwise OR the offset with the Page mask stored in B
	mov b, a ; [B] The formatted PP0OOOOO

.waitLcd:
	in PortLcdStat
	rlc ; shift busy bit out into carry bit
	jc .waitLcd ; If cary set, LCD is busy, so keep looping

	mov a, b
	out PortLcdCmd ; Set page and offset

	pop b
	ret

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

VirtualPadDown:    ds 1
VirtualPadPressed: ds 1

VariablesEnd:

; Align level to 256 offset
; This eneables us to translate easily between
; Y coordinates and the 3 least significant
; bits of the tile address.
Level equ ($ + 0xff) & 0xff00
LevelEnd equ Level + 8*24

UndoEntryCount equ Level + 0x100 - 2 ; Just to have two distinctive states that alternate every move so we know how many tile changes constitute a single undo event.
UndoBufferAt equ Level + 0x100 - 1
UndoBuffer equ Level + 0x100
UndoBufferEnd equ UndoBuffer + 0x100

	assert UndoBufferEnd < ProgramLimitAddr
	; assert Level - VariablesEnd < 36
