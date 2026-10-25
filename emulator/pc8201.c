#include <stddef.h>
#include <stdbool.h>

#define VM8085_IMPLEMENTATION
#include "vm8085.h"

#ifdef TARGET_WEB
#define memset __builtin_memset
#define memcpy __builtin_memcpy
#define die(...) ((void)0)
#elif TARGET_DESKTOP
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define die(...) do { fprintf(stderr, __VA_ARGS__); exit(1); } while(0)
#endif

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 64
#define CPU_CLOCK_FREQ 2457600.0 // 2.4576 MHz

#ifdef TARGET_WEB
#define DECL_EXPORT __attribute__((visibility("default")))
#else
#define DECL_EXPORT
#endif


#define PC8201_PORTS(X) \
    X(PortExtVidA,    0x70) \
    X(PortExtVidB,    0x71) \
    X(PortExtVidC,    0x72) \
    X(PortExtVidCmd,  0x73) \
    X(PortRom128KHi,  0x80) \
    X(PortRom128KLo,  0x84) \
    X(PortRom128KMid, 0x88) \
    X(PortRom128KRead,0x8C) \
    X(PortIoCtl,      0x90) \
    X(PortBankStatus, 0xA0) \
    X(PortBankSel,    0xA1) \
    X(Port81C55Cmd,   0xB8) \
    X(Port81C55A,     0xB9) \
    X(Port81C55B,     0xBA) \
    X(Port81C55C,     0xBB) \
    X(PortTimerLow,   0xBC) \
    X(PortTimerHigh,  0xBD) \
    X(PortSerialData, 0xC8) \
    X(PortSerialCtl,  0xD8) \
    X(PortKeyIn,      0xE8) \
    X(PortLcdCmd,     0xFE) \
    X(PortLcdData,    0xFF) \
    /*end*/

typedef enum {
#define X(name, val) name = val,
    PC8201_PORTS(X)
#undef X
} Pc8201Port;

typedef struct {
    uint8_t bytes[50][4];
    uint8_t page; // 0..3
    uint8_t offset; // 0..49
    uint8_t start_page; // 0..3
    bool display_on;
    bool is_counter_mode_down;
    uint8_t output_register;

    uint64_t t_state_ready;
} Pc8201LcdDriver;

typedef struct {
    Pc8201LcdDriver drivers[10];
    uint16_t driver_select; // bit 0 selects driver 0, 1 selects driver 1, ... , 10 selects driver 10.
    bool on;
    // 32-bit RGBA buffer that JavaScript will read directly to draw to the HTML5 canvas
} Pc8201Lcd;

typedef struct {
    uint8_t matrix[9]; // Index 0-7 = PA0-PA7, Index 8 = PB0
    uint8_t strobe_a;  // State of Port 0xB9 (PA0-PA7)
    uint8_t strobe_b;  // State of Port 0xBA (PB0)
} Pc8201Keyboard;

typedef uint8_t Timer_Mode;
enum {
    // This-mode transmits a single-
    // square wave which the first
    // half of the number of count
    // is high and remaining 1s low.
    TIMER_ONE_SHOT_SQUARE = 0,

    // This mode continually transmits
    // a Mode 0 type square wave.
    TIMER_CONTINOUS_SQUARE = 1,

    // This mode transmits a L-pulse
    // (single pulse) during one
    // clock when finishing the
    // terminal count.
    TIMER_ONE_SHOT_PULSE = 2,

    // This mode continually transmits
    // a Mode 2 type pulse.
    TIMER_CONTINOUS_PULSE = 3,
};

enum { MAX_TIMER_COUNT = (1 << 14) - 1 };

typedef uint8_t BreakPointKind;
enum {
    BP_NONE = 0,
    BP_TEMP = 1,
    BP_PERM = 2,
};

typedef union {
    struct {
        uint16_t line_number;
        uint16_t file_index;
    } s;
    uint32_t v;
} DebuggerLoc;

typedef uint8_t SerialInterfaceSelection;
enum {
    SER_NOT_USED = 0,
    SER_SIO2 = 1,
    SER_SIO1 = 2,
    SER_RS232C = 3,
};

#define BANK_SIZE (32*1024)

typedef struct {
    uint8_t *base;
    bool writeable;
} MemPage;

typedef struct {
    uint8_t system_rom[BANK_SIZE];
    uint8_t option_rom[BANK_SIZE];
    uint8_t ram_banks[4][BANK_SIZE];
    MemPage pages[2];
    uint8_t low_address_select;
    uint8_t high_address_select;
} Memory;

static void memory_init(Memory *mem) {
    // Low page: System ROM (read-only)
    mem->pages[0].base = mem->system_rom;
    mem->pages[0].writeable = false;

    // High page: RAM Bank 0 / Bank 1 (writable)
    mem->pages[1].base = mem->ram_banks[0];
    mem->pages[1].writeable = true;
}

static void memory_bank_set(Memory *mem, uint8_t data) {
    uint8_t ladr = data & 0x03;
    uint8_t hadr = (data >> 2) & 0x03;

    mem->low_address_select = ladr;
    mem->high_address_select = hadr;

    switch (ladr) {
        case 0:  mem->pages[0] = (MemPage){mem->system_rom, false}; break;
        case 1:  mem->pages[0] = (MemPage){mem->option_rom, false}; break;
        case 2:  mem->pages[0] = (MemPage){mem->ram_banks[1], true}; break;
        case 3:  mem->pages[0] = (MemPage){mem->ram_banks[2], true}; break;
    }

    switch (hadr) {
        case 0:  mem->pages[1] = (MemPage){mem->ram_banks[0], true}; break;
        case 1:  mem->pages[1] = (MemPage){mem->ram_banks[0], true}; break; // HADR = 1 ("NOT USED")
        case 2:  mem->pages[1] = (MemPage){mem->ram_banks[1], true}; break;
        case 3:  mem->pages[1] = (MemPage){mem->ram_banks[2], true}; break;
    }
}

static inline uint8_t memory_read(Memory *mem, uint16_t address) {
    uint8_t idx = address >> 15;
    return mem->pages[idx].base[address & 0x7FFF];
}

static inline void memory_write(Memory *mem, uint16_t address, uint8_t data) {
    uint8_t idx = address >> 15;
    if (mem->pages[idx].writeable) {
        mem->pages[idx].base[address & 0x7FFF] = data;
    }
}

typedef struct {
    Vm_8085 cpu;
    Pc8201Lcd lcd;
    uint32_t canvas[SCREEN_WIDTH * SCREEN_HEIGHT];

    Memory mem;

    Pc8201Keyboard key;

    struct {

        // NOTE(jkk): I learned from reading service manual and tech ref that
        // the timer input is the synchronized to the CPU clock and the timer
        // output is determined by the mode.

        // The timer output is connected to the buzzer but gated by some lines
        // that are controlled by other registers in the 80C55.

        uint64_t time_next_period; // total T states
        uint16_t interval;
        uint16_t counter; // Only 14 bits
        Timer_Mode mode;
    } timer;

    uint8_t ports_prev[256];

    bool cassette_motor_on;
    bool clock_command_strobe;
    bool printer_strobe;
    SerialInterfaceSelection serial_interface_selection;

    bool buzzer_on;

    // Debugger support
    BreakPointKind breakpoints[65536];
    DebuggerLoc address_to_line[65536];
} Pc8201Machine;

// Globally instantiate our machine state
static Pc8201Machine machine;

void update_canvas_buffer(Pc8201Machine* mach) {
    Pc8201Lcd lcd = mach->lcd;
    uint32_t *canvas = mach->canvas;

    for (int i = 0; i < 10; ++i)
    {

        uint32_t driver_y = (i / 5) * 32;
        uint32_t driver_x = (i % 5) * 50;

        Pc8201LcdDriver *driv = &lcd.drivers[i];
        for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 50; ++col)
        {
            if (i % 5 == 4 && col >= 40) continue; // Clip the right-most driver

            uint8_t pix8 = driv->bytes[col][row];

            for (int pix_index = 0; pix_index < 8; ++pix_index)
            {
                uint32_t color = pix8 & (1 << pix_index)
                    ? 0xFF000000
                    : 0xFFFFFFFF;

                uint32_t x = driver_x + col;
                uint32_t y = driver_y + row*8 + pix_index;
                uint32_t offset = x + y * SCREEN_WIDTH;
                canvas[offset] = color;
            }
        }
    }
}

static const char* string_from_port(uint8_t port) {
    switch (port) {
#define X(name, val) case val: return #name;
        PC8201_PORTS(X)
#undef X
        default: return "UnknownPort";
    }
}

static void mem_cb(Vm_8085 *vm, uint16_t addr, bool is_write, uint8_t *in_out_data)
{
    Pc8201Machine *mach = (Pc8201Machine *)vm->user_data;

    if (is_write) {
        memory_write(&mach->mem, addr, *in_out_data);
    }
    else {
        *in_out_data = memory_read(&mach->mem, addr);
    }
}

static void loop_cb(Vm_8085 *vm) {
    Pc8201Machine *mach = (Pc8201Machine *)vm->user_data;

    // Calculate time from total_t_states
    while (mach->cpu.total_t_states >= mach->timer.time_next_period) {
        uint64_t t = mach->cpu.total_t_states - mach->timer.time_next_period;
        mach->timer.counter = t % mach->timer.interval;
        mach->timer.time_next_period += mach->timer.interval;

        // Trigger RST 7.5 interrupt edge latch
        vm->rst75_latch = true;
    }
}

#ifndef TARGET_WEB
#include <stdio.h>
#include <inttypes.h>
#else
#define fprintf(...) ((void)0)
#endif

static bool io_cb(Vm_8085 *vm, uint8_t port, bool is_write, uint8_t *in_out_data)
{
    Pc8201Machine *mach = (Pc8201Machine *)vm->user_data;
    Pc8201Lcd *lcd = &mach->lcd;
    uint8_t data = is_write ? *in_out_data : 0;
    uint8_t data_prev = mach->ports_prev[port];
    if (!is_write) *in_out_data = data_prev;

    bool handled = false;

    switch (port) {

    case PortIoCtl: {

        // TODO(jkk):
        // Reading/writing data with CMT is done via SID Pin ,SOD Pin of
        // CPU(80C85). And Motor is controlled by SCP (System Control
        // Port,^X90). The on-bit, Logical High, is represented by 2400Hz
        // wave (called MARK) and the off-bit, Logical Low, is 1200Hz
        // wave (called SPACE). So the Baud Rate of the CMT can be up to 1200
        // bps, bit per second. ( System ROM, ROM #0 Uses 600 bps to maintain
        // the compatibility with PC-8001A.)

        // 11.1.1
        //  Writing Operation.
        // While SOD is high, MARK is put out to MIC and TxC.
        // Otherwise, SPACE is put out. Refer the next illustration.

        // 11.1.2
        //  Reading Operation.
        // Input wave from EAR Pin' is performed to Square wave and
        // sent to SID Pin of 80C8S as shown olow. The input wave is
        // inverted on the way to SID Pin from EAR Pin.
        //  In reading
        // operation, the electric high/low. level has no meaning. The
        // pulse frequency indicate whether high or low of the data. The
        // frequency, 2400Hz means logical high ,and the frequency,
        // 1200Hz means low.

        if (is_write) {

            mach->cassette_motor_on          = !!(data & (1 << 3));
            mach->clock_command_strobe       = !!(data & (1 << 4));
            mach->printer_strobe             = !!(data & (1 << 5));
            mach->serial_interface_selection = (data >> 6) & 0x3;
            handled = true;
        }
        else {

        }
    } break;

    case PortBankStatus: { // 0xA0
        if (is_write) {

        }
        else /* read */ {

            *in_out_data = (
                mach->mem.low_address_select |
                (mach->mem.high_address_select << 2) |
                (mach->serial_interface_selection << 6));

            handled = true;
        }
    } break;

    case PortBankSel: { // 0xA1
        if (is_write) {
            memory_bank_set(&mach->mem, data);
        }
        handled = true;
    } break;

    case Port81C55A: {
        if (is_write) {
            mach->key.strobe_a = data;

            lcd->driver_select &= 0xff00;
            lcd->driver_select |= data;
        }
        handled = true;
    } break;
 
    case Port81C55B: {
        if (is_write) {
            mach->key.strobe_b = data; // Track keyboard strobe (PB0)

            lcd->driver_select &= 0x00ff;
            lcd->driver_select |= (data & 0x3) << 8;

            mach->buzzer_on = !!(data & (1 << 5));
        }
        else /* read */ {
            *in_out_data = (lcd->driver_select >> 8) & 0x3;
        }

        handled = true;
    } break;

    case Port81C55C: {
        if (is_write) {
            // TODO
        }
        else /* read */ {
            // Clear Bit 2 (Printer Busy = 0, meaning ready)
            // Adjust other bits as needed for CTS/DSR/etc.
            *in_out_data = 0xF8;
            handled = true;
        }
    } break;

    case Port81C55Cmd: {
        if (is_write) {
            // TODO
        }
        else /* read */ {
            *in_out_data = 0x89;
            handled = true;
        }
    } break;

    case PortTimerLow: {
        if (is_write) {
            mach->timer.interval &= 0xff00;
            mach->timer.interval |= data;
            handled = true;
        }
        else /* read */ {
            // TODO
        }
    } break;

    case PortTimerHigh: {
        if (is_write) {
            mach->timer.interval &= 0xC0ff;
            mach->timer.interval |= (((uint16_t)(data & 0x3f)) << 8);
            mach->timer.mode = (data >> 6) & 3;
            handled = true;
        }
        else /* read */ {
            // TODO
        }
    } break;

    //case PortLcdStat alias
    case PortLcdCmd : {
        uint8_t cmd = data;

        uint16_t select = lcd->driver_select;
        select &= -select; // Only allow 1 selected

        for (int i = 0; i < 10; ++i) {
            if (!(select & (1 << i))) continue;

            Pc8201LcdDriver *driv = &lcd->drivers[i];

            if (is_write) {
                if ((cmd & 0xfe) == 0x38) {
                    // Command: DISPLAY ON/OFF
                    driv->display_on = (cmd & 1) != 0;
                }
                else if ((cmd & 0x3f) == 0x3f) {
                    // Command: Start page offset
                    driv->start_page = (cmd >> 6) & 0x03;
                }
                else if ((cmd & 0xfe) == 0x32) {
                    // Command: Set up/down offset counter mode
                    driv->is_counter_mode_down = (cmd & 1) == 0; // 1 = Up Counter, 0 = Down Counter
                }
                else {
                    // Command: Set page and offset
                    driv->page = (cmd >> 6) & 0x3;
                    driv->offset = cmd & 0x3F;
                }

                driv->t_state_ready = mach->cpu.total_t_states + 8;
            }
            else /* read */ {
                bool busy = driv->t_state_ready > mach->cpu.total_t_states;

                *in_out_data = (
                    (busy ? (1 << 4) : 0) |
                    (driv->display_on ? (1 << 5) : 0) |
                    (driv->is_counter_mode_down ? 0 : (1 << 6)) |
                    (busy ? (1 << 7) : 0));
            }

            break;
        }

        handled = true;
    } break;

    case PortLcdData: {
        for (int i = 0; i < 10; ++i) {
            if (!(lcd->driver_select & (1u << i)))
                continue;

            Pc8201LcdDriver *driv = &lcd->drivers[i];
            bool valid_address = driv->offset < 50 && driv->page < 4;

            if (is_write) {
                if (valid_address) {
                    driv->bytes[driv->offset][(driv->page + driv->start_page) & 3] = data;
                    driv->t_state_ready = mach->cpu.total_t_states + 8;
                }
            } else {
                *in_out_data = driv->output_register;

                if (valid_address) {
                    driv->output_register = driv->bytes[driv->offset][(driv->page + driv->start_page) & 3];
                }
            }

            /* Advance after both reads and writes, including the dummy read. */
            if (driv->is_counter_mode_down)
                driv->offset = (driv->offset == 0 || driv->offset >= 50)
                    ? 49 : driv->offset - 1;
            else
                driv->offset = (driv->offset >= 49)
                    ? 0 : driv->offset + 1;
        }

        handled = true;
    } break;

    // Handle Keyboard Data Read (Port 0xE8)
    case PortKeyIn: {
        if (is_write) {
            // TODO
        }
        else /* read */ {
            uint8_t result = 0xFF; // Default to all keys released

            // Check Port A strobes (PA0 - PA7)
            for (int i = 0; i < 8; i++) {
                if (!(mach->key.strobe_a & (1 << i))) {
                    // Strobe is ACTIVE (0), merge in depressed keys (0) for this row
                    result &= mach->key.matrix[i];
                }
            }

            // Check Port B strobe (PB0 is bit 0)
            if (!(mach->key.strobe_b & 0x01)) {
                // Strobe is ACTIVE (0), merge in depressed keys for PB0 row
                result &= mach->key.matrix[8];
            }

            *in_out_data = result;
            handled = true;
        }
    } break;

    default: // TODO
    }

#define BINPAT "%c%c%c%c%c%c%c%c"
#define BYTE_TO_BIN(byte)  \
    ((byte) & 0x80 ? '1' : '0'), \
    ((byte) & 0x40 ? '1' : '0'), \
    ((byte) & 0x20 ? '1' : '0'), \
    ((byte) & 0x10 ? '1' : '0'), \
    ((byte) & 0x08 ? '1' : '0'), \
    ((byte) & 0x04 ? '1' : '0'), \
    ((byte) & 0x02 ? '1' : '0'), \
    ((byte) & 0x01 ? '1' : '0')

    if (!handled) {
        const char *portstr = string_from_port(port);
        uint64_t t = mach->cpu.total_t_states;
        if (!is_write) data = *in_out_data;
        fprintf(stderr, "@: %04X T: %"PRIu64" - IO %s port %02X (%15.20s), value = %02x ("BINPAT");\n", mach->cpu.pc, t, is_write ? "WRITE" : "READ ", port, portstr, data, BYTE_TO_BIN(data));
    }

    return handled;
}

#ifdef TARGET_DESKTOP
#include <stdlib.h>
#endif

DECL_EXPORT void reset_emulator(void) {
    memset(&machine, 0, sizeof(machine));

    for (int i = 0; i < 9; i++) {
        machine.key.matrix[i] = 0xFF;
    }
    machine.key.strobe_a = 0xFF;
    machine.key.strobe_b = 0xFF;

    machine.cpu.io_cb = io_cb;
    machine.cpu.mem_cb = mem_cb;
    machine.cpu.loop_cb = loop_cb;
    machine.cpu.user_data = (void *)&machine;

    memory_init(&machine.mem);

    machine.timer.interval = 16384;
}

DECL_EXPORT void load_co_file(uint8_t *co_file, int32_t co_file_len, bool insert_jump_at_address_zero) {
    if (co_file_len < 6) {
        die("A valid CO file must be at least as big as the 6 byte header.\n");
    }

    uint16_t program_base_addr  = co_file[0] | ((uint16_t)co_file[1] << 8);
    uint16_t program_length     = co_file[2] | ((uint16_t)co_file[3] << 8);
    uint16_t program_entry_addr = co_file[4] | ((uint16_t)co_file[5] << 8);
    uint8_t *program            = co_file + 6;

    if (co_file_len != 6 + program_length) {
        die("CO file length does not match file header.\n");
    }

    for (uint16_t i = 0; i < program_length; ++i) {
        memory_write(&machine.mem, program_base_addr + i, program[i]);
    }

    if (insert_jump_at_address_zero) {
        machine.mem.system_rom[0] = 0xCD; // CALL
        machine.mem.system_rom[1] = program_entry_addr & 0xff;
        machine.mem.system_rom[2] = (program_entry_addr >> 8) & 0xff;
        machine.mem.system_rom[3] = 0xC3; // JMP
        machine.mem.system_rom[4] = 0x0;
        machine.mem.system_rom[5] = 0x0;
        machine.cpu.sp = 0x9DE4; // Taken directly from the debugger in VirtualT.
    }
}

DECL_EXPORT void load_rom(uint8_t *rom_data, int32_t rom_size) {
    assert(rom_size == (1 << 15));
    memcpy(machine.mem.system_rom, rom_data, rom_size);
}

DECL_EXPORT void reset_emulator_with_co_file(uint8_t *co_file, int32_t co_file_len) {
    reset_emulator();
    load_co_file(co_file, co_file_len, true);
}

#ifdef TARGET_WEB
// The WebAssembly linker automatically provides the __heap_base symbol.
// It marks the end of statically allocated memory (like global variables)
// and represents the safe starting point for our dynamic heap.

extern unsigned char __heap_base;
const size_t WASM_PAGE_SIZE = 1 << 16;

DECL_EXPORT void* allocate(size_t size) {
    static uintptr_t bump_ptr = (uintptr_t)&__heap_base;

    if (size == 0) return NULL;

    size_t aligned_size = (size + 7) & ~7;
    uintptr_t next_bump_ptr = bump_ptr + aligned_size;

    // __builtin_wasm_memory_size(0) returns the current memory size in pages.
    // We multiply by WASM_PAGE_SIZE to get the exact byte boundary.
    // The '0' refers to the memory index (Wasm currently uses index 0).
    size_t current_mem_bytes = __builtin_wasm_memory_size(0) * WASM_PAGE_SIZE;

    // Check if our allocation exceeds the current memory boundary
    if (next_bump_ptr > current_mem_bytes) {
        // Calculate how many extra bytes we need
        size_t needed_bytes = next_bump_ptr - current_mem_bytes;

        // Convert needed bytes to pages, rounding up to the nearest whole page
        size_t pages_to_grow = (needed_bytes + WASM_PAGE_SIZE - 1) / WASM_PAGE_SIZE;

        // __builtin_wasm_memory_grow(0, pages) asks the host for more memory.
        // It returns the previous memory size in pages on success, or -1 on failure.
        if (__builtin_wasm_memory_grow(0, pages_to_grow) == (size_t)-1) {
            // The host refused to give us more memory (Out of Memory)
            return NULL;
        }
    }

    // Memory is guaranteed to be large enough now. Proceed with allocation.
    void* allocated_memory = (void*)bump_ptr;
    bump_ptr = next_bump_ptr;

    return allocated_memory;
}
#else
DECL_EXPORT void* allocate(size_t size) {
    return malloc(size);
}
#endif // TARGET_WEB

DECL_EXPORT uint32_t* get_canvas_buffer(void) {
    return machine.canvas;
}

DECL_EXPORT void set_breakpoint(uint32_t addr, int32_t is_temporary) {
    if (addr >= 65536) return;

    uint8_t *bp = &machine.breakpoints[(uint16_t)addr];

    if (*bp != BP_PERM) {
        *bp = is_temporary ? BP_TEMP : BP_PERM;
    }
}

DECL_EXPORT void clear_breakpoint(uint32_t addr) {
    if (addr >= 65536) return;

    machine.breakpoints[(uint16_t)addr] = BP_NONE;
}

DECL_EXPORT void clear_temporary_breakpoints(void) {
    for (uint32_t i = 0; i < sizeof(machine.breakpoints) / sizeof(machine.breakpoints[0]); ++i)
    {
        if (machine.breakpoints[i] == BP_TEMP) {
            machine.breakpoints[i] = BP_NONE;
        }
    }
}

DECL_EXPORT int32_t set_step_out_breakpoint(void) {
    if (machine.cpu.call_depth == 0) {
        return -1;
    }

    uint16_t ret_addr = machine.cpu.call_stack[machine.cpu.call_depth - 1];
    set_breakpoint(ret_addr, true);
    return ret_addr;
}

static bool breakpoint_hit(uint16_t addr) {

    BreakPointKind bp = machine.breakpoints[addr];

    if (bp == BP_NONE) {
        return false;
    }

    if (bp == BP_TEMP) {
        machine.breakpoints[addr] = BP_NONE;
        return true;
    }

    assert(bp == BP_PERM);
    return true;
}

DECL_EXPORT uint16_t step_into(void) {
    vm8085_run(&machine.cpu, 1);
    update_canvas_buffer(&machine);
    return machine.cpu.pc;
}

DECL_EXPORT uint16_t step_over(void) {
    uint16_t start_depth = machine.cpu.call_depth;
    do vm8085_run(&machine.cpu, 1); while (!breakpoint_hit(machine.cpu.pc) && (start_depth < machine.cpu.call_depth));

    update_canvas_buffer(&machine);
    return machine.cpu.pc;
}

DECL_EXPORT int32_t run(int32_t t_state_goal) {
    int32_t t_states_executed = 0;

    while (t_states_executed < t_state_goal) {
        // Stop execution immediately if we hit a breakpoint
        if (breakpoint_hit(machine.cpu.pc)) {
            update_canvas_buffer(&machine);
            return -1; // Special return value indicating execution paused
        }
        t_states_executed += vm8085_run(&machine.cpu, 1) + 1;
    }

    update_canvas_buffer(&machine);
    return t_states_executed - t_state_goal;

    // // Run a batch of instructions for this browser frame
    // int32_t t_states_over_goal = vm8085_run(&machine.cpu, t_state_goal);
    // // Update the visual buffer based on current state of 8085 RAM
    // update_canvas_buffer(&machine);
    // return t_states_over_goal;
}

DECL_EXPORT void set_key_state(uint32_t row, uint32_t col, bool is_pressed) {
    if (row > 8 || col > 7) return; // Guard against out-of-bounds

    if (is_pressed) {
        // 0 = Depressed
        machine.key.matrix[row] &= ~(1 << col);
    } else {
        // 1 = Not depressed
        machine.key.matrix[row] |= (1 << col);
    }
}

DECL_EXPORT Vm_8085 *get_cpu_state_ptr(void) {
    return &machine.cpu;
}

DECL_EXPORT DebuggerLoc *get_source_map_ptr(void) {
    return machine.address_to_line;
}

#ifdef TARGET_DESKTOP

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>



static void handle_sdl_key(const SDL_KeyboardEvent *event) {
    //      col0 col1 col2 col3 col4 col5 col6 col7
    //
    // row0   z    x    c    v    b    n    m    l
    // row1   a    s    d    f    g    h    j    k
    // row2   q    w    e    r    t    y    u    i
    // row3   o    p   (@^) (\|) (,<) (.>) (/?) (]})
    // row4  (1!) (2") (3#) (4$) (5%) (6&) (7') (8()
    // row5  (9)) (0-) (;+) (:*) (_=) ([{) <sp> ins
    // row6   bs   up   dn   lf   rg  tab  esc  ret
    // row7   f1   f2   f3   f4   f5            stop
    // row8  shft ctrl grph      caps           

    int row = -1;
    int col = -1;

    switch (event->scancode) {
        // Row 0: z, x, c, v, b, n, m, l
        case SDL_SCANCODE_Z: row = 0; col = 0; break;
        case SDL_SCANCODE_X: row = 0; col = 1; break;
        case SDL_SCANCODE_C: row = 0; col = 2; break;
        case SDL_SCANCODE_V: row = 0; col = 3; break;
        case SDL_SCANCODE_B: row = 0; col = 4; break;
        case SDL_SCANCODE_N: row = 0; col = 5; break;
        case SDL_SCANCODE_M: row = 0; col = 6; break;
        case SDL_SCANCODE_L: row = 0; col = 7; break;

        // Row 1: a, s, d, f, g, h, j, k
        case SDL_SCANCODE_A: row = 1; col = 0; break;
        case SDL_SCANCODE_S: row = 1; col = 1; break;
        case SDL_SCANCODE_D: row = 1; col = 2; break;
        case SDL_SCANCODE_F: row = 1; col = 3; break;
        case SDL_SCANCODE_G: row = 1; col = 4; break;
        case SDL_SCANCODE_H: row = 1; col = 5; break;
        case SDL_SCANCODE_J: row = 1; col = 6; break;
        case SDL_SCANCODE_K: row = 1; col = 7; break;

        // Row 2: q, w, e, r, t, y, u, i
        case SDL_SCANCODE_Q: row = 2; col = 0; break;
        case SDL_SCANCODE_W: row = 2; col = 1; break;
        case SDL_SCANCODE_E: row = 2; col = 2; break;
        case SDL_SCANCODE_R: row = 2; col = 3; break;
        case SDL_SCANCODE_T: row = 2; col = 4; break;
        case SDL_SCANCODE_Y: row = 2; col = 5; break;
        case SDL_SCANCODE_U: row = 2; col = 6; break;
        case SDL_SCANCODE_I: row = 2; col = 7; break;

        // Row 3: o, p, (@^), (\|), (,<), (.>), (/^), (]})
        case SDL_SCANCODE_O:            row = 3; col = 0; break;
        case SDL_SCANCODE_P:            row = 3; col = 1; break;
        case SDL_SCANCODE_GRAVE:        row = 3; col = 2; break;
        case SDL_SCANCODE_BACKSLASH:    row = 3; col = 3; break;
        case SDL_SCANCODE_COMMA:        row = 3; col = 4; break;
        case SDL_SCANCODE_PERIOD:       row = 3; col = 5; break;
        case SDL_SCANCODE_SLASH:        row = 3; col = 6; break;
        case SDL_SCANCODE_RIGHTBRACKET: row = 3; col = 7; break;

        // Row 4: (1!), (2"), (3#), (4$), (5%), (6&), (7'), (8()
        case SDL_SCANCODE_1: row = 4; col = 0; break;
        case SDL_SCANCODE_2: row = 4; col = 1; break;
        case SDL_SCANCODE_3: row = 4; col = 2; break;
        case SDL_SCANCODE_4: row = 4; col = 3; break;
        case SDL_SCANCODE_5: row = 4; col = 4; break;
        case SDL_SCANCODE_6: row = 4; col = 5; break;
        case SDL_SCANCODE_7: row = 4; col = 6; break;
        case SDL_SCANCODE_8: row = 4; col = 7; break;

        // Row 5: (9)), (0-), (;+), (:*), (_=), ([{), <sp>, ins
        case SDL_SCANCODE_9:           row = 5; col = 0; break;
        case SDL_SCANCODE_0:           row = 5; col = 1; break;
        case SDL_SCANCODE_SEMICOLON:   row = 5; col = 2; break;
        case SDL_SCANCODE_APOSTROPHE:  row = 5; col = 3; break;
        case SDL_SCANCODE_MINUS:       row = 5; col = 4; break;
        case SDL_SCANCODE_LEFTBRACKET: row = 5; col = 5; break;
        case SDL_SCANCODE_SPACE:       row = 5; col = 6; break;
        case SDL_SCANCODE_INSERT:      row = 5; col = 7; break;

        // Row 6: bs, up, dn, lf, rg, tab, esc, ret
        case SDL_SCANCODE_BACKSPACE: row = 6; col = 0; break;
        case SDL_SCANCODE_UP:        row = 6; col = 1; break;
        case SDL_SCANCODE_DOWN:      row = 6; col = 2; break;
        case SDL_SCANCODE_LEFT:      row = 6; col = 3; break;
        case SDL_SCANCODE_RIGHT:     row = 6; col = 4; break;
        case SDL_SCANCODE_TAB:       row = 6; col = 5; break;
        case SDL_SCANCODE_ESCAPE:    row = 6; col = 6; break;
        case SDL_SCANCODE_RETURN:    row = 6; col = 7; break;

        // Row 7: f1, f2, f3, f4, f5, _, _, stop
        case SDL_SCANCODE_F1:    row = 7; col = 0; break;
        case SDL_SCANCODE_F2:    row = 7; col = 1; break;
        case SDL_SCANCODE_F3:    row = 7; col = 2; break;
        case SDL_SCANCODE_F4:    row = 7; col = 3; break;
        case SDL_SCANCODE_F5:    row = 7; col = 4; break;
        case SDL_SCANCODE_F6:    row = 7; col = 7; break; // STOP

        // Row 8: shft, ctrl, grph, _, caps, _, _, _
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT:    row = 8; col = 0; break;
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL:     row = 8; col = 1; break;
        case SDL_SCANCODE_LALT:
        case SDL_SCANCODE_RALT:      row = 8; col = 2; break;
        case SDL_SCANCODE_CAPSLOCK:  row = 8; col = 4; break;

        default:
            break;
    }

    if (row != -1 && col != -1) {
        set_key_state(row, col, (event->type == SDL_EVENT_KEY_DOWN));
    }
}

const char* vertex_shader_src =
    "#version 300 es\n"
    "in vec2 a_position;\n"
    "out vec2 v_texCoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "    v_texCoord = a_position * 0.5 + 0.5;\n"
    "    v_texCoord.y = 1.0 - v_texCoord.y;\n" // Handles OpenGL bottom-up vs SDL top-down
    "}\n";

const char* fragment_shader_src =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_texCoord;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_image;\n"
    "uniform vec2 u_resolution;\n"
    "uniform vec2 u_aaWidth;\n"
    "uniform vec3 u_colorBg;\n"
    "uniform vec3 u_colorPixel;\n"
    "uniform vec3 u_colorShadow;\n"
    "const float PIXEL_GAP = 0.1;\n"
    "const vec2 SHADOW_OFFSET = vec2(0.20, 0.25);\n"
    "const float CONTRAST = 1.00;\n"
    "const float BRIGHTNESS = 0.0;\n"
    "const float BORDER_WIDTH = 2.0;\n"
    "float getPixelMask(vec2 gridFract, vec2 aaWidth) {\n"
    "    vec2 bl = smoothstep(vec2(PIXEL_GAP) - aaWidth, vec2(PIXEL_GAP) + aaWidth, gridFract);\n"
    "    vec2 tr = smoothstep(vec2(PIXEL_GAP) - aaWidth, vec2(PIXEL_GAP) + aaWidth, 1.0 - gridFract);\n"
    "    return bl.x * bl.y * tr.x * tr.y;\n"
    "}\n"
    "float hash(vec2 p) {\n"
    "    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453123);\n"
    "}\n"
    "void main() {\n"
    "    vec2 pixelCoord = v_texCoord * u_resolution;\n"
    "    float lightFactor = dot(v_texCoord, vec2(0.7, -0.7)) * 0.1 + 0.95;\n"
    "    vec3 baseColor = u_colorBg * lightFactor;\n"
    "    baseColor += (hash(gl_FragCoord.xy) * 0.08);\n"
    "    vec3 finalColor = baseColor;\n"
    "    \n"
    "    if (pixelCoord.x >= BORDER_WIDTH && pixelCoord.x < (u_resolution.x - BORDER_WIDTH) &&\n"
    "        pixelCoord.y >= BORDER_WIDTH && pixelCoord.y < (u_resolution.y - BORDER_WIDTH)) {\n"
    "        vec2 innerCoord = pixelCoord - vec2(BORDER_WIDTH);\n"
    "        vec2 innerResolution = u_resolution - vec2(2.0 * BORDER_WIDTH);\n"
    "        \n"
    "        vec2 gridId = floor(innerCoord);\n"
    "        vec2 gridFract = fract(innerCoord);\n"
    "        vec2 texUV = (gridId + 0.5) / innerResolution;\n"
    "        vec4 texColor = texture(u_image, texUV);\n"
    "        float pixelActive = step(texColor.r, 0.52);\n"
    "        \n"
    "        vec2 shadowCoord = innerCoord - SHADOW_OFFSET;\n"
    "        vec2 shadowGridId = floor(shadowCoord);\n"
    "        vec2 shadowUV = (shadowGridId + 0.5) / innerResolution;\n"
    "        vec4 shadowTexColor = texture(u_image, shadowUV);\n"
    "        float shadowActive = step(shadowTexColor.r, 0.52);\n"
    "        \n"
    "        float pixelMask = getPixelMask(gridFract, u_aaWidth);\n"
    "        float shadowMask = getPixelMask(fract(shadowCoord), u_aaWidth);\n"
    "        \n"
    "        finalColor = mix(finalColor, u_colorShadow, shadowMask * shadowActive);\n"
    "        finalColor = mix(finalColor, u_colorPixel, pixelMask * pixelActive);\n"
    "    }\n"
    "    \n"
    "    finalColor *= vec3(1.0, 0.99, 0.96);\n"
    "    finalColor = (finalColor - 0.5) * CONTRAST + 0.5 + BRIGHTNESS;\n"
    "    finalColor = clamp(finalColor, 0.0, 1.0);\n"
    "    fragColor = vec4(finalColor, 1.0);\n"
    "}\n";

#define GL_FUNC_LIST(X) \
    X(PFNGLCREATESHADERPROC,            CreateShader) \
    X(PFNGLSHADERSOURCEPROC,            ShaderSource) \
    X(PFNGLCOMPILESHADERPROC,           CompileShader) \
    X(PFNGLGETSHADERIVPROC,             GetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC,        GetShaderInfoLog) \
    X(PFNGLCREATEPROGRAMPROC,           CreateProgram) \
    X(PFNGLATTACHSHADERPROC,            AttachShader) \
    X(PFNGLLINKPROGRAMPROC,             LinkProgram) \
    X(PFNGLGETPROGRAMIVPROC,            GetProgramiv) \
    X(PFNGLUSEPROGRAMPROC,              UseProgram) \
    X(PFNGLGENBUFFERSPROC,              GenBuffers) \
    X(PFNGLBINDBUFFERPROC,              BindBuffer) \
    X(PFNGLBUFFERDATAPROC,              BufferData) \
    X(PFNGLGENVERTEXARRAYSPROC,         GenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC,         BindVertexArray) \
    X(PFNGLGETATTRIBLOCATIONPROC,       GetAttribLocation) \
    X(PFNGLVERTEXATTRIBPOINTERPROC,     VertexAttribPointer) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray) \
    X(PFNGLGETUNIFORMLOCATIONPROC,      GetUniformLocation) \
    X(PFNGLUNIFORM1IPROC,               Uniform1i) \
    X(PFNGLUNIFORM2FPROC,               Uniform2f) \
    X(PFNGLUNIFORM3FPROC,               Uniform3f) \
    X(PFNGLACTIVETEXTUREPROC,           ActiveTexture) \
    /*end*/

#define GL_FUNC_STRUCT_DECL(type, name) type name;

struct {
    GL_FUNC_LIST(GL_FUNC_STRUCT_DECL)
} gl;

bool load_gl_extensions() {
    #define GL_FUNC_LOAD(type, name) \
        gl.name = (type)SDL_GL_GetProcAddress("gl" #name); \
        if (!gl.name) { SDL_Log("Failed to load gl" #name); return false; }

    GL_FUNC_LIST(GL_FUNC_LOAD)

    return true;
}

GLuint compile_shader(GLenum type, const char* src) {
    GLuint shader = gl.CreateShader(type);
    gl.ShaderSource(shader, 1, &src, NULL);
    gl.CompileShader(shader);
    GLint success;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info[512];
        gl.GetShaderInfoLog(shader, 512, NULL, info);
        SDL_Log("Shader compilation failed: %s", info);
    }
    return shader;
}

#define SAMPLE_RATE 22050

void update_audio(SDL_AudioStream *audio_stream) {
    static int current_sine_sample = 0;

    const int minimum_audio = 128;
    if (SDL_GetAudioStreamQueued(audio_stream) < minimum_audio) {
        static float samples[512];  /* this will feed 512 samples each frame until we get to our maximum. */
        enum { PREV_SAMPLE_COUNT = 32 };
        static float prev_samples[PREV_SAMPLE_COUNT];
        float mute_factor = machine.buzzer_on ? 1.0f : 0.0f;

        /* generate a 440Hz pure tone */
        for (uint32_t i = 0; i < SDL_arraysize(samples); i++) {
            const int freq = 440;
            const float phase = current_sine_sample * freq / (float)SAMPLE_RATE;
            samples[i] = mute_factor * (SDL_fmodf(phase, 1.0f) > 0.5f ? 1.0f : -1.0f);
            samples[i] /= (float)(PREV_SAMPLE_COUNT + 1);

            for (uint32_t s = 0; s < SDL_arraysize(prev_samples); ++s)
                samples[i] += prev_samples[s] / (float) (PREV_SAMPLE_COUNT + 1);

            for (uint32_t s = SDL_arraysize(prev_samples) - 1; s > 0; --s)
                prev_samples[s] = prev_samples[s-1];

            current_sine_sample++;
        }

        /* wrapping around to avoid floating-point errors */
        current_sine_sample %= SAMPLE_RATE;

        /* feed the new data to the stream. It will queue at the end, and trickle out as the hardware needs more data. */
        SDL_PutAudioStreamData(audio_stream, samples, sizeof (samples));
    }
}

uint8_t *read_entire_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0) {
        fclose(f);
        return NULL;
    }

    uint8_t *data = (uint8_t *)allocate((size_t)file_size);
    if (data == NULL) {
        fclose(f);
        return NULL;
    }

    size_t bytes_read = fread(data, 1, (size_t)file_size, f);
    fclose(f);

    if (bytes_read != (size_t)file_size) {
        free(data);
        return NULL;
    }

    if (out_size) {
        *out_size = (size_t)file_size;
    }

    return data;
}

#define arg_shift(argc, argv) ((argc) > 0 ? (--(argc), *(argv)++) : NULL)

int main(int argc, char* argv[]) {

    const char *program = arg_shift(argc, argv);
    (void)program;

    const char *co_file_path = NULL;
    const char *rom_file_path = NULL;

    while (argc > 0) {
        const char *arg = arg_shift(argc, argv);

        if (strncmp(arg, "--", 2) != 0) {
            continue;
        }

        arg += 2;
    
        if (strcmp(arg, "rom") == 0) {
            if (!(rom_file_path = arg_shift(argc, argv))) {
                SDL_Log("Missing argument <ROM File path>.\n");
                return 1;
            }
        }
        else if (strcmp(arg, "co-file") == 0) {
            if (!(co_file_path = arg_shift(argc, argv))) {
                SDL_Log("TODO(jkk): Missing argument <CO File path>.\n");
                return 1;
            }
        }
        else {
            SDL_Log("Unknown argument: %s\n", arg);
        }
    }

    reset_emulator();

    if (rom_file_path) {
        size_t rom_size = 0;
        uint8_t *rom_data = read_entire_file(rom_file_path, &rom_size);
    
        if (!rom_data) {
            SDL_Log("Could not open or read ROM file '%s'\n", rom_file_path);
            return 1;
        }

        load_rom(rom_data, rom_size);
        free(rom_data);
    }

    if (co_file_path) {
        size_t file_size = 0;
        uint8_t *co_file_data = read_entire_file(co_file_path, &file_size);
     
        if (!co_file_data) {
            SDL_Log("Could not open or read CO file '%s'\n", co_file_path);
            return 1;
        }

        if (file_size <= 6) {
            SDL_Log("Invalid CO file size: %zu bytes\n", file_size);
            free(co_file_data);
            return 1;
        }

        load_co_file(co_file_data, (int32_t)file_size, rom_file_path == NULL);
        free(co_file_data);
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return 1;
    }

    // Initialize audio
    SDL_AudioStream *audio_stream = NULL;
    {
        SDL_AudioSpec spec = {0};
        spec.channels = 1;
        spec.format = SDL_AUDIO_F32;
        spec.freq = SAMPLE_RATE;
        audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (!audio_stream) {
            SDL_Log("Couldn't create audio stream: %s", SDL_GetError());
            return 1;
        }
        SDL_ResumeAudioStreamDevice(audio_stream);
    }

    // Request OpenGL ES 3.0 context (matches #version 300 es)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

    SDL_Window* window = SDL_CreateWindow(
        "PC8201 Emulator",
        SCREEN_WIDTH * 3,
        SCREEN_HEIGHT * 3,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL
    );

    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        SDL_Log("SDL_GL_CreateContext failed: %s", SDL_GetError());
        return 1;
    }

    SDL_GL_SetSwapInterval(1);

    if (!load_gl_extensions()) {
        return 1;
    }

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_src);
    GLuint shaderProgram = gl.CreateProgram();
    gl.AttachShader(shaderProgram, vs);
    gl.AttachShader(shaderProgram, fs);
    gl.LinkProgram(shaderProgram);
    gl.UseProgram(shaderProgram);

    GLuint VAO, VBO;
    gl.GenVertexArrays(1, &VAO);
    gl.GenBuffers(1, &VBO);

    float full_screen_quad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f,  1.0f, 1.0f,  1.0f};
    gl.BindVertexArray(VAO);
    gl.BindBuffer(GL_ARRAY_BUFFER, VBO);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof(full_screen_quad), full_screen_quad, GL_STATIC_DRAW);

    GLint posAttrib = gl.GetAttribLocation(shaderProgram, "a_position");
    gl.EnableVertexAttribArray(posAttrib);
    gl.VertexAttribPointer(posAttrib, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);

    // Texture Setup
    GLuint texture;
    glGenTextures(1, &texture);
    gl.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);

    // GL_NEAREST ensures the shader accurately calculates pixel locations
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Create an empty texture buffer (RGBA8888 maps perfectly to GL_RGBA/GL_UNSIGNED_BYTE)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SCREEN_WIDTH, SCREEN_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    // Set Static Uniforms
    gl.Uniform1i(gl.GetUniformLocation(shaderProgram, "u_image"), 0);
    gl.Uniform2f(gl.GetUniformLocation(shaderProgram, "u_resolution"), (float)SCREEN_WIDTH + 4, (float)SCREEN_HEIGHT + 4);

    // Define standard LCD greenish/gray colors for the shader
    gl.Uniform3f(gl.GetUniformLocation(shaderProgram, "u_colorBg"),     168.f/255.f, 172.0f/255.f, 164.f/255.f);
    gl.Uniform3f(gl.GetUniformLocation(shaderProgram, "u_colorPixel"),   41.f/255.f,  42.0f/255.f,  40.f/255.f);
    gl.Uniform3f(gl.GetUniformLocation(shaderProgram, "u_colorShadow"), 129.f/255.f, 130.0f/255.f, 124.f/255.f);

    bool running = true;
    SDL_Event event;

    uint64_t last_time = SDL_GetTicksNS();
    int32_t t_states_excess = 0;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
            else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
                if (!event.key.repeat) {
                    handle_sdl_key(&event.key);
                }
            }
        }

        uint64_t current_time = SDL_GetTicksNS();
        double delta_time = (double)(current_time - last_time) / 1e9;
        if (delta_time > 0.1) delta_time = 0.1;
        last_time = current_time;

        int32_t t_state_goal = (int32_t)(CPU_CLOCK_FREQ * delta_time) - t_states_excess;

        if (t_state_goal > 0) {
            int32_t res = run(t_state_goal);
            t_states_excess = (res < 0) ? 0 : res;
        }

        // Audio
        update_audio(audio_stream);

        int window_w, window_h;
        SDL_GetWindowSizeInPixels(window, &window_w, &window_h);

        // Calculate the letterboxed viewport dimensions
        float scale_w = (float)window_w / SCREEN_WIDTH;
        float scale_h = (float)window_h / SCREEN_HEIGHT;
        float scale = (scale_w < scale_h) ? scale_w : scale_h;

        int viewport_w = (int)(SCREEN_WIDTH * scale);
        int viewport_h = (int)(SCREEN_HEIGHT * scale);
        int viewport_x = (window_w - viewport_w) / 2;
        int viewport_y = (window_h - viewport_h) / 2;

        // Apply the letterboxed viewport
        glViewport(viewport_x, viewport_y, viewport_w, viewport_h);

        // Clear the ENTIRE window to black before drawing the restricted viewport
        // (We enable the scissor test briefly to clear outside the glViewport boundaries)
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Update AA width uniformly based on the single letterbox scale factor
        gl.Uniform2f(gl.GetUniformLocation(shaderProgram, "u_aaWidth"), 1.0f / scale, 1.0f / scale);

        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, get_canvas_buffer());

        gl.UseProgram(shaderProgram);
        gl.BindVertexArray(VAO);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        SDL_GL_SwapWindow(window);
    }

    // Cleanup
    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

#endif // TARGET_DESKTOP
