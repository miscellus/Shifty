#ifndef VM8085_H
#define VM8085_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool cy, p, ac, z, s;
} Flags;

typedef struct Vm_8085 Vm_8085;

typedef void (* Vm_8085_Memory_Cb)(Vm_8085 *vm, uint16_t address, bool write, uint8_t *in_out_data);
typedef bool (* Vm_8085_Io_Cb)(Vm_8085* vm, uint8_t port, bool is_out, uint8_t *in_out_data);
typedef void (* Vm_8085_Loop_Cb)(Vm_8085* vm);

struct Vm_8085 {
    Flags flags;

    uint8_t a;
    uint8_t b, c;
    uint8_t d, e;
    uint8_t h, l;

    uint16_t sp;
    uint16_t pc;

    bool halt;
    bool interrupts_enabled;
    bool ei_delay_active;

    // --- New Hardware Interrupt Pins & Masks ---
    bool trap_asserted; // Highest priority (NMI)

    bool rst75_latch;   // Priority 2 (Edge-triggered, requires a latch)
    bool rst75_mask;

    bool rst65_asserted; // Priority 3 (Level-triggered)
    bool rst65_mask;

    bool rst55_asserted; // Priority 4 (Level-triggered)
    bool rst55_mask;

    // --- Existing INTR state (Priority 5) ---
    bool intr_asserted;
    uint8_t intr_vector_opcode;
    uint16_t intr_call_address;

    Vm_8085_Memory_Cb mem_cb;
    Vm_8085_Io_Cb io_cb;
    Vm_8085_Loop_Cb loop_cb;
    void* user_data;

    uint64_t total_t_states;
};

uint32_t vm8085_step(Vm_8085 *vm);
uint32_t vm8085_run(Vm_8085 *vm, uint32_t t_states_goal);


#ifdef __cplusplus
}
#endif

#endif // VM8085_H








///////////////////////////////////////////////////////////////////////////
#ifdef VM8085_IMPLEMENTATION
///////////////////////////////////////////////////////////////////////////

#include <stdint.h>
#include <stdbool.h>

#ifdef TARGET_WEB
#define assert(...)
#else
#include <assert.h>
#endif

// The 4 main opcode groups (Bits 6-7)
typedef uint8_t InstructionGroup;
enum {
    GRP_CTRL_MEM = 0, // Control, Immediate, and Direct Memory instructions
    GRP_MOV_HLT  = 1, // Register-to-Register MOVs and HLT
    GRP_ALU      = 2, // Register/Memory ALU Operations
    GRP_BRANCH   = 3  // Jumps, Calls, Returns, Stack, and Immediates
};

// 8-bit Registers (Bits 0-2 or 3-5)
typedef struct { uint8_t v; } Reg8;
enum {
    REG_B = 0,
    REG_C = 1,
    REG_D = 2,
    REG_E = 3,
    REG_H = 4,
    REG_L = 5,
    REG_M = 6, // Memory pseudo-register (HL pointer)
    REG_A = 7  // Accumulator
};

// 16-bit Register Pairs (Bits 4-5)
typedef struct { uint8_t v; } Reg16;
#define RP_BC     ((Reg16){0})
#define RP_DE     ((Reg16){1})
#define RP_HL     ((Reg16){2})
#define RP_SP_PSW ((Reg16){3}) // Stack Pointer (or Program Status Word for PUSH/POP)

// ALU Operations (Bits 3-5 in Group 2 & Group 3 Immediates)
typedef struct { uint8_t v; } AluOp;
enum {
    ALU_ADD = 0,
    ALU_ADC = 1,
    ALU_SUB = 2,
    ALU_SBB = 3,
    ALU_ANA = 4,
    ALU_XRA = 5,
    ALU_ORA = 6,
    ALU_CMP = 7
};

typedef struct { uint8_t v; } AccCyOp;
enum {
    ACC_CY_RLC = 0,
    ACC_CY_RRC = 1,
    ACC_CY_RAL = 2,
    ACC_CY_RAR = 3,
    ACC_CY_DAA = 4,
    ACC_CY_CMA = 5,
    ACC_CY_STC = 6,
    ACC_CY_CMC = 7
};

// Branch Conditions (Bits 3-5 in Group 3)
typedef struct { uint8_t v; } ConditionKind;
enum {
    COND_NZ = 0, // Not Zero
    COND_Z  = 1, // Zero
    COND_NC = 2, // No Carry
    COND_C  = 3, // Carry
    COND_PO = 4, // Parity Odd
    COND_PE = 5, // Parity Even
    COND_P  = 6, // Plus (Positive)
    COND_M  = 7  // Minus (Negative)
};

// Z-Categories for Group 0 (Bits 0-2)
typedef uint8_t Group0Op;
enum {
    G0_MISC_CTRL    = 0, // NOP, RIM, SIM
    G0_LXI_DAD      = 1, // LXI, DAD
    G0_LD_ST_A      = 2, // STAX, LDAX, SHLD, LHLD, STA, LDA
    G0_INC_DEC_16   = 3, // INX, DCX
    G0_INC_8        = 4, // INR
    G0_DEC_8        = 5, // DCR
    G0_MVI          = 6, // MVI
    G0_ACC_CTRL     = 7  // RLC, RRC, RAL, RAR, DAA, CMA, STC, CMC
};

// Z-Categories for Group 3 (Bits 0-2)
typedef uint8_t Group3Op;
enum {
    G3_RET_COND     = 0, // Rcc
    G3_POP_RET_MISC = 1, // POP, RET, PCHL, SPHL
    G3_JMP_COND     = 2, // Jcc
    G3_JMP_MISC     = 3, // JMP, IN, OUT, XTHL, XCHG, DI, EI
    G3_CALL_COND    = 4, // Ccc
    G3_PUSH_CALL    = 5, // PUSH, CALL
    G3_ALU_IMM      = 6, // ADI, ACI, SUI, SBI, ANI, XRI, ORI, CPI
    G3_RST          = 7  // RST n
};

// Specific static opcodes for clarity
typedef uint8_t SpecialOpcode;
enum {
    OP_NOP  = 0x00,
    OP_RIM  = 0x20,
    OP_SIM  = 0x30,
    OP_HLT  = 0x76,
    OP_EI   = 0xFB,
    OP_CALL = 0xCD
};

// Masking constants for decoding RST hardware interrupts
typedef uint8_t RstVectorMask;
enum {
    RST_BASE_OPCODE = 0xC7, // 11000111 in binary
    RST_OPCODE_MASK = 0xC7, // Mask to check if bits 6,7 and 0,1,2 match RST pattern
    RST_ADDR_MASK   = 0x38  // 00111000 (Extracts 'n * 8' directly from the opcode)
};

// Execution timing (T-states) for hardware interrupt acknowledgment
typedef uint32_t IntrAckTiming;
enum {
    T_STATES_INTR_ACK_RST  = 12, // Standard RST acknowledgment timing
    T_STATES_INTR_ACK_CALL = 18  // 3-byte CALL acknowledgment timing
};

enum {
    FLAG_CY = (1 << 0),
    FLAG_P  = (1 << 2),
    FLAG_AC = (1 << 4),
    FLAG_Z  = (1 << 6),
    FLAG_S  = (1 << 7),
};

static inline uint8_t flags_pack(Flags flags) {
    uint8_t packed_flags = 0;
    if (flags.cy) packed_flags |= FLAG_CY;
    if (flags.p)  packed_flags |= FLAG_P;
    if (flags.ac) packed_flags |= FLAG_AC;
    if (flags.z)  packed_flags |= FLAG_Z;
    if (flags.s)  packed_flags |= FLAG_S;
    return packed_flags;
}

static inline Flags flags_unpack(uint8_t packed_flags) {
    Flags flags = {0};
    flags.cy = (packed_flags & FLAG_CY) != 0;
    flags.p  = (packed_flags & FLAG_P)  != 0;
    flags.ac = (packed_flags & FLAG_AC) != 0;
    flags.z  = (packed_flags & FLAG_Z)  != 0;
    flags.s  = (packed_flags & FLAG_S)  != 0;
    return flags;
}

//
// Memory & Hardware Abstractions
//

static inline uint8_t mem_read_byte(Vm_8085* vm, uint16_t address) {
    uint8_t data = 0xff;
    vm->mem_cb(vm, address, false, &data);
    return data;
}

static inline void mem_write_byte(Vm_8085* vm, uint16_t address, uint8_t data) {
    vm->mem_cb(vm, address, true, &data);
}

static inline uint16_t mem_read_word(Vm_8085 *vm, uint16_t address) {
    uint16_t lo = mem_read_byte(vm, address);
    uint16_t hi = mem_read_byte(vm, address + 1);
    return (hi << 8) | lo;
}

static inline void mem_write_word(Vm_8085* vm, uint16_t address, uint16_t val) {
    mem_write_byte(vm, address, (uint8_t)val);
    mem_write_byte(vm, address + 1, (uint8_t)(val >> 8));
}

static inline uint8_t fetch_byte(Vm_8085 *vm) {
    uint8_t result = mem_read_byte(vm, vm->pc);
    vm->pc += 1;
    return result;
}

static inline uint16_t fetch_word(Vm_8085 *vm) {
    uint16_t result = mem_read_word(vm, vm->pc);
    vm->pc += 2;
    return result;
}

static inline void stack_push(Vm_8085 *vm, uint16_t val) {
    vm->sp -= 2;
    mem_write_word(vm, vm->sp, val);
}

static inline uint16_t stack_pop(Vm_8085 *vm) {
    uint16_t val = mem_read_word(vm, vm->sp);
    vm->sp += 2;
    return val;
}

//
// Register & Flag Management
//

static inline void flags_update_szp(Vm_8085 *vm, uint8_t result) {
    vm->flags.z = result == 0;
    vm->flags.s = (result & 0x80) != 0;

    // Bitwise parity check
    result ^= result >> 4;
    result ^= result >> 2;
    result ^= result >> 1;
    vm->flags.p = (result & 1) != 0;
}

static uint8_t reg8_read(Vm_8085 *vm, Reg8 reg_index) {
    switch (reg_index.v) {
        case 0: return vm->b;
        case 1: return vm->c;
        case 2: return vm->d;
        case 3: return vm->e;
        case 4: return vm->h;
        case 5: return vm->l;
        case 6: return mem_read_byte(vm, (vm->h << 8) | vm->l);
        case 7: return vm->a;
        default: assert(0 && "Invalid reg8");
    }
    return 0;
}

static void reg8_write(Vm_8085 *vm, Reg8 reg_index, uint8_t val) {
    switch (reg_index.v) {
        case 0: vm->b = val; break;
        case 1: vm->c = val; break;
        case 2: vm->d = val; break;
        case 3: vm->e = val; break;
        case 4: vm->h = val; break;
        case 5: vm->l = val; break;
        case 6: mem_write_byte(vm, (vm->h << 8) | vm->l, val); break;
        case 7: vm->a = val; break;
        default: assert(0 && "Invalid reg8");
    }
}

static uint16_t reg16_read(Vm_8085 *vm, Reg16 rp_index, bool is_psw) {
    switch (rp_index.v) {
        case 0: return (vm->b << 8) | vm->c;
        case 1: return (vm->d << 8) | vm->e;
        case 2: return (vm->h << 8) | vm->l;
        case 3: return is_psw ? ((vm->a << 8) | flags_pack(vm->flags)) : vm->sp;
        default: assert(0 && "Invalid reg16");
    }
    return 0;
}

static void reg16_write(Vm_8085 *vm, Reg16 rp_index, uint16_t val, bool is_psw) {
    uint8_t hi = (uint8_t)(val >> 8);
    uint8_t lo = (uint8_t)val;
    switch (rp_index.v) {
        case 0: vm->b = hi; vm->c = lo; break;
        case 1: vm->d = hi; vm->e = lo; break;
        case 2: vm->h = hi; vm->l = lo; break;
        case 3:
            if (is_psw) {
                vm->a = hi;
                // 8085 HW enforcing: Bit 1 is 1, Bits 3/5 are 0
                vm->flags = flags_unpack((lo & 0xD7) | 0x02);
            }
            else {
                vm->sp = val;
            }
            break;
        default: assert(0 && "Invalid reg16");
    }
}

static inline uint8_t io_read(Vm_8085* vm, uint16_t address) {
    uint8_t data = 0xFF;

    if (vm->io_cb && vm->io_cb(vm, (uint8_t)address, /*is_write*/ false, &data)) {
        return data;
    }

    return 0xFF;
}

static inline void io_write(Vm_8085* vm, uint16_t address, uint8_t data) {
    if (vm->io_cb) {
        vm->io_cb(vm, (uint8_t)address, /*is_write*/ true, &data);
    }
}


//
// ALU Core
//

static bool check_condition(Vm_8085 *vm, ConditionKind cond) {
    switch (cond.v) {
        case COND_NZ: return !vm->flags.z;
        case COND_Z:  return  vm->flags.z;
        case COND_NC: return !vm->flags.cy;
        case COND_C:  return  vm->flags.cy;
        case COND_PO: return !vm->flags.p;
        case COND_PE: return  vm->flags.p;
        case COND_P:  return !vm->flags.s;
        case COND_M:  return  vm->flags.s;
    }
    return false;
}

static inline void alu_add(Vm_8085 *vm, uint8_t operand, uint8_t cy_in) {
    uint16_t res = vm->a + operand + cy_in;
    vm->flags.ac = ((vm->a & 0x0F) + (operand & 0x0F) + cy_in) > 0x0F;
    vm->flags.cy = res > 0xFF;
    vm->a = (uint8_t)res;
    flags_update_szp(vm, vm->a);
}

static inline void alu_sub(Vm_8085 *vm, uint8_t operand, uint8_t borrow_in, bool update_acc) {
    uint8_t inv_operand = ~operand;
    uint8_t carry_in = borrow_in ? 0 : 1;
    uint16_t internal_res = vm->a + inv_operand + carry_in;

    // The AC flag is the carry out of bit 3 from this internal addition
    vm->flags.ac = ((vm->a & 0x0F) + (inv_operand & 0x0F) + carry_in) > 0x0F;
    vm->flags.cy = !(internal_res > 0xFF); // True borrow is inverted internal carry

    if (update_acc) vm->a = (uint8_t)internal_res;
    flags_update_szp(vm, (uint8_t)internal_res);
}

static void execute_alu_op(Vm_8085 *vm, AluOp op, uint8_t operand) {
    switch (op.v) {
        case ALU_ADD: alu_add(vm, operand, 0); break;
        case ALU_ADC: alu_add(vm, operand, vm->flags.cy ? 1 : 0); break;
        case ALU_SUB: alu_sub(vm, operand, 0, true); break;
        case ALU_SBB: alu_sub(vm, operand, vm->flags.cy ? 1 : 0, true); break;
        case ALU_ANA:
            vm->a &= operand;
            vm->flags.cy = false;
            vm->flags.ac = true; // Documented Intel standard for ANA
            flags_update_szp(vm, vm->a);
            break;
        case ALU_XRA:
            vm->a ^= operand;
            vm->flags.cy = false;
            vm->flags.ac = false;
            flags_update_szp(vm, vm->a);
            break;
        case ALU_ORA:
            vm->a |= operand;
            vm->flags.cy = false;
            vm->flags.ac = false;
            flags_update_szp(vm, vm->a);
            break;
        case ALU_CMP: alu_sub(vm, operand, 0, false); break;
    }
}

static void execute_acc_op(Vm_8085 *vm, AccCyOp op_index) {
    uint8_t a = vm->a;
    switch (op_index.v) {
        case ACC_CY_RLC: {
            uint8_t bit7 = (a >> 7) & 1;
            vm->a = (a << 1) | bit7;
            vm->flags.cy = bit7;
            break;
        }
        case ACC_CY_RRC: {
            uint8_t bit0 = a & 1;
            vm->a = (a >> 1) | (bit0 << 7);
            vm->flags.cy = bit0 != 0;
            break;
        }
        case ACC_CY_RAL: {
            uint8_t cy = vm->flags.cy ? 1 : 0;
            vm->flags.cy = ((a >> 7) & 1) != 0;
            vm->a = (a << 1) | cy;
            break;
        }
        case ACC_CY_RAR: {
            uint8_t cy = vm->flags.cy ? 1 : 0;
            vm->flags.cy = (a & 1) != 0;
            vm->a = (a >> 1) | (cy << 7);
            break;
        }
        case ACC_CY_DAA: {
            uint16_t res = a;
            uint8_t correction = 0;
            if ((a & 0x0F) > 9 || vm->flags.ac) {
                correction |= 0x06;
                vm->flags.ac = true;
            } else {
                vm->flags.ac = false;
            }
            if (a > 0x99 || vm->flags.cy) {
                correction |= 0x60;
                vm->flags.cy = true;
            }
            res += correction;
            vm->a = (uint8_t)res;
            flags_update_szp(vm, vm->a);
            break;
        }
        case ACC_CY_CMA: {
            vm->a = ~a;
            break;
        }
        case ACC_CY_STC: {
            vm->flags.cy = true;
            break;
        }
        case ACC_CY_CMC: {
            vm->flags.cy = !vm->flags.cy;
            break;
        }
    }
}

static void op_sim(Vm_8085 *vm) {
    uint8_t a = vm->a;

    // 1. Update Interrupt Masks
    // Only apply the new masks if the Mask Set Enable (MSE) bit 3 is high
    if (a & 0x08) {
        vm->rst55_mask = (a & 0x01) != 0; // Bit 0
        vm->rst65_mask = (a & 0x02) != 0; // Bit 1
        vm->rst75_mask = (a & 0x04) != 0; // Bit 2
    }

    // 2. Reset RST 7.5 Latch
    // If the Reset R7.5 bit 4 is high, clear the pending edge-triggered latch
    if (a & 0x10) {
        vm->rst75_latch = false;
    }

    // 3. Serial Output Enable (SOE)
    // If SOE (bit 6) is high, process the Serial Output Data (SOD, bit 7)
    if (a & 0x40) {
        bool sod_bit = (a & 0x80) != 0;
        (void)sod_bit;

        // Example hook if you plan to support the SOD pin
        // if (vm->serial_out_cb) vm->serial_out_cb(vm, sod_bit);
    }
}

static void op_rim(Vm_8085 *vm) {
    uint8_t result = 0;

    // 1. Current Mask Status (Bits 0-2)
    if (vm->rst55_mask) result |= 0x01;
    if (vm->rst65_mask) result |= 0x02;
    if (vm->rst75_mask) result |= 0x04;

    // 2. Global Interrupt Enable Status (Bit 3)
    // Represents the internal IE flip-flop state
    if (vm->interrupts_enabled) {
        result |= 0x08;
    }

    // 3. Pending Interrupt Status (Bits 4-6)
    // Reflects the physical pins and latches before the CPU acknowledges them
    if (vm->rst55_asserted) result |= 0x10; // Bit 4
    if (vm->rst65_asserted) result |= 0x20; // Bit 5
    if (vm->rst75_latch)    result |= 0x40; // Bit 6

    // 4. Serial Input Data (SID) - Bit 7
    // Read the physical SID pin state if your emulator supports it
    // bool sid_bit = vm->serial_in_cb ? vm->serial_in_cb(vm) : false;
    // if (sid_bit) result |= 0x80;

    // Finally, load the constructed status byte into the Accumulator
    vm->a = result;
}

/* -------------------------------------------------------------------------- */
/* Main Core Cycle Hook                                                       */
/* -------------------------------------------------------------------------- */


// Define the hardware vectors for clarity
#define VECTOR_TRAP  0x0024 // RST 4.5
#define VECTOR_RST75 0x003C
#define VECTOR_RST65 0x0034
#define VECTOR_RST55 0x002C

// Added for INTR parsing
#define RST_OPCODE_MASK 0xC7
#define RST_BASE_OPCODE 0xC7
#define RST_ADDR_MASK   0x38

static uint32_t service_interrupts(Vm_8085 *vm) {
    // 1. TRAP (NMI) - Highest Priority
    if (vm->trap_asserted) {
        vm->trap_asserted = false; // Acknowledge
        vm->interrupts_enabled = false;
        vm->halt = false;

        stack_push(vm, vm->pc);
        vm->pc = VECTOR_TRAP;
        return 12; // M1=6, M2=3, M3=3
    }

    // Guard check for all maskable interrupts
    if (!vm->interrupts_enabled || vm->ei_delay_active) {
        return 0; // Proceed with normal execution
    }

    // 2. RST 7.5 - Edge triggered (latched)
    if (vm->rst75_latch && !vm->rst75_mask) {
        vm->rst75_latch = false; // Clear latch on service
        vm->interrupts_enabled = false;
        vm->halt = false;

        stack_push(vm, vm->pc);
        vm->pc = VECTOR_RST75;
        return 12;
    }

    // 3. RST 6.5 - Level triggered
    if (vm->rst65_asserted && !vm->rst65_mask) {
        vm->interrupts_enabled = false;
        vm->halt = false;

        stack_push(vm, vm->pc);
        vm->pc = VECTOR_RST65;
        return 12;
    }

    // 4. RST 5.5 - Level triggered
    if (vm->rst55_asserted && !vm->rst55_mask) {
        vm->interrupts_enabled = false;
        vm->halt = false;

        stack_push(vm, vm->pc);
        vm->pc = VECTOR_RST55;
        return 12;
    }

    // 5. INTR - Lowest Priority
    if (vm->intr_asserted) {
        // Do not clear vm->intr_asserted here! INTR is level-triggered.
        // The external device should drop the line once it receives INTA.
        vm->interrupts_enabled = false;
        vm->halt = false;

        uint8_t vector_op = vm->intr_vector_opcode;

        // Handle standard 1-byte RST n instructions (e.g., 0xFF = RST 7)
        if ((vector_op & RST_OPCODE_MASK) == RST_BASE_OPCODE) {
            stack_push(vm, vm->pc);
            vm->pc = (vector_op & RST_ADDR_MASK);
            return 12; // M1(INTA)=6, M2(MW)=3, M3(MW)=3
        }

        // Handle 3-byte hardware CALL instruction (0xCD)
        else if (vector_op == 0xCD) {
            stack_push(vm, vm->pc);

            // To fetch the next two bytes, the CPU issues two more INTA pulses.
            // Since we bypass the normal memory fetch (PC is not incremented),
            // you must provide a way for the peripheral to supply the address.

            // OPTION A: Using a callback to the peripheral bus
            // uint8_t addr_lo = vm->inta_read_cb(vm);
            // uint8_t addr_hi = vm->inta_read_cb(vm);
            // vm->pc = (addr_hi << 8) | addr_lo;

            // OPTION B: Assuming the peripheral pre-loaded the full 16-bit address
            // into the VM struct alongside the opcode (Fastest for WebAssembly)
            vm->pc = vm->intr_call_address;

            return 18; // M1(INTA)=6, M2(INTA)=3, M3(INTA)=3, M4(MW)=3, M5(MW)=3
        }

        // Failsafe: If a peripheral injects an invalid or unsupported opcode,
        // clear the interrupt state to prevent a deadlock and resume normal execution.
        else {
            vm->intr_asserted = false;
            vm->interrupts_enabled = true; // Re-enable to avoid lock-out
            return 0;
        }
    }

    return 0; // No interrupts firing
}

uint32_t vm8085_step(Vm_8085 *vm) {
    uint32_t t_states = service_interrupts(vm);

    if (t_states) {
        // We return early because an interrupt was handled instead of normal instruction flow.
        return t_states;
    }

    if (vm->halt) {
        return 4; // Return minimum execution state timing if halted
    }

    // CLEAR THE SHADOW: By default, the delay shadow expires at the start of the next instruction.
    vm->ei_delay_active = false;

    uint8_t opcode = fetch_byte(vm);

    // Unpack the raw opcode into our distinct, aliased byte slots
    struct {
        uint8_t x;
        union { uint8_t y; Reg8 dest; AluOp alu_op; AccCyOp acc_cy_op; ConditionKind cond; };
        union { uint8_t z; Reg8 src; };
        Reg16 reg16;
    } op = {
        .x     = (opcode >> 6) & 0x03,
        .y     = (opcode >> 3) & 0x07,
        .z     =  opcode       & 0x07,
        .reg16 = {(opcode >> 4) & 0x03},
    };

    switch (op.x) {
        // ---------------------------------------------------------------------
        // GROUP 0: Immediates, Increment/Decrement, Direct Addressing
        // ---------------------------------------------------------------------
        case GRP_CTRL_MEM:
            switch (op.z) {
                case G0_MISC_CTRL:
                    if      (opcode == OP_NOP) { t_states = 4; }
                    else if (opcode == OP_RIM) { op_rim(vm); t_states = 4; }
                    else if (opcode == OP_SIM) { op_sim(vm); t_states = 4; }
                    break;
                case G0_LXI_DAD:
                    if ((op.y & 1) == 0) { // LXI
                        reg16_write(vm, op.reg16, fetch_word(vm), false);
                        t_states = 10;
                    } else {               // DAD
                        uint16_t val = reg16_read(vm, op.reg16, false);
                        uint16_t hl  = reg16_read(vm, RP_HL, false);
                        uint32_t res = (uint32_t)hl + val;
                        reg16_write(vm, RP_HL, (uint16_t)res, false);
                        vm->flags.cy = res > 0xFFFF;
                        t_states = 10;
                    }
                    break;
                case G0_LD_ST_A:
                    switch (op.y) {
                        case 0: /* STAX B */ mem_write_byte(vm, reg16_read(vm, RP_BC, false), vm->a); t_states = 7; break;
                        case 1: /* LDAX B */ vm->a = mem_read_byte(vm, reg16_read(vm, RP_BC, false)); t_states = 7; break;
                        case 2: /* STAX D */ mem_write_byte(vm, reg16_read(vm, RP_DE, false), vm->a); t_states = 7; break;
                        case 3: /* LDAX D */ vm->a = mem_read_byte(vm, reg16_read(vm, RP_DE, false)); t_states = 7; break;
                        case 4: { /* SHLD   */ uint16_t addr = fetch_word(vm); mem_write_word(vm, addr, reg16_read(vm, RP_HL, false)); t_states = 16; break; }
                        case 5: { /* LHLD   */ uint16_t addr = fetch_word(vm); reg16_write(vm, RP_HL, mem_read_word(vm, addr), false); t_states = 16; break; }
                        case 6: { /* STA    */ uint16_t addr = fetch_word(vm); mem_write_byte(vm, addr, vm->a); t_states = 13; break; }
                        case 7: { /* LDA    */ uint16_t addr = fetch_word(vm); vm->a = mem_read_byte(vm, addr); t_states = 13; break; }
                    }
                    break;
                case G0_INC_DEC_16:
                    if ((op.y & 1) == 0) { // INX
                        reg16_write(vm, op.reg16, reg16_read(vm, op.reg16, false) + 1, false);
                    } else {               // DCX
                        reg16_write(vm, op.reg16, reg16_read(vm, op.reg16, false) - 1, false);
                    }
                    t_states = 6;
                    break;
                case G0_INC_8: {
                    uint8_t orig = reg8_read(vm, op.dest);
                    uint8_t res = orig + 1;
                    reg8_write(vm, op.dest, res);
                    vm->flags.ac = (orig & 0x0F) == 0x0F;
                    flags_update_szp(vm, res);
                    t_states = (op.dest.v == REG_M) ? 10 : 4;
                    break;
                }
                case G0_DEC_8: {
                    uint8_t orig = reg8_read(vm, op.dest);
                    uint8_t res = orig - 1;
                    reg8_write(vm, op.dest, res);
                    vm->flags.ac = (orig & 0x0F) == 0x00;
                    flags_update_szp(vm, res);
                    t_states = (op.dest.v == REG_M) ? 10 : 4;
                    break;
                }
                case G0_MVI:
                    reg8_write(vm, op.dest, fetch_byte(vm));
                    t_states = (op.dest.v == REG_M) ? 10 : 7;
                    break;
                case G0_ACC_CTRL:
                    execute_acc_op(vm, op.acc_cy_op);
                    t_states = 4;
                    break;
            }
            break;

        // ---------------------------------------------------------------------
        // GROUP 1: MOV dest, src and HLT
        // ---------------------------------------------------------------------
        case GRP_MOV_HLT:
            if (opcode == OP_HLT) {
                vm->halt = true;
                t_states = 5;
            } else {
                reg8_write(vm, op.dest, reg8_read(vm, op.src));
                t_states = (op.dest.v == REG_M || op.src.v == REG_M) ? 7 : 4;
            }
            break;

        // ---------------------------------------------------------------------
        // GROUP 2: ALU Operations (ADD, ADC, SUB, SBB, ANA, XRA, ORA, CMP)
        // ---------------------------------------------------------------------
        case GRP_ALU:
            execute_alu_op(vm, op.alu_op, reg8_read(vm, op.src));
            t_states = (op.src.v == REG_M) ? 7 : 4;
            break;

        // ---------------------------------------------------------------------
        // GROUP 3: Branches, Calls, Returns, Stack, Interrupts
        // ---------------------------------------------------------------------
        case GRP_BRANCH:
            switch (op.z) {
                case G3_RET_COND:
                    if (check_condition(vm, op.cond)) {
                        vm->pc = stack_pop(vm);
                        t_states = 12;
                    } else {
                        t_states = 6;
                    }
                    break;
                case G3_POP_RET_MISC:
                    if ((op.y & 1) == 0) { // POP rp
                        reg16_write(vm, op.reg16, stack_pop(vm), true);
                        t_states = 10;
                    } else {
                        switch (op.y) {
                            case 1: vm->pc = stack_pop(vm); t_states = 10; break; // RET
                            case 5: vm->pc = reg16_read(vm, RP_HL, false); t_states = 6; break; // PCHL
                            case 7: reg16_write(vm, RP_SP_PSW, reg16_read(vm, RP_HL, false), false); t_states = 6; break; // SPHL
                        }
                    }
                    break;
                case G3_JMP_COND:
                    {
                        uint16_t j_addr = fetch_word(vm);
                        if (check_condition(vm, op.cond)) {
                            vm->pc = j_addr;
                            t_states = 10;
                        } else {
                            t_states = 7;
                        }
                    }
                    break;
                case G3_JMP_MISC:
                    switch (op.y) {
                        case 0: vm->pc = fetch_word(vm); t_states = 10; break; // JMP
                        case 2: { // OUT
                            uint8_t port = fetch_byte(vm);
                            io_write(vm, port, vm->a);
                            t_states = 10;
                            break;
                        }
                        case 3: { // IN
                            uint8_t port = fetch_byte(vm);
                            vm->a = io_read(vm, port);
                            t_states = 10;
                            break;
                        }
                        case 4: { // XTHL
                            uint16_t temp_stack = mem_read_word(vm, vm->sp);
                            uint16_t hl_val = reg16_read(vm, RP_HL, false);
                            mem_write_word(vm, vm->sp, hl_val);
                            reg16_write(vm, RP_HL, temp_stack, false);
                            t_states = 16;
                            break;
                        }
                        case 5: { // XCHG
                            uint16_t de_val = reg16_read(vm, RP_DE, false);
                            uint16_t hl_val = reg16_read(vm, RP_HL, false);
                            reg16_write(vm, RP_DE, hl_val, false);
                            reg16_write(vm, RP_HL, de_val, false);
                            t_states = 4;
                            break;
                        }
                        case 6: // DI
                            vm->interrupts_enabled = false;
                            t_states = 4;
                            break;
                        case 7: // EI
                            vm->interrupts_enabled = true;
                            vm->ei_delay_active = true;
                            t_states = 4;
                            break;
                    }
                    break;
                case G3_CALL_COND:
                    {
                        uint16_t c_addr = fetch_word(vm);
                        if (check_condition(vm, op.cond)) {
                            stack_push(vm, vm->pc);
                            vm->pc = c_addr;
                            t_states = 18;
                        } else {
                            t_states = 9;
                        }
                    }
                    break;
                case G3_PUSH_CALL:
                    if ((op.y & 1) == 0) { // PUSH rp
                        stack_push(vm, reg16_read(vm, op.reg16, true));
                        t_states = 12;
                    } else if (op.y == 1) { // CALL
                        uint16_t call_addr = fetch_word(vm);
                        stack_push(vm, vm->pc);
                        vm->pc = call_addr;
                        t_states = 18;
                    }
                    break;
                case G3_ALU_IMM:
                    execute_alu_op(vm, op.alu_op, fetch_byte(vm));
                    t_states = 7;
                    break;
                case G3_RST:
                    stack_push(vm, vm->pc);
                    vm->pc = op.y * 8;
                    t_states = 12;
                    break;
            }
            break;
    }

    assert(t_states != 0);
    return t_states;
}

uint32_t vm8085_run(Vm_8085 *vm, uint32_t t_state_goal) {
    uint32_t t_state_sum = 0;

    while (t_state_sum < t_state_goal) {
        if (vm->loop_cb) vm->loop_cb(vm);
        uint32_t t_states = vm8085_step(vm);
        t_state_sum += t_states;
        vm->total_t_states += t_states;
    }

    return t_state_sum - t_state_goal;
}

#endif // VM8085_IMPLEMENTATION