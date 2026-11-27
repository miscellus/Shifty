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

#define ARRLEN(arr) (sizeof(arr)/sizeof(*(arr)))

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 64
#define CPU_CLOCK_FREQ 2457600 // 2.4576 MHz

#ifdef TARGET_WEB
#define DECL_EXPORT __attribute__((visibility("default")))
#else
#define DECL_EXPORT
#endif
enum {
    RD = 1 << 0,
    WR = 1 << 1,
    RW = RD | WR
};

// NOTE(jkk):
// X(Name, Address, Direction, b7, b6, b5, b4, b3, b2, b1, b0)
#define PC8201_PORTS(X) \
    X(PortExtVidA,    0x70, RD|WR, x7,         x6,        x5,        x4,         x3,         x2,        x1,        x0       ) \
    X(PortExtVidB,    0x71, RD|WR, x7,         x6,        x5,        x4,         x3,         x2,        x1,        x0       ) \
    X(PortExtVidC,    0x72, RD|WR, x7,         x6,        x5,        x4,         x3,         x2,        x1,        x0       ) \
    X(PortExtVidCmd,  0x73, RD|WR, x7,         x6,        x5,        x4,         x3,         x2,        x1,        x0       ) \
    X(PortRom128KHi,  0x80, RD|WR, x7,         x6,        x5,        x4,         x3,         x2,        x1,        x0       ) \
    X(PortRom128KLo,  0x84, RD|WR, x7,         x6,        x5,        x4,         x3,         x2,        x1,        x0       ) \
    X(PortRom128KMid, 0x88, RD|WR, x7,         x6,        x5,        x4,         x3,         x2,        x1,        x0       ) \
    X(PortRom128KRead,0x8C, RD|WR, x7,         x6,        x5,        x4,         x3,         x2,        x1,        x0       ) \
    X(PortSystemCtl,  0x90,    WR, SerSelH,    SerSelL,   PrintStrb, ClkCmdStrb, CasMotCtl,  x2,        x1,        x0       ) \
    X(PortBankStat,   0xA0, RD   , x7,         x6,        x5,        x4,         StHADR2,    StHADR1,   StLADR2,   StLADR1  ) \
    X(PortBankSel,    0xA1,    WR, x7,         x6,        x5,        x4,         HADR2,      HADR1,     LADR2,     LADR1    ) \
    X(Port81C55Cmd,   0xB8,    WR, TmrCmd1,    TmrCmd0,   PortCCmd1, PortCCmd0,  PortBIntE,  PortAIntE, PortBMode, PortAMode) \
    X(Port81C55Stat,  0xB8, RD   , TmrIntr,    TmrRun,    PortCIntr, PortBIntr,  PortAIntr,  PortCIntE, PortBIntE, PortAIntE) \
    X(Port81C55A,     0xB9, RD|WR, A7,         A6,        A5,        A4,         A3,         A2,        A1,        A0       ) \
    X(Port81C55B,     0xBA, RD|WR, notRTS,     notDTR,    Bell,      AutPwrOff,  DcdRdSel,   MelodyCtl, LcdDrv9,   LcdDrv8  ) \
    X(Port81C55C,     0xBB, RD   , x7,         x6,        notDsr,    notCts,     BcrInput,   PrintBusy, Select,    ClkInput ) \
    X(PortTimerLow,   0xBC, RD|WR, TimerL7,    TimerL6,   TimerL5,   TimerL4,    TimerL3,    TimerL2,   TimerL1,   TimerL0  ) \
    X(PortTimerHigh,  0xBD, RD|WR, TimerMode1, TimerMode0,TimerH5,   TimerH4,    TimerH3,    TimerH2,   TimerH1,   TimerH0  ) \
    X(PortUartData,   0xC8, RD|WR, UartData7,  UartData6, UartData5, UartData4,  UartData3,  UartData2, UartData1, UartData0  ) \
    X(PortUartCmd,    0xD8,    WR, x7,         x6,        x5,        CalLenSelH, CalLenSelL, ParityInhb,OddEvenPar,StopBitSel) \
    X(PortUartStat,   0xD8, RD   , LowPower,   x6,        x5,        TxBufEmpty, ParityErr,  FrameErr,  OverrunErr,DcdRd    ) \
    X(PortKeyIn,      0xE8, RD   , KeyData7,   KeyData6,  KeyData5,  KeyData4,   KeyData3,   KeyData2,  KeyData1,  KeyData0 ) \
    X(PortLcdCmd,     0xFE, RD|WR, x7,         x6,        x5,        x4,         x3,         x2,        x1,        x0       ) \
    X(PortLcdData,    0xFF, RD|WR, LcdData7,   LcdData6,  LcdData5,  LcdData4,   LcdData3,   LcdData2,  LcdData1,  LcdData0 ) \
    /*end*/

typedef enum {
#define X(name, val, dir, ...) name = val,
    PC8201_PORTS(X)
#undef X
} Pc8201Port;

typedef enum {
#define X(name, val, dir, b0, b1, b2, b3, b4, b5, b6, b7) \
    name##0 = (val << 3) | 0, name##_##b0 = name##0, \
    name##1 = (val << 3) | 1, name##_##b1 = name##1, \
    name##2 = (val << 3) | 2, name##_##b2 = name##2, \
    name##3 = (val << 3) | 3, name##_##b3 = name##3, \
    name##4 = (val << 3) | 4, name##_##b4 = name##4, \
    name##5 = (val << 3) | 5, name##_##b5 = name##5, \
    name##6 = (val << 3) | 6, name##_##b6 = name##6, \
    name##7 = (val << 3) | 7, name##_##b7 = name##7,
    PC8201_PORTS(X)
#undef X
} Pc8201PortBits;

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
        case 1:  break; // HADR = 1 ("NOT USED")
        case 2:  mem->pages[1] = (MemPage){mem->ram_banks[1], true}; break;
        case 3:  mem->pages[1] = (MemPage){mem->ram_banks[2], true}; break;
    }
}

enum { MAX_TIMER_COUNT = (1 << 14) - 1 };
typedef enum {
    TIMER_SINGLE_SQUARE = 0,   // ceil(count/2) ticks HIGH, then floor(count/2) ticks LOW, not repeated
    TIMER_REPEATED_SQUARE = 1, // ceil(count/2) ticks HIGH, then floor(count/2) ticks LOW, repeated
    TIMER_SINGLE_PULSE = 2,    // |L|L count - 1 ticks LOW, then 1 tick HIGH, not repeated
    TIMER_REPEATED_PULSE = 3, // count - 1 ticks LOW, then 1 tick HIGH, repeated
} Timer_Mode;

typedef struct {

        // NOTE(jkk): I learned from reading service manual and tech ref that
        // the timer input is the synchronized to the CPU clock and the timer
        // output is determined by the mode.

        // The timer output is connected to the buzzer but gated by some lines
        // that are controlled by other registers in the 80C55.

        uint64_t time_next_period; // total T states
        uint16_t target_count;
        uint16_t counter; // Only 14 bits
        Timer_Mode mode;
} Ic_80C55_Timer;

typedef enum Ic_Clock_Command {
    CLOCK_CMD_REGISTER_HOLD = 0,
    CLOCK_CMD_REGISTER_SHIFT = 1,
    CLOCK_CMD_TIME_SET_COUNTER_HOLD = 2,
    CLOCK_CMD_TIME_READ = 3,

    CLOCK_CMD_FREQ_64 = 4,
    CLOCK_CMD_FREQ_256 = 5,
    CLOCK_CMD_FREQ_2048 = 6,
    CLOCK_CMD_TEST_MODE = 7,
} Ic_Clock_Command;

typedef struct {
    uint64_t time_interval_ns;
    uint64_t time_next_pulse_ns;

    uint64_t shift_register; // 40 bits
} Ic_Clock;

#define FREQ_PERIOD_NS(FREQ) (1000000000/(FREQ))

void clock_command(Ic_Clock *clock, Ic_Clock_Command command, bool shift_on, uint64_t data_bit) {
    (void)shift_on;
    (void)data_bit;

    switch (command) {
    case CLOCK_CMD_REGISTER_HOLD:         /*printf*/(void)("TODO:CLOCK_CMD_REGISTER_HOLD\n"); break;
    case CLOCK_CMD_REGISTER_SHIFT:        /*printf*/(void)("TODO:CLOCK_CMD_REGISTER_SHIFT\n"); break;
    case CLOCK_CMD_TIME_SET_COUNTER_HOLD: /*printf*/(void)("TODO:CLOCK_CMD_TIME_SET_COUNTER_HOLD\n"); break;
    case CLOCK_CMD_TIME_READ:             /*printf*/(void)("TODO:CLOCK_CMD_TIME_READ\n"); break;
    case CLOCK_CMD_FREQ_64:               clock->time_interval_ns = FREQ_PERIOD_NS(64); break;
    case CLOCK_CMD_FREQ_256:              clock->time_interval_ns = FREQ_PERIOD_NS(256); break;
    case CLOCK_CMD_FREQ_2048:             clock->time_interval_ns = FREQ_PERIOD_NS(2048); break;
    case CLOCK_CMD_TEST_MODE: assert(!"CLOCK_CMD_TEST_MODE not emulated"); break;
    }
}


#define AUDIO_SAMPLE_BUFFER_SIZE      1024
#define AUDIO_SAMPLE_RATE             22050
#define AUIDO_SAMPLE_PERIOD_NS        FREQ_PERIOD_NS(AUDIO_SAMPLE_RATE)

typedef struct {
    uint64_t time_next_sample_ns;
    float sample_buffer[AUDIO_SAMPLE_BUFFER_SIZE];
    uint32_t sample_count;

    bool PB5;
    bool PB2;
} Audio;

typedef struct {

    uint64_t clock_period_ns;
    uint64_t time_ns;

    Vm_8085 cpu;
    Pc8201Lcd lcd;
    uint32_t canvas[SCREEN_WIDTH * SCREEN_HEIGHT];

    Memory mem;

    Pc8201Keyboard key;

    Ic_80C55_Timer timer;

    Ic_Clock clock;

    Audio audio;

    uint8_t ports_prev[256];

    bool cassette_motor_on;
    bool clock_command_strobe;
    bool printer_strobe;
    SerialInterfaceSelection serial_interface_selection;

    // Debugger support
    BreakPointKind breakpoints[65536];
    DebuggerLoc address_to_line[65536];
} Pc8201Machine;

// Globally instantiate our machine state
static Pc8201Machine machine;

static inline uint8_t memory_read(Memory *mem, uint16_t address) {
    uint8_t idx = address >> 15;
    return mem->pages[idx].base[address & 0x7FFF];
}

static inline void memory_write(Memory *mem, uint16_t address, uint8_t data) {
    uint8_t idx = address >> 15;
    if (mem->pages[idx].writeable) {
        mem->pages[idx].base[address & 0x7FFF] = data;
    }
    else {
        printf("!!!! [%#04x] = %#02x (PC = %#04x)\n", address, data, machine.cpu.pc);
    }
}

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

static const char* string_from_port(uint8_t port, uint8_t dir) {
#define X(NAME, VAL, DIR, ...) if ((port == (VAL)) && (dir & (DIR))) return #NAME;
        PC8201_PORTS(X)
#undef X
    return "UnknownPort";
}

static const char *string_from_port_bit(uint8_t port, uint8_t bit, uint8_t dir) {
#define X(NAME, VAL, DIR, B0, B1, B2, B3, B4, B5, B6, B7) \
    if ((port == (VAL)) && (dir & (DIR)) && (bit == 0)) return #B0; \
    if ((port == (VAL)) && (dir & (DIR)) && (bit == 1)) return #B1; \
    if ((port == (VAL)) && (dir & (DIR)) && (bit == 2)) return #B2; \
    if ((port == (VAL)) && (dir & (DIR)) && (bit == 3)) return #B3; \
    if ((port == (VAL)) && (dir & (DIR)) && (bit == 4)) return #B4; \
    if ((port == (VAL)) && (dir & (DIR)) && (bit == 5)) return #B5; \
    if ((port == (VAL)) && (dir & (DIR)) && (bit == 6)) return #B6; \
    if ((port == (VAL)) && (dir & (DIR)) && (bit == 7)) return #B7; \
    /*end*/
    PC8201_PORTS(X)
#undef X
    return "UnknownPortBit";
}

static void mem_cb(Vm_8085 *vm, uint16_t addr, bool is_write, uint8_t *in_out_data)
{
    Pc8201Machine *mach = (Pc8201Machine *)vm->user_data;

    if (is_write) {
        memory_write(&mach->mem, addr, *in_out_data);
        assert(memory_read(&mach->mem, addr) == *in_out_data);
    }
    else {
        *in_out_data = memory_read(&mach->mem, addr);
    }
}

#include <math.h>

static void loop_cb(Vm_8085 *vm) {
    Pc8201Machine *mach = (Pc8201Machine *)vm->user_data;

    // Update time
    mach->time_ns = mach->cpu.total_t_states * mach->clock_period_ns;

    // Calculate time from total_t_states
    for (; mach->audio.time_next_sample_ns < mach->time_ns && mach->audio.sample_count < AUDIO_SAMPLE_BUFFER_SIZE
         ; mach->audio.time_next_sample_ns += AUIDO_SAMPLE_PERIOD_NS)
    {
        static float phase = 0.0f;

        float sample_value = mach->audio.PB5 ? 1.0f : 0.0f;

        if (mach->audio.PB5 && !mach->audio.PB2) {
            if (mach->timer.target_count > 0) {
                float frequency = 1e9 / (mach->timer.target_count * mach->clock_period_ns);
                float phase_step = frequency / (float)AUDIO_SAMPLE_RATE;

                // Generate square wave from timer frequency
                phase = fmodf(phase + phase_step, 1.0f);
                sample_value = (phase > 0.5f ? 1.0f : 0.0f);
            }
        }

        mach->audio.sample_buffer[mach->audio.sample_count++] = sample_value * 0.2f;
    }

    while (mach->cpu.total_t_states >= mach->timer.time_next_period) {
        uint64_t t = mach->cpu.total_t_states - mach->timer.time_next_period;
        mach->timer.counter = t % mach->timer.target_count;
        mach->timer.time_next_period += mach->timer.target_count;
    }

    // Calendar clock pulses
    while (mach->time_ns >= mach->clock.time_next_pulse_ns) {
        mach->clock.time_next_pulse_ns += mach->clock.time_interval_ns;

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

    uint8_t handled = 0;
    enum {
        B0 = 0x01,
        B1 = 0x02,
        B2 = 0x04,
        B3 = 0x08,
        B4 = 0x10,
        B5 = 0x20,
        B6 = 0x40,
        B7 = 0x80,
    };

    switch (port) {

    case PortSystemCtl: {

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
            handled =  B7 | B6 | B5 | B4 | B3 | !B2 | !B1 | !B0;
        }
        else {

        }
    } break;

    case PortBankStat: { // 0xA0
        if (is_write) {

        }
        else /* read */ {

            *in_out_data = (
                mach->mem.low_address_select |
                (mach->mem.high_address_select << 2) |
                (mach->serial_interface_selection << 6));

            handled = B7 | B6 | !B5 | !B6 | B3 | B2 | B1 | B0;
        }
    } break;

    case PortBankSel: { // 0xA1
        if (is_write) {
            memory_bank_set(&mach->mem, data);
            handled =  B7 | B6 | B5 | B4 | B3 | B2 | B1 | B0;
        }
    } break;

    case Port81C55A: {
        if (is_write) {
            mach->key.strobe_a = data;

            lcd->driver_select &= 0xff00;
            lcd->driver_select |= data;

            Ic_Clock_Command cmd = data & 3;
            bool shift_clock_on = !!((data >> 3) & 1);
            uint64_t data_bit = (data >> 4) & 1;
            clock_command(&mach->clock, cmd, shift_clock_on, data_bit);

            handled = !B7 | !B6 | !B5 | B4 | B3 | B2 | B1 | B0;
        }
    } break;
 
    case Port81C55B: {
        if (is_write) {
            mach->key.strobe_b = data; // Track keyboard strobe (PB0)

            // LCD driver select lines (PB1 ~ PB0)[cite: 1]
            lcd->driver_select &= 0x00ff;
            lcd->driver_select |= (data & 0x3) << 8;

            // // Bit 2: MELODY CONTROL OUTPUT (MC) - 0 = On, 1 = Off[cite: 1]
            // mach->audio.silence_tone = (data & (1 << 2)) != 0;

            bool new_pb2 = (data & (1 << 2)) != 0;
            bool new_pb5 = (data & (1 << 5)) != 0;


            mach->audio.PB2 = new_pb2;
            mach->audio.PB5 = new_pb5;

            // // Bit 5: BUZZER OUTPUT (BELL) - 0 = Ring, 1 = Not Ring[cite: 1]
            // mach->audio.buzzer_on = (data & (1 << 5)) != 0;

            handled = !B7 | !B6 | B5 | !B4 | !B3 | B2 | B1 | B0;


            // mach->audio.melody_control_output = (data & (1 << 2)) == 0; // active low

            // // Bit 3: DCD/RD SELECT OF RS-232C - 0 = Ring Detect, 1 = Data Carrier Detect[cite: 1]
            // // mach->dcd_rd_select = (data & (1 << 3)) != 0;

            // // Bit 4: AUTO POWER OFF OUTPUT (APO) - 0 = Output Off, 1 = Output On[cite: 1]
            // // mach->auto_power_off = (data & (1 << 4)) != 0;

            // mach->audio.buzzer_on = (data & (1 << 5)) == 0;

            // Bits 6 & 7: RS-232C DTR and RTS (Active Low)[cite: 1]
            // bool dtr_active = (data & (1 << 6)) == 0;
            // bool rts_active = (data & (1 << 7)) == 0;

            // // Apply serial signal changes
            // uint8_t ser_flags = 0;
            // if (dtr_active) ser_flags |= SER_SIGNAL_DTR;
            // if (rts_active) ser_flags |= SER_SIGNAL_RTS;
            // ser_set_signals(ser_flags);

            // Handle Auto Power Off change if needed
            // if (!mach->auto_power_off) {
            //     // power_down();
            // }
        }
        else /* read */ {
            *in_out_data = (lcd->driver_select >> 8) & 0x3;

            handled =  !B7 | !B6 | !B5 | !B4 | !B3 | !B2 | B1 | B0;
        }

    } break;

    case Port81C55C: {
        if (is_write) {
            // TODO
        }
        else /* read */ {
            // Clear Bit 2 (Printer Busy = 0, meaning ready)
            // Adjust other bits as needed for CTS/DSR/etc.
            *in_out_data = 0xF8;
            handled =  B7 | B6 | B5 | B4 | B3 | !B2 | !B1 | !B0;
        }
    } break;

    case Port81C55Cmd: {
        if (is_write) {

        }
        else /* read */ {
            // *in_out_data = mach->ports_prev[Port81C55Cmd];
        }
    } break;

    case PortTimerLow: {
        if (is_write) {
            mach->timer.target_count = (mach->timer.target_count & 0xff00) | data;
            handled =  B7 | B6 | B5 | B4 | B3 | B2 | B1 | B0;
        }
        else /* read */ {
            *in_out_data = mach->timer.target_count & 0xff;
            handled =  B7 | B6 | B5 | B4 | B3 | B2 | B1 | B0;
        }
    } break;

    case PortTimerHigh: {
        if (is_write) {
            mach->timer.target_count = (mach->timer.target_count & 0xff) | (((uint16_t)(data & 0x3f)) << 8);
            mach->timer.mode = (data >> 6) & 3;

            printf("TIMER: frequency: % .2f\n", 1e9f / (mach->timer.target_count * mach->clock_period_ns));
            handled = B7 | B6 | B5 | B4 | B3 | B2 | B1 | B0;
        }
        else /* read */ {
            *in_out_data = (
                ((mach->timer.target_count >> 8) & 0x3f) |
                ((mach->timer.mode & 3) << 6)
            );
            handled =  B7 | B6 | B5 | B4 | B3 | B2 | B1 | B0;
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

        handled = B7 | B6 | B5 | B4 | B3 | B2 | B1 | B0;
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

        handled = B7 | B6 | B5 | B4 | B3 | B2 | B1 | B0;
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

            handled = B7 | B6 | B5 | B4 | B3 | B2 | B1 | B0;
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

    if (false && handled != 0xFF) {

        uint8_t dir = is_write?2:1;
        const char *portstr = string_from_port(port, dir);
        uint64_t t = mach->cpu.total_t_states;
        if (!is_write) data = *in_out_data;
        fprintf(stderr, "@: %04X T: %"PRIu64" - IO %s port %02X (%15.20s), value = %02x ("BINPAT");\n", mach->cpu.pc, t, is_write ? "WRITE" : "READ ", port, portstr, data, BYTE_TO_BIN(data));

        for (int i = 0; i < 8; ++i)
        {
            if (handled & (1 << i)) continue;
            fprintf(stderr, "  %s\n", string_from_port_bit(port, (uint8_t)i, dir));
        }
    }

    return handled;
}

void serial_cb(Vm_8085 *vm, bool write, bool *in_out_bit)
{
    (void)vm;
    (void)write;
    (void)in_out_bit;
}

#ifdef TARGET_DESKTOP
#include <stdlib.h>
#endif

DECL_EXPORT void reset_emulator(uint64_t clock_frequency) {
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

    machine.timer.target_count = 16384;
    machine.clock.time_interval_ns = FREQ_PERIOD_NS(256); // 256 herz is the default for PC-8201

    machine.clock_period_ns = FREQ_PERIOD_NS(clock_frequency);
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
    reset_emulator(CPU_CLOCK_FREQ);
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

typedef struct {
    int x, y, w, h; /* Bottom-up OpenGL viewport coordinates (y = 0 at bottom) */
} IntRect;

typedef struct {
    float x, y, w, h;
} FloatRect;

static inline bool PointInsideFloatRect(float x, float y, FloatRect rect) {
    return x >= rect.x && x < rect.x + rect.w
        && y >= rect.y && y < rect.y + rect.h;
}

typedef struct {
    bool debugger_enabled;
    float lcd_top_ratio;   /* Fraction of window height reserved for top LCD area, e.g. 0.28f */
    IntRect lcd_rect;
    IntRect code_rect;
} Layout;

#include "gl.h"
#include "debugger.c"

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


#define SAMPLE_RATE 22050

static void update_audio(Pc8201Machine *mach, SDL_AudioStream *stream)
{
    if (mach->audio.sample_count != AUDIO_SAMPLE_BUFFER_SIZE) return;

    size_t byte_count = AUDIO_SAMPLE_BUFFER_SIZE * sizeof(mach->audio.sample_buffer[0]);


    if (!SDL_PutAudioStreamData(stream, mach->audio.sample_buffer, byte_count)) {
        fprintf(stderr, "SDL_PutAudioStreamData: %s\n", SDL_GetError());
    }

    mach->audio.sample_count = 0;
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

static void compute_layout(Layout *layout, int window_w, int window_h) {
    if (window_w < 1) window_w = 1;
    if (window_h < 1) window_h = 1;

    if (!layout->debugger_enabled) {
        /* Normal mode: letterboxed LCD centered across the full window */
        float scale_w = (float)window_w / (float)SCREEN_WIDTH;
        float scale_h = (float)window_h / (float)SCREEN_HEIGHT;
        float scale = (scale_w < scale_h) ? scale_w : scale_h;

        int vw = (int)((float)SCREEN_WIDTH * scale);
        int vh = (int)((float)SCREEN_HEIGHT * scale);

        layout->lcd_rect.w = vw;
        layout->lcd_rect.h = vh;
        layout->lcd_rect.x = (window_w - vw) / 2;
        layout->lcd_rect.y = (window_h - vh) / 2;

        layout->code_rect = (IntRect){0, 0, 0, 0};
        return;
    }

    /* Debugger mode: LCD at TOP of window, source viewer fills BOTTOM */
    int top_band_h = (int)((float)window_h * layout->lcd_top_ratio);
    if (top_band_h < SCREEN_HEIGHT) {
        top_band_h = (SCREEN_HEIGHT < window_h) ? SCREEN_HEIGHT : window_h;
    }
    int bottom_band_h = window_h - top_band_h;
    if (bottom_band_h < 0) bottom_band_h = 0;

    /* Letterbox LCD inside the top band, centered horizontally and vertically in that band */
    float scale_w = (float)window_w / (float)SCREEN_WIDTH;
    float scale_h = (float)top_band_h / (float)SCREEN_HEIGHT;
    float scale = (scale_w < scale_h) ? scale_w : scale_h;
    if (scale <= 0.0f) scale = 1.0f;

    int lcd_w = (int)((float)SCREEN_WIDTH * scale);
    int lcd_h = (int)((float)SCREEN_HEIGHT * scale);
    int lcd_x = (window_w - lcd_w) / 2;
    /* Top of screen in GL coords is window_h; top band sits from (window_h - top_band_h) to window_h */
    int lcd_y = (window_h - top_band_h) + (top_band_h - lcd_h) / 2;

    layout->lcd_rect = (IntRect){lcd_x, lcd_y, lcd_w, lcd_h};
    layout->code_rect = (IntRect){0, 0, window_w, bottom_band_h};
}

#define arg_shift(argc, argv) ((argc) > 0 ? (--(argc), *(argv)++) : NULL)

int main(int argc, char* argv[]) {

    const char *program = arg_shift(argc, argv);
    (void)program;

    const char *co_file_path = NULL;
    const char *rom_file_path = NULL;
    const char *dbg_map_path = NULL;

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
                SDL_Log("Missing argument <CO File path>.\n");
                return 1;
            }
        }
        else if (strcmp(arg, "dbg-map") == 0) {
            if (!(dbg_map_path = arg_shift(argc, argv))) {
                SDL_Log("Missing argument <Debug map file path>.\n");
                return 1;
            }
        }
        else {
            SDL_Log("Unknown argument: %s\n", arg);
        }
    }

    reset_emulator(CPU_CLOCK_FREQ);

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

    Dbg dbg = {0};
    if (dbg_map_path) {
        dbg_init(&dbg, &machine, dbg_map_path);
    }

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_src);
    GLuint shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vs);
    glAttachShader(shaderProgram, fs);
    glLinkProgram(shaderProgram);
    glUseProgram(shaderProgram);

    GLuint VAO, VBO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);

    float full_screen_quad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f,  1.0f, 1.0f,  1.0f};
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(full_screen_quad), full_screen_quad, GL_STATIC_DRAW);

    GLint posAttrib = glGetAttribLocation(shaderProgram, "a_position");
    glEnableVertexAttribArray(posAttrib);
    glVertexAttribPointer(posAttrib, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);

    // Texture Setup
    GLuint texture;
    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);

    // GL_NEAREST ensures the shader accurately calculates pixel locations
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Create an empty texture buffer (RGBA8888 maps perfectly to GL_RGBA/GL_UNSIGNED_BYTE)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SCREEN_WIDTH, SCREEN_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    // Set Static Uniforms
    glUniform1i(glGetUniformLocation(shaderProgram, "u_image"), 0);
    glUniform2f(glGetUniformLocation(shaderProgram, "u_resolution"), (float)SCREEN_WIDTH + 4, (float)SCREEN_HEIGHT + 4);

    // Define standard LCD greenish/gray colors for the shader
    glUniform3f(glGetUniformLocation(shaderProgram, "u_colorBg"),     168.f/255.f, 172.0f/255.f, 164.f/255.f);
    glUniform3f(glGetUniformLocation(shaderProgram, "u_colorPixel"),   41.f/255.f,  42.0f/255.f,  40.f/255.f);
    glUniform3f(glGetUniformLocation(shaderProgram, "u_colorShadow"), 129.f/255.f, 130.0f/255.f, 124.f/255.f);

    Layout layout = {0};
    layout.lcd_top_ratio = 0.25;

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
                    if (event.key.key == SDLK_F12 && event.type == SDL_EVENT_KEY_DOWN) {
                        layout.debugger_enabled = !layout.debugger_enabled;
                    }
                    else {
                        handle_sdl_key(&event.key);
                    }
                }
            }
            else if (layout.debugger_enabled && layout.code_rect.h > 0) {
                int win_w = 0, win_h = 0;
                SDL_GetWindowSizeInPixels(window, &win_w, &win_h);
                float code_top_y = (float)(win_h - (layout.code_rect.y + layout.code_rect.h));
                FloatRect area = { layout.code_rect.x, code_top_y, layout.code_rect.w, layout.code_rect.h };
                dbg_handle_event(&dbg, &event, area);
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
        update_audio(&machine, audio_stream);

        int window_w, window_h;
        SDL_GetWindowSizeInPixels(window, &window_w, &window_h);

        compute_layout(&layout, window_w, window_h);

        // Clear the ENTIRE window to black before drawing the restricted viewport
        // (We enable the scissor test briefly to clear outside the glViewport boundaries)
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, window_w, window_h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Draw emulated LCD in its target viewport
        float lcd_scale = (float)layout.lcd_rect.w / (float)SCREEN_WIDTH;
        if (lcd_scale <= 0.0f) lcd_scale = 1.0f;

        glViewport(layout.lcd_rect.x, layout.lcd_rect.y, layout.lcd_rect.w, layout.lcd_rect.h);

        glUseProgram(shaderProgram);
        glUniform2f(glGetUniformLocation(shaderProgram, "u_aaWidth"), 1.0f / lcd_scale, 1.0f / lcd_scale);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT,
                        GL_RGBA, GL_UNSIGNED_BYTE, get_canvas_buffer());

        glBindVertexArray(VAO);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        if (layout.debugger_enabled && layout.code_rect.h > 0) {
            glViewport(layout.code_rect.x, layout.code_rect.y, layout.code_rect.w, layout.code_rect.h);
            dbg_draw_source(&dbg, (float)layout.code_rect.w, (float)layout.code_rect.h);
        }

        SDL_GL_SwapWindow(window);
    }

    // Cleanup
    dbg_free(&dbg);
    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

#endif // TARGET_DESKTOP
