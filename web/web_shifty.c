#define VM8085_IMPLEMENTATION
#include "vm8085.h"

#ifdef TARGET_WEB
#define memset __builtin_memset
#define memcpy __builtin_memcpy
#else
#include <string.h>
#endif

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 64

#ifdef TARGET_WEB
#define DECL_EXPORT __attribute__((visibility("default")))
#else
#define DECL_EXPORT
#endif

#include "executable_code.h"

typedef struct {
    uint8_t bytes[50][4];
    uint8_t page; // 0-3
    uint8_t offset; // 0-49
    bool is_counter_mode_down;
} Pc8201LcdDriver;

typedef struct {
    Pc8201LcdDriver drivers[10];
    uint16_t driver_select; // bit 0 selects driver 0, 1 selects driver 1, ... , 10 selects driver 10.

    // 32-bit RGBA buffer that JavaScript will read directly to draw to the HTML5 canvas
} Pc8201Lcd;

typedef uint8_t BreakPointKind;
enum
{
    BP_NONE = 0,
    BP_TEMP = 1,
    BP_PERM = 2,
};


// --- 3. Outer Machine State Structure ---
typedef struct {
    Vm_8085 cpu;
    Pc8201Lcd lcd;
    uint32_t canvas[SCREEN_WIDTH * SCREEN_HEIGHT];

    uint8_t memory[65536];

    // Keyboard Matrix State
    uint8_t key_matrix[9]; // Index 0-7 = PA0-PA7, Index 8 = PB0
    uint8_t key_strobe_a;  // State of Port 0xB9 (PA0-PA7)
    uint8_t key_strobe_b;  // State of Port 0xBA (PB0)

    BreakPointKind breakpoints[65536];
} Pc8201Machine;

// Globally instantiate our machine state
static Pc8201Machine machine;

// --- 5. System-Specific Peripherals: Translate 8085 RAM into RGBA Canvas Pixels ---
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

static void mem_cb(Vm_8085 *vm, uint16_t addr, bool is_write, uint8_t *in_out_data)
{
    Pc8201Machine *mach = (Pc8201Machine *)vm->user_data;

    if (is_write) {
        mach->memory[addr] = *in_out_data;
    }
    else {
        *in_out_data = mach->memory[addr];
    }
}

#ifdef TARGET_DEBUG
#include <stdio.h>
#else
#define fprintf(...)
#endif

static bool io_cb(Vm_8085 *vm, uint8_t port, bool is_write, uint8_t *in_out_data)
{
    enum
    {
        PortLcdCmd = 0xfe,
        PortLcdStat = PortLcdCmd,
        PortLcdData = 0xff,
        Port81C55Cmd = 0xB8,
        Port81C55A = 0xB9,
        Port81C55B = 0xBA,
        PortKeyIn = 0xE8,
    };

    bool is_read = !is_write;
    Pc8201Machine *mach = (Pc8201Machine *)vm->user_data;
    Pc8201Lcd *lcd = &mach->lcd;

    if (is_write && port == Port81C55A)
    {
        mach->key_strobe_a = *in_out_data; // Track keyboard strobe

        lcd->driver_select &= 0xff00;
        lcd->driver_select |= *in_out_data;
        return true;
    }

    if (is_write && port == Port81C55B)
    {
        mach->key_strobe_b = *in_out_data; // Track keyboard strobe (PB0)

        lcd->driver_select &= 0x00ff;
        lcd->driver_select |= (*in_out_data & 0x3) << 8;
        return true;
    }

    if (is_read && port == Port81C55B)
    {
        *in_out_data = (lcd->driver_select >> 8) & 0x3;
        return true;
    }

    // Handle Keyboard Data Read (Port 0xE8)
    if (is_read && port == PortKeyIn)
    {
        uint8_t result = 0xFF; // Default to all keys released

        // Check Port A strobes (PA0 - PA7)
        for (int i = 0; i < 8; i++) {
            if (!(mach->key_strobe_a & (1 << i))) {
                // Strobe is ACTIVE (0), merge in depressed keys (0) for this row
                result &= mach->key_matrix[i];
            }
        }

        // Check Port B strobe (PB0 is bit 0)
        if (!(mach->key_strobe_b & 0x01)) {
            // Strobe is ACTIVE (0), merge in depressed keys for PB0 row
            result &= mach->key_matrix[8];
        }

        *in_out_data = result;
        return true;
    }

    if (is_write && port == PortLcdCmd)
    {
        uint8_t cmd = *in_out_data;

        for (int i = 0; i < 10; ++i) {
            if (!(lcd->driver_select & (1 << i))) continue;

            Pc8201LcdDriver *driv = &lcd->drivers[i];

            // Handle the Up/Down selection command (0x3A / 0x3B)
            if ((cmd & 0xFE) == 0x3A) {
                driv->is_counter_mode_down = !(cmd & 0x01); // 1 = Up Counter, 0 = Down Counter
            }
            else {
                driv->page = (cmd >> 6) & 0x3;
                driv->offset = cmd & 0x3F;
            }

        }
        return true;
    }

    if (is_write && port == PortLcdData)
    {
        for (int i = 0; i < 10; ++i) {
            if (!(lcd->driver_select & (1 << i))) continue;

            Pc8201LcdDriver* driv = &lcd->drivers[i];

            // Enforce bounds checking before writing
            if (driv->offset < 50 && driv->page < 4) {
                driv->bytes[driv->offset][driv->page] = *in_out_data;
            }

            // Mimic "module 50" loop counter behavior
            driv->offset += driv->is_counter_mode_down ? -1 : 1;
            if (driv->offset >= 50) driv->offset = driv->is_counter_mode_down ? 49 : 0;
        }
        return true;
    }

    if (is_read && port == PortLcdStat)
    {
        *in_out_data = 0x00; // bit 7 is 0 to indicate LCD ready (For now the LCD is always ready)
        return true;
    }

    if (is_write) fprintf(stderr, "IO WRITE port %#02X, value = %#02X;\n", port, *in_out_data);
    else fprintf(stderr, "IO READ port %#02X;\n", port);

    return false;
}

DECL_EXPORT void init_machine(uint32_t random_seed) {
    // Clear memory & registers

    // for (uint32_t i = 0; i < sizeof(machine); ++i) *(uint8_t*)(&machine) = 0;
    memset(&machine, 0, sizeof(machine));

    // Load the hardcoded binary directly into the CPU's memory array at address 0x0000
    memcpy(machine.memory + program_origin, program, program_len);

    machine.memory[0] = 0xC3; // JMP
    machine.memory[1] = program_origin & 0xff;
    machine.memory[2] = (program_origin >> 8) & 0xff;
    machine.cpu.sp = 0x9DE4; // Taken directly from the debugger in VirtualT.

    for (int i = 0; i < 9; i++) {
        machine.key_matrix[i] = 0xFF;
    }
    machine.key_strobe_a = 0xFF;
    machine.key_strobe_b = 0xFF;

    machine.cpu.io_cb = io_cb;
    machine.cpu.mem_cb = mem_cb;
    machine.cpu.user_data = (void *)&machine;
}

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

DECL_EXPORT int32_t set_break_on_return(void) {
    if (machine.cpu.debug.ret_addr_stack_index == 0) {
        return -1;
    }

    uint16_t ret_addr = machine.cpu.debug.ret_addr_stack[machine.cpu.debug.ret_addr_stack_index - 1];
    set_breakpoint(ret_addr, true);
    return ret_addr;
}

static bool check_breakpoint(uint16_t addr) {

    BreakPointKind bp = machine.breakpoints[addr];

    if (bp == BP_NONE) {
        return false;
    }

    if (bp == BP_TEMP) {
        machine.breakpoints[addr] = BP_NONE;
    }

    assert(bp == BP_PERM);
    return true;
}

DECL_EXPORT void step_cpu(void) {
    vm8085_run(&machine.cpu, 1);
    update_canvas_buffer(&machine);
}

DECL_EXPORT void step_over_cpu(void) {
    uint16_t call_depth_before = machine.cpu.debug.ret_addr_stack_index;
    uint16_t call_depth_after;
    do
    {
        vm8085_run(&machine.cpu, 1);
        call_depth_after = machine.cpu.debug.ret_addr_stack_index;
    } while (call_depth_after > call_depth_before);

    update_canvas_buffer(&machine);
}

DECL_EXPORT int32_t run_frame(int32_t t_state_goal) {
    int32_t t_states_executed = 0;

    while (t_states_executed < t_state_goal) {
        // Stop execution immediately if we hit a breakpoint
        if (check_breakpoint(machine.cpu.pc)) {
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
        machine.key_matrix[row] &= ~(1 << col);
    } else {
        // 1 = Not depressed
        machine.key_matrix[row] |= (1 << col);
    }
}

DECL_EXPORT Vm_8085 *get_cpu_state_ptr(void) {
    return &machine.cpu;
}

#ifdef TARGET_DEBUG
int main(void)
{
    init_machine(42);
    while (true)
    {
        run_frame(40);
    }
    return 0;
}
#endif