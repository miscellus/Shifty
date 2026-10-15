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

#ifdef TARGET_WEB
#define DECL_EXPORT __attribute__((visibility("default")))
#else
#define DECL_EXPORT
#endif

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

typedef struct {
    uint16_t timer;
    Timer_Mode timer_mode;
} Pc8201_81C55;

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

// --- 3. Outer Machine State Structure ---
typedef struct {
    Vm_8085 cpu;
    Pc8201Lcd lcd;
    uint32_t canvas[SCREEN_WIDTH * SCREEN_HEIGHT];

    uint8_t memory[65536]; // TODO(jkk): Banking

    Pc8201Keyboard key;

    Pc8201_81C55 ic_81c55;

    // Debugger support
    BreakPointKind breakpoints[65536];
    DebuggerLoc address_to_line[65536];
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

static void loop_cb(Vm_8085 *vm) {
    Pc8201Machine *mach = (Pc8201Machine *)vm->user_data;
    (void)mach;
}

#ifdef TARGET_DEBUG
#include <stdio.h>
#else
#define fprintf(...) ((void)0)
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
        mach->key.strobe_a = *in_out_data; // Track keyboard strobe

        lcd->driver_select &= 0xff00;
        lcd->driver_select |= *in_out_data;
        return true;
    }

    if (is_write && port == Port81C55B)
    {
        mach->key.strobe_b = *in_out_data; // Track keyboard strobe (PB0)

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

#ifdef TARGET_DESKTOP
#include <stdlib.h>
#endif

DECL_EXPORT void reset_emulator_with_co_file(uint8_t *co_file, int32_t co_file_len) {
    // Clear memory & registers

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

    // Load the hardcoded binary directly into the CPU's memory array at address 0x0000
    memset(&machine, 0, sizeof(machine));
    memcpy(machine.memory + program_base_addr, program, program_length);

    machine.memory[0] = 0xCD; // CALL
    machine.memory[1] = program_entry_addr & 0xff;
    machine.memory[2] = (program_entry_addr >> 8) & 0xff;
    machine.memory[3] = 0xC3; // JMP
    machine.memory[4] = 0x0;
    machine.memory[5] = 0x0;
    machine.cpu.sp = 0x9DE4; // Taken directly from the debugger in VirtualT.

    for (int i = 0; i < 9; i++) {
        machine.key.matrix[i] = 0xFF;
    }
    machine.key.strobe_a = 0xFF;
    machine.key.strobe_b = 0xFF;

    machine.cpu.io_cb = io_cb;
    machine.cpu.mem_cb = mem_cb;
    machine.cpu.loop_cb = loop_cb;
    machine.cpu.user_data = (void *)&machine;
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

#if 0
#ifdef TARGET_DESKTOP

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 64
#define CPU_CLOCK_FREQ 2457600.0 // 2.4576 MHz

// Helper to translate SDL keys to the PC-8201 9x8 keyboard matrix
static void handle_sdl_key(SDL_Keycode key, bool is_pressed) {
    int row = -1, col = -1;

    switch (key) {
        // Row 0
        case SDLK_Z:          row = 0; col = 0; break;
        case SDLK_X:          row = 0; col = 1; break;
        case SDLK_C:          row = 0; col = 2; break;
        case SDLK_V:          row = 0; col = 3; break;
        case SDLK_B:          row = 0; col = 4; break;
        case SDLK_N:          row = 0; col = 5; break;
        case SDLK_M:          row = 0; col = 6; break;
        case SDLK_COMMA:      row = 0; col = 7; break;

        // Row 1
        case SDLK_A:          row = 1; col = 0; break;
        case SDLK_S:          row = 1; col = 1; break;
        case SDLK_D:          row = 1; col = 2; break;
        case SDLK_F:          row = 1; col = 3; break;
        case SDLK_G:          row = 1; col = 4; break;
        case SDLK_H:          row = 1; col = 5; break;
        case SDLK_J:          row = 1; col = 6; break;
        case SDLK_K:          row = 1; col = 7; break;

        // Row 2
        case SDLK_Q:          row = 2; col = 0; break;
        case SDLK_W:          row = 2; col = 1; break;
        case SDLK_E:          row = 2; col = 2; break;
        case SDLK_R:          row = 2; col = 3; break;
        case SDLK_T:          row = 2; col = 4; break;
        case SDLK_Y:          row = 2; col = 5; break;
        case SDLK_U:          row = 2; col = 6; break;
        case SDLK_I:          row = 2; col = 7; break;

        // Row 3
        case SDLK_O:          row = 3; col = 0; break;
        case SDLK_P:          row = 3; col = 1; break;
        case SDLK_APOSTROPHE: row = 3; col = 2; break;
        case SDLK_LEFTBRACKET:row = 3; col = 3; break;
        case SDLK_BACKSLASH:  row = 3; col = 4; break;
        case SDLK_RIGHTBRACKET:row = 3; col = 5; break;
        case SDLK_GRAVE:      row = 3; col = 6; break;
        case SDLK_SLASH:      row = 3; col = 7; break;

        // Row 4
        case SDLK_1:          row = 4; col = 0; break;
        case SDLK_2:          row = 4; col = 1; break;
        case SDLK_3:          row = 4; col = 2; break;
        case SDLK_4:          row = 4; col = 3; break;
        case SDLK_5:          row = 4; col = 4; break;
        case SDLK_6:          row = 4; col = 5; break;
        case SDLK_7:          row = 4; col = 6; break;
        case SDLK_8:          row = 4; col = 7; break;

        // Row 5
        case SDLK_9:          row = 5; col = 0; break;
        case SDLK_0:          row = 5; col = 1; break;
        case SDLK_SEMICOLON:  row = 5; col = 2; break;
        case SDLK_MINUS:      row = 5; col = 3; break;
        case SDLK_EQUALS:     row = 5; col = 4; break;
        case SDLK_SPACE:      row = 5; col = 5; break;
        case SDLK_INSERT:     row = 5; col = 6; break;
        case SDLK_PERIOD:     row = 5; col = 7; break;

        // Row 6
        case SDLK_BACKSPACE:  row = 6; col = 0; break;
        case SDLK_UP:         row = 6; col = 1; break;
        case SDLK_DOWN:       row = 6; col = 2; break;
        case SDLK_LEFT:       row = 6; col = 3; break;
        case SDLK_RIGHT:      row = 6; col = 4; break;
        case SDLK_TAB:        row = 6; col = 5; break;
        case SDLK_ESCAPE:     row = 6; col = 6; break;
        case SDLK_RETURN:     row = 6; col = 7; break;

        // Row 7
        case SDLK_F1:         row = 7; col = 0; break;
        case SDLK_F2:         row = 7; col = 1; break;
        case SDLK_F3:         row = 7; col = 2; break;
        case SDLK_F4:         row = 7; col = 3; break;
        case SDLK_F5:         row = 7; col = 4; break;
        case SDLK_PAUSE:      row = 7; col = 7; break;

        // Row 8 (Modifiers)
        case SDLK_LSHIFT:
        case SDLK_RSHIFT:     row = 8; col = 0; break;
        case SDLK_LCTRL:
        case SDLK_RCTRL:      row = 8; col = 1; break;
        case SDLK_LALT:
        case SDLK_RALT:       row = 8; col = 2; break;
        case SDLK_CAPSLOCK:   row = 8; col = 4; break;

        default: break;
    }

    if (row != -1 && col != -1) {
        set_key_state(row, col, is_pressed);
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        SDL_Log("Usage: %s <path_to_co_file>", argv[0]);
        return 1;
    }

    // --- 1. Load .CO File ---
    FILE *co_file = fopen(argv[1], "rb");
    if (co_file == NULL) {
        SDL_Log("Could not open CO file '%s'", argv[1]);
        return 1;
    }

    fseek(co_file, 0, SEEK_END);
    long file_size = ftell(co_file);
    fseek(co_file, 0, SEEK_SET);

    if (file_size <= 6) { // CO files must have at least a 6-byte header
        SDL_Log("Invalid CO file size: %ld bytes", file_size);
        fclose(co_file);
        return 1;
    }

    uint8_t *co_file_data = (uint8_t *)allocate((size_t)file_size);
    if (!co_file_data) {
        SDL_Log("Failed to allocate memory for CO file");
        fclose(co_file);
        return 1;
    }

    size_t bytes_read = fread(co_file_data, 1, (size_t)file_size, co_file);
    fclose(co_file);

    if (bytes_read != (size_t)file_size) {
        SDL_Log("Failed to read complete file data");
        free(co_file_data);
        return 1;
    }

    // --- 2. Initialize Emulator State ---
    reset_emulator_with_co_file(co_file_data, (int32_t)file_size);
    free(co_file_data); // Memory was copied into machine.memory

    // --- 3. SDL3 Initialization ---
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "PC8201 Emulator",
        SCREEN_WIDTH * 3,
        SCREEN_HEIGHT * 3,
        SDL_WINDOW_RESIZABLE
    );

    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_SetRenderVSync(renderer, 1);

    SDL_Texture* texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_STREAMING,
        SCREEN_WIDTH,
        SCREEN_HEIGHT
    );

    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

    // --- 4. Main Loop ---
    bool running = true;
    SDL_Event event;

    uint64_t last_time = SDL_GetTicksNS();
    int32_t t_states_excess = 0;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
            else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (!event.key.repeat) {
                    handle_sdl_key(event.key.key, true);
                }
            }
            else if (event.type == SDL_EVENT_KEY_UP) {
                handle_sdl_key(event.key.key, false);
            }
        }

        // --- Delta Time & Dynamic Cycle Execution (matching JS logic) ---
        uint64_t current_time = SDL_GetTicksNS();
        double delta_time = (double)(current_time - last_time) / 1e9;
        if (delta_time > 0.1) delta_time = 0.1; // Clamp max dt to prevent huge execution jumps
        last_time = current_time;

        int32_t t_state_goal = (int32_t)(CPU_CLOCK_FREQ * delta_time) - t_states_excess;

        if (t_state_goal > 0) {
            int32_t res = run(t_state_goal);
            t_states_excess = (res < 0) ? 0 : res; // Handle breakpoint pause
        }

        // --- Rendering ---
        uint32_t* src = get_canvas_buffer();
        SDL_UpdateTexture(texture, NULL, src, SCREEN_WIDTH * sizeof(uint32_t));

        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
    }

    // Cleanup
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

#endif // TARGET_DESKTOP

#else

#ifdef TARGET_DESKTOP

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 64
#define CPU_CLOCK_FREQ 2457600.0 // 2.4576 MHz

// Helper to translate SDL keys to the PC-8201 9x8 keyboard matrix
static void handle_sdl_key(SDL_Keycode key, bool is_pressed) {
    int row = -1, col = -1;

    switch (key) {
        // Row 0
        case SDLK_Z:          row = 0; col = 0; break;
        case SDLK_X:          row = 0; col = 1; break;
        case SDLK_C:          row = 0; col = 2; break;
        case SDLK_V:          row = 0; col = 3; break;
        case SDLK_B:          row = 0; col = 4; break;
        case SDLK_N:          row = 0; col = 5; break;
        case SDLK_M:          row = 0; col = 6; break;
        case SDLK_COMMA:      row = 0; col = 7; break;

        // Row 1
        case SDLK_A:          row = 1; col = 0; break;
        case SDLK_S:          row = 1; col = 1; break;
        case SDLK_D:          row = 1; col = 2; break;
        case SDLK_F:          row = 1; col = 3; break;
        case SDLK_G:          row = 1; col = 4; break;
        case SDLK_H:          row = 1; col = 5; break;
        case SDLK_J:          row = 1; col = 6; break;
        case SDLK_K:          row = 1; col = 7; break;

        // Row 2
        case SDLK_Q:          row = 2; col = 0; break;
        case SDLK_W:          row = 2; col = 1; break;
        case SDLK_E:          row = 2; col = 2; break;
        case SDLK_R:          row = 2; col = 3; break;
        case SDLK_T:          row = 2; col = 4; break;
        case SDLK_Y:          row = 2; col = 5; break;
        case SDLK_U:          row = 2; col = 6; break;
        case SDLK_I:          row = 2; col = 7; break;

        // Row 3
        case SDLK_O:          row = 3; col = 0; break;
        case SDLK_P:          row = 3; col = 1; break;
        case SDLK_APOSTROPHE: row = 3; col = 2; break;
        case SDLK_LEFTBRACKET:row = 3; col = 3; break;
        case SDLK_BACKSLASH:  row = 3; col = 4; break;
        case SDLK_RIGHTBRACKET:row = 3; col = 5; break;
        case SDLK_GRAVE:      row = 3; col = 6; break;
        case SDLK_SLASH:      row = 3; col = 7; break;

        // Row 4
        case SDLK_1:          row = 4; col = 0; break;
        case SDLK_2:          row = 4; col = 1; break;
        case SDLK_3:          row = 4; col = 2; break;
        case SDLK_4:          row = 4; col = 3; break;
        case SDLK_5:          row = 4; col = 4; break;
        case SDLK_6:          row = 4; col = 5; break;
        case SDLK_7:          row = 4; col = 6; break;
        case SDLK_8:          row = 4; col = 7; break;

        // Row 5
        case SDLK_9:          row = 5; col = 0; break;
        case SDLK_0:          row = 5; col = 1; break;
        case SDLK_SEMICOLON:  row = 5; col = 2; break;
        case SDLK_MINUS:      row = 5; col = 3; break;
        case SDLK_EQUALS:     row = 5; col = 4; break;
        case SDLK_SPACE:      row = 5; col = 5; break;
        case SDLK_INSERT:     row = 5; col = 6; break;
        case SDLK_PERIOD:     row = 5; col = 7; break;

        // Row 6
        case SDLK_BACKSPACE:  row = 6; col = 0; break;
        case SDLK_UP:         row = 6; col = 1; break;
        case SDLK_DOWN:       row = 6; col = 2; break;
        case SDLK_LEFT:       row = 6; col = 3; break;
        case SDLK_RIGHT:      row = 6; col = 4; break;
        case SDLK_TAB:        row = 6; col = 5; break;
        case SDLK_ESCAPE:     row = 6; col = 6; break;
        case SDLK_RETURN:     row = 6; col = 7; break;

        // Row 7
        case SDLK_F1:         row = 7; col = 0; break;
        case SDLK_F2:         row = 7; col = 1; break;
        case SDLK_F3:         row = 7; col = 2; break;
        case SDLK_F4:         row = 7; col = 3; break;
        case SDLK_F5:         row = 7; col = 4; break;
        case SDLK_PAUSE:      row = 7; col = 7; break;

        // Row 8 (Modifiers)
        case SDLK_LSHIFT:
        case SDLK_RSHIFT:     row = 8; col = 0; break;
        case SDLK_LCTRL:
        case SDLK_RCTRL:      row = 8; col = 1; break;
        case SDLK_LALT:
        case SDLK_RALT:       row = 8; col = 2; break;
        case SDLK_CAPSLOCK:   row = 8; col = 4; break;

        default: break;
    }

    if (row != -1 && col != -1) {
        set_key_state(row, col, is_pressed);
    }
}

// --- Shaders ---
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
    "    vec2 gridId = floor(pixelCoord);\n"
    "    vec2 gridFract = fract(pixelCoord);\n"
    "    vec2 texUV = (gridId + 0.5) / u_resolution;\n"
    "    vec4 texColor = texture(u_image, texUV);\n"
    "    float pixelActive = step(texColor.r, 0.52);\n"
    "    vec2 shadowCoord = pixelCoord - SHADOW_OFFSET;\n"
    "    vec2 shadowGridId = floor(shadowCoord);\n"
    "    vec2 shadowUV = (shadowGridId + 0.5) / u_resolution;\n"
    "    vec4 shadowTexColor = texture(u_image, shadowUV);\n"
    "    float shadowActive = step(shadowTexColor.r, 0.52);\n"
    "    float pixelMask = getPixelMask(gridFract, u_aaWidth);\n"
    "    float shadowMask = getPixelMask(fract(shadowCoord), u_aaWidth);\n"
    "    float lightFactor = dot(v_texCoord, vec2(0.7, -0.7)) * 0.1 + 0.95;\n"
    "    vec3 baseColor = u_colorBg * lightFactor;\n"
    "    baseColor += (hash(gl_FragCoord.xy) * 0.08);\n"
    "    vec3 finalColor = baseColor;\n"
    "    finalColor = mix(finalColor, u_colorShadow, shadowMask * shadowActive);\n"
    "    finalColor = mix(finalColor, u_colorPixel, pixelMask * pixelActive);\n"
    "    finalColor *= vec3(1.0, 0.99, 0.96);\n"
    "    finalColor = (finalColor - 0.5) * CONTRAST + 0.5 + BRIGHTNESS;\n"
    "    finalColor = clamp(finalColor, 0.0, 1.0);\n"
    "    fragColor = vec4(finalColor, 1.0);\n"
    "}\n";

// --- Minimal GL Loader (avoids needing GLEW/GLAD) ---
struct {
    PFNGLCREATESHADERPROC CreateShader;
    PFNGLSHADERSOURCEPROC ShaderSource;
    PFNGLCOMPILESHADERPROC CompileShader;
    PFNGLGETSHADERIVPROC GetShaderiv;
    PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog;
    PFNGLCREATEPROGRAMPROC CreateProgram;
    PFNGLATTACHSHADERPROC AttachShader;
    PFNGLLINKPROGRAMPROC LinkProgram;
    PFNGLGETPROGRAMIVPROC GetProgramiv;
    PFNGLUSEPROGRAMPROC UseProgram;
    PFNGLGENBUFFERSPROC GenBuffers;
    PFNGLBINDBUFFERPROC BindBuffer;
    PFNGLBUFFERDATAPROC BufferData;
    PFNGLGENVERTEXARRAYSPROC GenVertexArrays;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray;
    PFNGLGETATTRIBLOCATIONPROC GetAttribLocation;
    PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
    PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
    PFNGLUNIFORM1IPROC Uniform1i;
    PFNGLUNIFORM2FPROC Uniform2f;
    PFNGLUNIFORM3FPROC Uniform3f;
    PFNGLACTIVETEXTUREPROC ActiveTexture;
} gl;

bool load_gl_extensions() {
    #define LOAD_GL(type, name) \
        gl.name = (type)SDL_GL_GetProcAddress("gl" #name); \
        if (!gl.name) { SDL_Log("Failed to load gl" #name); return false; }

    LOAD_GL(PFNGLCREATESHADERPROC, CreateShader);
    LOAD_GL(PFNGLSHADERSOURCEPROC, ShaderSource);
    LOAD_GL(PFNGLCOMPILESHADERPROC, CompileShader);
    LOAD_GL(PFNGLGETSHADERIVPROC, GetShaderiv);
    LOAD_GL(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog);
    LOAD_GL(PFNGLCREATEPROGRAMPROC, CreateProgram);
    LOAD_GL(PFNGLATTACHSHADERPROC, AttachShader);
    LOAD_GL(PFNGLLINKPROGRAMPROC, LinkProgram);
    LOAD_GL(PFNGLGETPROGRAMIVPROC, GetProgramiv);
    LOAD_GL(PFNGLUSEPROGRAMPROC, UseProgram);
    LOAD_GL(PFNGLGENBUFFERSPROC, GenBuffers);
    LOAD_GL(PFNGLBINDBUFFERPROC, BindBuffer);
    LOAD_GL(PFNGLBUFFERDATAPROC, BufferData);
    LOAD_GL(PFNGLGENVERTEXARRAYSPROC, GenVertexArrays);
    LOAD_GL(PFNGLBINDVERTEXARRAYPROC, BindVertexArray);
    LOAD_GL(PFNGLGETATTRIBLOCATIONPROC, GetAttribLocation);
    LOAD_GL(PFNGLVERTEXATTRIBPOINTERPROC, VertexAttribPointer);
    LOAD_GL(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray);
    LOAD_GL(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation);
    LOAD_GL(PFNGLUNIFORM1IPROC, Uniform1i);
    LOAD_GL(PFNGLUNIFORM2FPROC, Uniform2f);
    LOAD_GL(PFNGLUNIFORM3FPROC, Uniform3f);
    LOAD_GL(PFNGLACTIVETEXTUREPROC, ActiveTexture);
    #undef LOAD_GL
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


int main(int argc, char* argv[]) {
    if (argc < 2) {
        SDL_Log("Usage: %s <path_to_co_file>", argv[0]);
        return 1;
    }

    // --- 1. Load .CO File ---
    FILE *co_file = fopen(argv[1], "rb");
    if (co_file == NULL) {
        SDL_Log("Could not open CO file '%s'", argv[1]);
        return 1;
    }

    fseek(co_file, 0, SEEK_END);
    long file_size = ftell(co_file);
    fseek(co_file, 0, SEEK_SET);

    if (file_size <= 6) {
        SDL_Log("Invalid CO file size: %ld bytes", file_size);
        fclose(co_file);
        return 1;
    }

    uint8_t *co_file_data = (uint8_t *)allocate((size_t)file_size);
    if (!co_file_data) {
        SDL_Log("Failed to allocate memory for CO file");
        fclose(co_file);
        return 1;
    }

    size_t bytes_read = fread(co_file_data, 1, (size_t)file_size, co_file);
    fclose(co_file);

    if (bytes_read != (size_t)file_size) {
        SDL_Log("Failed to read complete file data");
        free(co_file_data);
        return 1;
    }

    // --- 2. Initialize Emulator State ---
    reset_emulator_with_co_file(co_file_data, (int32_t)file_size);
    free(co_file_data); // Memory was copied into machine.memory

    // --- 3. SDL3 & OpenGL Initialization ---
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
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

    SDL_GL_SetSwapInterval(1); // VSync

    if (!load_gl_extensions()) {
        return 1;
    }

    // --- Shader & Geometry Setup ---
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_src);
    GLuint shaderProgram = gl.CreateProgram();
    gl.AttachShader(shaderProgram, vs);
    gl.AttachShader(shaderProgram, fs);
    gl.LinkProgram(shaderProgram);
    gl.UseProgram(shaderProgram);

    // Full screen quad
    float vertices[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f
    };

    GLuint VAO, VBO;
    gl.GenVertexArrays(1, &VAO);
    gl.GenBuffers(1, &VBO);

    gl.BindVertexArray(VAO);
    gl.BindBuffer(GL_ARRAY_BUFFER, VBO);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

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
    gl.Uniform2f(gl.GetUniformLocation(shaderProgram, "u_resolution"), (float)SCREEN_WIDTH, (float)SCREEN_HEIGHT);

    // Define standard LCD greenish/gray colors for the shader
    gl.Uniform3f(gl.GetUniformLocation(shaderProgram, "u_colorBg"),     168.f/255.f, 172.0f/255.f, 164.f/255.f);
    gl.Uniform3f(gl.GetUniformLocation(shaderProgram, "u_colorPixel"),   41.f/255.f,  42.0f/255.f,  40.f/255.f);
    gl.Uniform3f(gl.GetUniformLocation(shaderProgram, "u_colorShadow"), 129.f/255.f, 130.0f/255.f, 124.f/255.f);

    // --- 4. Main Loop ---
    bool running = true;
    SDL_Event event;

    uint64_t last_time = SDL_GetTicksNS();
    int32_t t_states_excess = 0;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
            else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (!event.key.repeat) {
                    handle_sdl_key(event.key.key, true);
                }
            }
            else if (event.type == SDL_EVENT_KEY_UP) {
                handle_sdl_key(event.key.key, false);
            }
        }

        // --- Delta Time & Dynamic Cycle Execution ---
        uint64_t current_time = SDL_GetTicksNS();
        double delta_time = (double)(current_time - last_time) / 1e9;
        if (delta_time > 0.1) delta_time = 0.1;
        last_time = current_time;

        int32_t t_state_goal = (int32_t)(CPU_CLOCK_FREQ * delta_time) - t_states_excess;

        if (t_state_goal > 0) {
            int32_t res = run(t_state_goal);
            t_states_excess = (res < 0) ? 0 : res;
        }

        // --- Viewport & Uniform Updates ---
        int win_w, win_h;
        SDL_GetWindowSizeInPixels(window, &win_w, &win_h);
        glViewport(0, 0, win_w, win_h);

        // Update AA width uniformly based on scaling to keep pixel boundaries crisp
        float scale_x = (float)win_w / SCREEN_WIDTH;
        float scale_y = (float)win_h / SCREEN_HEIGHT;
        gl.Uniform2f(gl.GetUniformLocation(shaderProgram, "u_aaWidth"), 1.0f / scale_x, 1.0f / scale_y);

        // --- Upload Emulator Display Frame ---
        uint32_t* src = get_canvas_buffer();
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, src);

        // --- Rendering ---
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

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

#endif