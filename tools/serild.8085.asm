INZ232 equ 0x6f58
RCVX equ 0x6dc2
RV232C equ 0x6dd3

	org 0xF380 - 256
Start:
	mvi h, 5 ; 1200 baud
	mvi l, 0b00011110 ; 8 bit data length, no parity, 1 stop bits
	mvi b, 0
	mvi c, 0o377
	call INZ232

	call RxByte
	mov l, a        ; Store Dest Low in E
	call RxByte
	mov h, a        ; Store Dest High in D. (DE now holds Dest Address)

	; 1. Read 16-bit Length (Little Endian: Low byte, then High byte)
	call RxByte
	mov c, a        ; Store Length Low in C
	call RxByte
	mov b, a        ; Store Length High in B. (BC now holds Length)

	; Skip Entry point
	call RxByte
	call RxByte

ReceiveLoop:
	; 3. Check if Length (BC) is 0
	mov a, b
	ora c           ; Logical OR of B and C. Zero flag sets if both are 0
	jz Done         ; If BC == 0, we are finished

	; 4. Read data byte and write to memory
	call RxByte
	mov m, a        ; Write byte to memory at address [HL]

	inx h           ; Increment destination address
	dcx b           ; Decrement remaining length count
	jmp ReceiveLoop ; Repeat until BC is 0

Done:
	ret

RxByte:
	call RCVX
	jz RxByte
	call RV232C
	ret

	assert $ <= 0xF380