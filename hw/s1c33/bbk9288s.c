/*
 * BBK 9288S research machine.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "cpu.h"
#include "exec/cpu-common.h"
#include "exec/log.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/loader.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/rtc.h"
#include "system/system.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/pixel_ops.h"

#define TYPE_BBK9288S_MACHINE   MACHINE_TYPE_NAME("bbk9288s")
OBJECT_DECLARE_SIMPLE_TYPE(BBK9288SMachineState, BBK9288S_MACHINE)

#define BBK9288S_SDRAM_BASE     0x02000000u
#define BBK9288S_DEFAULT_RAM    (8 * MiB)
#define BBK9288S_IRAM_SIZE      (16 * KiB)
#define BBK9288S_IRAM_RTC_COOKIE_OFFSET (BBK9288S_IRAM_SIZE - 4)
#define BBK9288S_BOOT_STACK_TOP 0x00003f80u
#define BBK9288S_BOARD_IO_BASE  0x01000000u
#define BBK9288S_BOARD_IO_SIZE  0x00010000u
#define BBK9288S_BOARD_CTRL     0x82
#define BBK9288S_BOARD_DATA     0x83
#define BBK9288S_BOARD_STATUS_H 0x84
#define BBK9288S_BOARD_STATUS_L 0x85
#define BBK9288S_IO_BASE        0x00040000u
#define BBK9288S_IO_SIZE        0x00010000u
#define BBK9288S_TOUCH_IO_BASE  0x00300000u
#define BBK9288S_TOUCH_IO_SIZE  0x00000200u
#define BBK9288S_TOUCH_SERIAL   0x20
#define BBK9288S_TOUCH_CONFIG   0x49
#define BBK9288S_TOUCH_CLK      0x80
#define BBK9288S_TOUCH_MISO     0x10
#define BBK9288S_TOUCH_MOSI     0x08
#define BBK9288S_NAND_DATA_BASE 0x04000000u
#define BBK9288S_NAND_DATA_SIZE 0x00001000u
#define BBK9288S_NAND_CE_SELECT 0x100
#define BBK9288S_NAND_ECC_READY 0x101
#define BBK9288S_NAND_ECC_ENABLE 0x102
#define BBK9288S_NAND_MODE      0x103
#define BBK9288S_NAND_ECC_BASE  0x104
#define BBK9288S_NAND_ECC_SIZE  6
#define BBK9288S_NAND_CLE       0x10
#define BBK9288S_NAND_ALE       0x20
#define BBK9288S_NAND_READY     0x40
#define BBK9288S_NAND_MFR_ID    0xec
#define BBK9288S_NAND_DEVICE_ID 0xda
#define BBK9288S_NAND_PAGE_SIZE 2048
#define BBK9288S_NAND_OOB_SIZE  64
#define BBK9288S_NAND_RAW_PAGE_SIZE \
    (BBK9288S_NAND_PAGE_SIZE + BBK9288S_NAND_OOB_SIZE)
#define BBK9288S_NAND_PAGES_PER_BLOCK 64
#define BBK9288S_NAND_BLOCKS    2048
#define BBK9288S_NAND_PAGE_COUNT \
    (BBK9288S_NAND_PAGES_PER_BLOCK * BBK9288S_NAND_BLOCKS)
#define BBK9288S_NAND_STORAGE_SIZE \
    ((size_t)BBK9288S_NAND_PAGE_COUNT * BBK9288S_NAND_RAW_PAGE_SIZE)
#define BBK9288S_NAND_RAW_BLOCK_SIZE \
    (BBK9288S_NAND_RAW_PAGE_SIZE * BBK9288S_NAND_PAGES_PER_BLOCK)
#define BBK9288S_NAND_BLOCK_DATA_SIZE \
    (BBK9288S_NAND_PAGE_SIZE * BBK9288S_NAND_PAGES_PER_BLOCK)
#define BBK9288S_NAND_RESERVED_BLOCKS 40
#define BBK9288S_NAND_LOGICAL_BLOCKS  1968
#define BBK9288S_FAT_SECTOR_SIZE      512
#define BBK9288S_FAT_MAX_DEPTH        8
#define BBK9288S_FAT_CLUSTER_END      0xfff8
#define BBK9288S_LCDC_IO_BASE   0x00380000u
#define BBK9288S_LCDC_IO_SIZE   0x00010000u
#define BBK9288S_IVRAM_BASE     0x003c0000u
#define BBK9288S_IVRAM_SIZE     (40 * KiB)
#define BBK9288S_KERNEL_HEADER  0x40
#define BBK9288S_LCD_WIDTH      160
#define BBK9288S_LCD_HEIGHT     240
#define BBK9288S_LCD_BPP        2
#define BBK9288S_LCD_STRIDE     (BBK9288S_LCD_WIDTH * BBK9288S_LCD_BPP / 8)
#define BBK9288S_MMIO_TRACE_LEN 64
#define BBK9288S_TBRP_OFFSET    0x812d
#define BBK9288S_TTBR_OFFSET    0x8134
#define BBK9288S_USB_IRQ_BIT    0x40
#define BBK9288S_ADC_DATA_LO    0x0240
#define BBK9288S_ADC_DATA_HI    0x0241
#define BBK9288S_ADC_TRIGGER    0x0242
#define BBK9288S_ADC_CHANNEL    0x0243
#define BBK9288S_ADC_ENABLE     0x0244
#define BBK9288S_ADC_SAMPLING   0x0245
#define BBK9288S_ADC_FLAGS      0x0246
#define BBK9288S_ADC_OVERWRITE  0x0247
#define BBK9288S_ADC_BUFFER_BASE 0x0248
#define BBK9288S_ADC_CHANNELS   8
#define BBK9288S_ADC_BATTERY_OK 0x03c0
#define BBK9288S_ADC_UPPER_LO   0x0258
#define BBK9288S_ADC_UPPER_HI   0x0259
#define BBK9288S_ADC_LOWER_LO   0x025a
#define BBK9288S_ADC_LOWER_HI   0x025b
#define BBK9288S_ADC_INTMASK    0x025c
#define BBK9288S_ADC_MODE       0x025f
#define BBK9288S_PIR_PORT0_1    0x0260
#define BBK9288S_PIR_PORT2_3    0x0261
#define BBK9288S_PIR_KEY_PORT0_3 0x0262
#define BBK9288S_PIR_SIF1_ADC   0x026a
#define BBK9288S_PIR2_16TM0_1   0x0266
#define BBK9288S_PIR3_16TM2_3   0x0267
#define BBK9288S_PIR4_16TM4_5   0x0268
#define BBK9288S_PIR_8TM_SIF0   0x0269
#define BBK9288S_PIR_PORT4_5    0x026c
#define BBK9288S_PIR_PORT6_7    0x026d
#define BBK9288S_EIR0_KEY_PORT0_3 0x0270
#define BBK9288S_EIR3_16TM2_3   0x0273
#define BBK9288S_EIR4_16TM4_5   0x0274
#define BBK9288S_EIR5_8TM0_3    0x0275
#define BBK9288S_EIR7_PORT4_7_CTM_ADC 0x0277
#define BBK9288S_FIR0_KEY_PORT0_3 0x0280
#define BBK9288S_FIR3_16TM2_3   0x0283
#define BBK9288S_FIR4_16TM4_5   0x0284
#define BBK9288S_FIR5_8TM0_3    0x0285
#define BBK9288S_FIR7_PORT4_7_CTM_ADC 0x0287
#define BBK9288S_ADC_IRQ_BIT    0x01
#define BBK9288S_ADC_VECTOR     64
#define BBK9288S_PORT0_VECTOR   12
#define BBK9288S_KEY0_IRQ_BIT   0x10
#define BBK9288S_KEY1_IRQ_BIT   0x20
#define BBK9288S_KEY0_VECTOR    16
#define BBK9288S_KEY1_VECTOR    17
#define BBK9288S_EIR2_16TM0_1   0x0272
#define BBK9288S_FIR2_16TM0_1   0x0282
#define BBK9288S_16TM0A_IRQ_BIT 0x08
#define BBK9288S_16TM0A_VECTOR  25
#define BBK9288S_16TM0B_IRQ_BIT 0x04
#define BBK9288S_16TM0B_VECTOR  24
#define BBK9288S_16TM2B_IRQ_BIT 0x04
#define BBK9288S_16TM2B_VECTOR  38
#define BBK9288S_16TM3B_IRQ_BIT 0x40
#define BBK9288S_16TM3B_VECTOR  42
#define BBK9288S_16TM3B_WAKE_MS 10
#define BBK9288S_8TM0_3_VECTOR  53
#define BBK9288S_8TM0_3_MASK    0x0f
#define BBK9288S_8TM1_IRQ_BIT   0x02
#define BBK9288S_8TM_CTRL_BASE  0x0160
#define BBK9288S_8TM_STRIDE     4
#define BBK9288S_8TM_CHANNELS   4
#define BBK9288S_8TM_UNDERFLOW_MS 10
#define BBK9288S_CTM_RUN_CTRL   0x0151
#define BBK9288S_CTM_INT_CTRL   0x0152
#define BBK9288S_CTM_DIVIDER    0x0153
#define BBK9288S_CTM_SEC        0x0154
#define BBK9288S_CTM_MIN        0x0155
#define BBK9288S_CTM_HOUR       0x0156
#define BBK9288S_CTM_DAY_LO     0x0157
#define BBK9288S_CTM_DAY_HI     0x0158
#define BBK9288S_CTM_MIN_CMP    0x0159
#define BBK9288S_CTM_HOUR_CMP   0x015a
#define BBK9288S_CTM_DAY_CMP    0x015b
#define BBK9288S_CTM_COUNTER_BASE BBK9288S_CTM_SEC
#define BBK9288S_CTM_COUNTER_SIZE 5
#define BBK9288S_CTM_TCRUN      0x01
#define BBK9288S_CTM_TCRST      0x02
#define BBK9288S_CTM_TCAF       0x01
#define BBK9288S_CTM_TCIF       0x02
#define BBK9288S_CTM_SELECT_MASK 0xfc
#define BBK9288S_CTM_TICKS_PER_SECOND 32
#define BBK9288S_CTM_TICK_MS    31
#define BBK9288S_CTM_EV_32HZ    0x01
#define BBK9288S_CTM_EV_8HZ     0x02
#define BBK9288S_CTM_EV_2HZ     0x04
#define BBK9288S_CTM_EV_1HZ     0x08
#define BBK9288S_CTM_EV_1MIN    0x10
#define BBK9288S_CTM_EV_1HOUR   0x20
#define BBK9288S_CTM_EV_1DAY    0x40
#define BBK9288S_16TM_CLOCK_CTRL_BASE 0x0147
#define BBK9288S_16TM_CHANNELS  6
#define BBK9288S_16TM_STRIDE    0x0008
#define BBK9288S_16TM_BASE      0x8180
#define BBK9288S_16TM_CLOCK_CTRL(n) \
    (BBK9288S_16TM_CLOCK_CTRL_BASE + (n))
#define BBK9288S_16TM_COMPARE_A(n) \
    (BBK9288S_16TM_BASE + (n) * BBK9288S_16TM_STRIDE)
#define BBK9288S_16TM_COMPARE_B(n) (BBK9288S_16TM_COMPARE_A(n) + 2)
#define BBK9288S_16TM_COUNTER(n)   (BBK9288S_16TM_COMPARE_A(n) + 4)
#define BBK9288S_16TM_CTRL(n)      (BBK9288S_16TM_COMPARE_A(n) + 6)
#define BBK9288S_16TM0_CLOCK_CTRL BBK9288S_16TM_CLOCK_CTRL(0)
#define BBK9288S_16TM_CTRL_PRUN    0x01
#define BBK9288S_16TM_CTRL_PRESET  0x02
#define BBK9288S_16TM_CTRL_PTM     0x04
#define BBK9288S_16TM_CTRL_CKSL    0x08
#define BBK9288S_16TM_CTRL_READ_MASK 0x7d
#define BBK9288S_16TM_TICK_MS      1
#define BBK9288S_PRESCALER_CLOCK_SELECT 0x0181
#define BBK9288S_PRESCALER_OSC1    0x01
#define BBK9288S_OSC1_HZ           32768
#define BBK9288S_CLG_POWER_CTRL 0x0180
#define BBK9288S_CLG_PRESCALER  0x0181
#define BBK9288S_CLG_CLOCK_OPT  0x0190
#define BBK9288S_CLG_PROTECT    0x019e
#define BBK9288S_CLOCK_OPT_8T1ON_OFF 0x04
#define BBK9288S_PIR_CTM        0x026b
#define BBK9288S_CTM_IRQ_BIT    0x02
#define BBK9288S_CTM_VECTOR     65
#define BBK9288S_PORT4_IRQ_BIT  0x04
#define BBK9288S_PORT4_VECTOR   68
#define BBK9288S_HSDMA_TRIG01   0x0298
#define BBK9288S_HSDMA_TRIG23   0x0299
#define BBK9288S_HSDMA_SW_TRIG  0x029a
#define BBK9288S_HSDMA_BASE     0x8220
#define BBK9288S_HSDMA_CH_SIZE  0x10
#define BBK9288S_HSDMA_CHANNELS 4
#define BBK9288S_PORT0_FUNC     0x02d0
#define BBK9288S_PORT0_DATA     0x02d1
#define BBK9288S_PORT0_IOCTRL   0x02d2
#define BBK9288S_PORT1_FUNC     0x02d4
#define BBK9288S_PORT1_DATA     0x02d5
#define BBK9288S_PORT1_IOCTRL   0x02d6
#define BBK9288S_PORT2_FUNC     0x02d8
#define BBK9288S_PORT2_DATA     0x02d9
#define BBK9288S_PORT2_IOCTRL   0x02da
#define BBK9288S_PORT3_FUNC     0x02dc
#define BBK9288S_PORT3_DATA     0x02dd
#define BBK9288S_PORT3_IOCTRL   0x02de
#define BBK9288S_KEY_GPIO_PORT  0
#define BBK9288S_KEY_P0_MASK    0xfb
#define BBK9288S_KEY_STROBE_BIT 0x08
#define BBK9288S_KEY_LINES       6
#define BBK9288S_K5_FUNC_SELECT 0x02c0
#define BBK9288S_K5_DATA        0x02c1
#define BBK9288S_K6_DATA        0x02c4
#define BBK9288S_PORT_INT_SELECT1 0x02c6
#define BBK9288S_PORT_INT_SELECT2 0x02c7
#define BBK9288S_PORT_INT_POLARITY 0x02c8
#define BBK9288S_PORT_INT_EDGE_LEVEL 0x02c9
#define BBK9288S_KEY_INPUT_SELECT 0x02ca
#define BBK9288S_KEY_INPUT_CONDITION0 0x02cc
#define BBK9288S_KEY_INPUT_CONDITION1 0x02cd
#define BBK9288S_KEY_INPUT_MASK0 0x02ce
#define BBK9288S_KEY_INPUT_MASK1 0x02cf
#define BBK9288S_K6_FUNC_SELECT 0x02c3
#define BBK9288S_WAKE_K6_BIT    0x10
#define BBK9288S_DEFAULT_TOUCH_FPT 6
#define BBK9288S_LCDC_HNDP      0x0040
#define BBK9288S_LCDC_HSIZE     0x0042
#define BBK9288S_LCDC_VNDP      0x004a
#define BBK9288S_LCDC_VSIZE     0x004c
#define BBK9288S_LCDC_CTRL      0x0202
#define BBK9288S_LCDC_SADDR     0x0210
#define BBK9288S_16TM0_COMPARE_A BBK9288S_16TM_COMPARE_A(0)
#define BBK9288S_16TM0_COMPARE_B BBK9288S_16TM_COMPARE_B(0)
#define BBK9288S_16TM0_COUNTER   BBK9288S_16TM_COUNTER(0)
#define BBK9288S_16TM0_CTRL      BBK9288S_16TM_CTRL(0)

static const uint8_t bbk9288s_board_latch_data[] = {
    0x00, 0xff, 0x55, 0xaa,
};

static const uint8_t bbk9288s_iram_rtc_cookie[] = {
    0x01, 0x02, 0x03, 0x04,
};

struct BBK9288SMachineState {
    MachineState parent_obj;

    uint32_t debug_usb_wakeup_ms;
    uint32_t debug_nmi_wakeup_ms;
    uint32_t debug_port4_wakeup_ms;
    uint32_t debug_port5_wakeup_ms;
    uint32_t debug_key_p0_low_mask;
    uint32_t debug_lcd_dump_ms;
    uint32_t touch_fpt;
    uint32_t touch_k5_low_mask;
    char *debug_lcd_dump_path;
    char *nand_image_path;
    bool touch_p0_low;
    bool debug_timer16_factors;
    bool trace_io;
    bool trace_key_scan;
    bool strict_board_io;
};

typedef enum BBK9288SRegFlags {
    BBK9288S_REG_NONE = 0,
    BBK9288S_REG_RESET_ONE_TO_CLEAR = 1 << 0,
} BBK9288SRegFlags;

typedef enum BBK9288STouchSerialPhase {
    BBK9288S_TOUCH_COMMAND,
    BBK9288S_TOUCH_DUMMY,
    BBK9288S_TOUCH_DATA,
    BBK9288S_TOUCH_TRAILER,
} BBK9288STouchSerialPhase;

typedef enum BBK9288SNANDPhase {
    BBK9288S_NAND_IDLE,
    BBK9288S_NAND_READ_ID,
    BBK9288S_NAND_READ_STATUS,
    BBK9288S_NAND_READ_SETUP,
    BBK9288S_NAND_READ_DATA,
    BBK9288S_NAND_ERASE_SETUP,
    BBK9288S_NAND_PROGRAM_DATA,
    BBK9288S_NAND_COPYBACK_READY,
} BBK9288SNANDPhase;

typedef struct BBK9288SRegInfo {
    hwaddr offset;
    const char *name;
    const char *group;
    uint8_t mask;
    uint8_t reset;
    BBK9288SRegFlags flags;
} BBK9288SRegInfo;

typedef struct BBK9288SITCPair {
    hwaddr enable_offset;
    hwaddr flag_offset;
    uint8_t mask;
    const char *name;
} BBK9288SITCPair;

typedef struct BBK9288SMMIOTrace {
    uint64_t seq;
    uint32_t pc;
    hwaddr addr;
    uint64_t value;
    unsigned size;
    bool is_write;
} BBK9288SMMIOTrace;

typedef struct BBK9288SIVRAMStats {
    uint32_t bytes;
    uint32_t nonzero;
    uint32_t checksum;
    uint32_t first_offset;
    uint32_t last_offset;
} BBK9288SIVRAMStats;

typedef struct BBK9288SState {
    MemoryRegion io;
    MemoryRegion board_io;
    MemoryRegion touch_io;
    MemoryRegion nand_io;
    MemoryRegion lcdc_io;
    MemoryRegion ivram;
    CPUState *cpu;
    QemuConsole *con;
    uint8_t *ivram_ptr;
    uint8_t io_regs[BBK9288S_IO_SIZE];
    uint8_t io_seen[BBK9288S_IO_SIZE];
    uint8_t board_regs[BBK9288S_BOARD_IO_SIZE];
    uint8_t board_seen[BBK9288S_BOARD_IO_SIZE];
    uint8_t touch_io_regs[BBK9288S_TOUCH_IO_SIZE];
    uint8_t board_data_latch_reads;
    bool board_data_latch_pending;
    uint8_t lcdc_regs[BBK9288S_LCDC_IO_SIZE];
    uint8_t lcdc_seen[BBK9288S_LCDC_IO_SIZE];
    BBK9288SMMIOTrace mmio_trace[BBK9288S_MMIO_TRACE_LEN];
    unsigned mmio_trace_pos;
    uint64_t mmio_trace_seq;
    uint32_t itc_pending_mask;
    uint8_t itc_irq_vector;
    uint8_t itc_irq_level;
    uint8_t gpio_input_low[4];
    uint8_t port_int_input_low[4];
    uint8_t k5_input_low;
    uint8_t k6_input_low;
    uint8_t key_input_high[2];
    bool key_input_high_valid[2];
    uint8_t port_input_high;
    bool port_input_high_valid;
    bool keyboard_qcode_down[Q_KEY_CODE__MAX];
    uint16_t keyboard_line_down_count[BBK9288S_KEY_LINES];
    uint8_t debug_key_p0_low_mask;
    uint16_t adc_touch_x;
    uint16_t adc_touch_y;
    uint16_t touch_pixel_x;
    uint16_t touch_pixel_y;
    uint16_t touch_serial_data;
    uint8_t touch_serial_command;
    uint8_t touch_serial_bits;
    uint8_t touch_serial_trailer_clocks;
    uint8_t touch_serial_logged_channels;
    BBK9288STouchSerialPhase touch_serial_phase;
    BBK9288SNANDPhase nand_phase;
    uint8_t nand_command;
    uint8_t nand_address[5];
    uint8_t nand_address_count;
    uint8_t nand_id_index;
    uint32_t nand_page;
    uint16_t nand_column;
    uint8_t nand_page_cache[BBK9288S_NAND_RAW_PAGE_SIZE];
    uint8_t nand_program_address_cycles;
    bool nand_page_cache_valid;
    bool nand_program_page_valid;
    bool nand_copyback_program;
    uint32_t nand_data_reads;
    uint32_t nand_data_writes;
    uint8_t *nand_storage;
    size_t nand_storage_size;
    char *nand_image_path;
    bool nand_dirty;
    uint64_t nand_command_count;
    uint64_t nand_page_read_count;
    uint64_t nand_program_count;
    uint64_t nand_erase_count;
    uint64_t hsdma_transfer_count[BBK9288S_HSDMA_CHANNELS];
    uint8_t adc_last_channel;
    uint8_t touch_fpt;
    uint8_t touch_k5_low_mask;
    bool touch_p0_low;
    bool touch_down;
    bool debug_timer16_factors;
    bool trace_io;
    bool trace_key_scan;
    bool strict_board_io;
    QemuInputHandlerState *kbd_handler;
    QemuInputHandlerState *ptr_handler;
    QEMUTimer *debug_usb_wakeup_timer;
    QEMUTimer *debug_nmi_wakeup_timer;
    QEMUTimer *debug_port4_wakeup_timer;
    QEMUTimer *debug_port5_wakeup_timer;
    QEMUTimer *debug_lcd_dump_timer;
    char *debug_lcd_dump_path;
    QEMUTimer *timer3b_wakeup_timer;
    QEMUTimer *timer16_tick_timer;
    QEMUTimer *timer2b_compare_timer;
    QEMUTimer *timer8_underflow_timer;
    QEMUTimer *clock_timer;
    uint8_t ctm_subsecond;
    Notifier exit;
} BBK9288SState;

static BBK9288SState *bbk9288s_active_machine;

static uint8_t bbk9288s_gpio_input_low_mask(BBK9288SState *s,
                                            unsigned port_index);
static void bbk9288s_key_input_update(BBK9288SState *s, const char *reason,
                                      bool allow_edges);
static void bbk9288s_clock_timer_update(BBK9288SState *s);
static void bbk9288s_clock_timer_cb(void *opaque);
static void bbk9288s_timer16_update(BBK9288SState *s);
static void bbk9288s_timer16_tick_cb(void *opaque);
static void bbk9288s_timer2b_update(BBK9288SState *s);
static void bbk9288s_timer2b_compare_cb(void *opaque);
static void bbk9288s_timer3b_update(BBK9288SState *s);
static void bbk9288s_timer8_underflow_cb(void *opaque);
static void bbk9288s_touch_serial_reset(BBK9288SState *s);
static void bbk9288s_nand_reset(BBK9288SState *s);
static void bbk9288s_nand_storage_save(BBK9288SState *s);

#define BBK9288S_16TM_REG(addr, name, mask) \
    { addr, name, "timer-16bit", mask, 0x00, BBK9288S_REG_NONE }

static const BBK9288SRegInfo bbk9288s_regs[] = {
    { BBK9288S_ADC_DATA_LO, "ADD_LO", "adc-touch", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_ADC_DATA_HI, "ADD_HI", "adc-touch", 0x03, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_ADC_TRIGGER, "ADTRG", "adc-touch", 0x3f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_ADC_CHANNEL, "ADCH", "adc-touch", 0x3f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_ADC_ENABLE, "ADCTL", "adc-touch", 0x7f, 0x10,
      BBK9288S_REG_NONE },
    { BBK9288S_ADC_SAMPLING, "ADSMP", "adc-touch", 0xf3, 0x03,
      BBK9288S_REG_NONE },
    { BBK9288S_ADC_FLAGS, "ADF_FLAGS", "adc-touch", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_ADC_OVERWRITE, "OWE_FLAGS", "adc-touch", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_ADC_UPPER_LO, "ADUPPER_LO", "adc-touch", 0xff, 0xff,
      BBK9288S_REG_NONE },
    { BBK9288S_ADC_UPPER_HI, "ADUPPER_HI", "adc-touch", 0x03, 0x03,
      BBK9288S_REG_NONE },
    { BBK9288S_ADC_LOWER_LO, "ADLOWER_LO", "adc-touch", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_ADC_LOWER_HI, "ADLOWER_HI", "adc-touch", 0x03, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_ADC_INTMASK, "ADINTMASK", "adc-touch", 0xff, 0xff,
      BBK9288S_REG_NONE },
    { BBK9288S_ADC_MODE, "ADCADV", "adc-touch", 0x01, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_8TM_CTRL_BASE + 0 * BBK9288S_8TM_STRIDE, "T8_CTRL0",
      "timer-8bit", 0x05, 0x00, BBK9288S_REG_NONE },
    { BBK9288S_8TM_CTRL_BASE + 0 * BBK9288S_8TM_STRIDE + 1, "T8_RELOAD0",
      "timer-8bit", 0xff, 0x00, BBK9288S_REG_NONE },
    { BBK9288S_8TM_CTRL_BASE + 0 * BBK9288S_8TM_STRIDE + 2, "T8_COUNT0",
      "timer-8bit", 0xff, 0x00, BBK9288S_REG_NONE },
    { BBK9288S_8TM_CTRL_BASE + 1 * BBK9288S_8TM_STRIDE, "T8_CTRL1",
      "timer-8bit", 0x05, 0x00, BBK9288S_REG_NONE },
    { BBK9288S_8TM_CTRL_BASE + 1 * BBK9288S_8TM_STRIDE + 1, "T8_RELOAD1",
      "timer-8bit", 0xff, 0x00, BBK9288S_REG_NONE },
    { BBK9288S_8TM_CTRL_BASE + 1 * BBK9288S_8TM_STRIDE + 2, "T8_COUNT1",
      "timer-8bit", 0xff, 0x00, BBK9288S_REG_NONE },
    { BBK9288S_8TM_CTRL_BASE + 2 * BBK9288S_8TM_STRIDE, "T8_CTRL2",
      "timer-8bit", 0x05, 0x00, BBK9288S_REG_NONE },
    { BBK9288S_8TM_CTRL_BASE + 2 * BBK9288S_8TM_STRIDE + 1, "T8_RELOAD2",
      "timer-8bit", 0xff, 0x00, BBK9288S_REG_NONE },
    { BBK9288S_8TM_CTRL_BASE + 2 * BBK9288S_8TM_STRIDE + 2, "T8_COUNT2",
      "timer-8bit", 0xff, 0x00, BBK9288S_REG_NONE },
    { BBK9288S_8TM_CTRL_BASE + 3 * BBK9288S_8TM_STRIDE, "T8_CTRL3",
      "timer-8bit", 0x05, 0x00, BBK9288S_REG_NONE },
    { BBK9288S_8TM_CTRL_BASE + 3 * BBK9288S_8TM_STRIDE + 1, "T8_RELOAD3",
      "timer-8bit", 0xff, 0x00, BBK9288S_REG_NONE },
    { BBK9288S_8TM_CTRL_BASE + 3 * BBK9288S_8TM_STRIDE + 2, "T8_COUNT3",
      "timer-8bit", 0xff, 0x00, BBK9288S_REG_NONE },
    { BBK9288S_16TM_CLOCK_CTRL(0), "T16_CLK0", "timer-16bit", 0x0f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_16TM_CLOCK_CTRL(1), "T16_CLK1", "timer-16bit", 0x0f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_16TM_CLOCK_CTRL(2), "T16_CLK2", "timer-16bit", 0x0f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_16TM_CLOCK_CTRL(3), "T16_CLK3", "timer-16bit", 0x0f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_16TM_CLOCK_CTRL(4), "T16_CLK4", "timer-16bit", 0x0f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_16TM_CLOCK_CTRL(5), "T16_CLK5", "timer-16bit", 0x0f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_CTM_RUN_CTRL, "TCRUN", "clock-timer", 0x03, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_CTM_INT_CTRL, "TCINT", "clock-timer", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_CTM_DIVIDER, "TCDIV", "clock-timer", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_CTM_SEC, "TCSEC", "clock-timer", 0x3f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_CTM_MIN, "TCMIN", "clock-timer", 0x3f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_CTM_HOUR, "TCHOUR", "clock-timer", 0x1f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_CTM_DAY_LO, "TCDAY_LO", "clock-timer", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_CTM_DAY_HI, "TCDAY_HI", "clock-timer", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_CTM_MIN_CMP, "TCALM_MIN", "clock-timer", 0x3f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_CTM_HOUR_CMP, "TCALM_HOUR", "clock-timer", 0x1f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_CTM_DAY_CMP, "TCALM_DAY", "clock-timer", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_CLG_POWER_CTRL, "CLG_POWER", "clock-generator", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_CLG_PRESCALER, "CLG_PRESCALER", "clock-generator", 0x01, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_CLG_CLOCK_OPT, "CLG_CLOCK_OPT", "clock-generator", 0x0d, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_CLG_PROTECT, "CLG_PROTECT", "clock-generator", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PIR_PORT0_1, "PPORT0_1", "itc-priority", 0x77, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PIR_PORT2_3, "PPORT2_3", "itc-priority", 0x77, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PIR_KEY_PORT0_3, "PKEY_PORT0_3", "itc-priority", 0x77, 0x00,
      BBK9288S_REG_NONE },
    { 0x263, "PHSDMA01", "itc-priority", 0x77, 0x00, BBK9288S_REG_NONE },
    { 0x264, "PHSDMA23", "itc-priority", 0x77, 0x00, BBK9288S_REG_NONE },
    { BBK9288S_PIR2_16TM0_1, "P16TM0_1", "itc-priority", 0x77, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PIR3_16TM2_3, "P16TM2_3", "itc-priority", 0x77, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PIR4_16TM4_5, "P16TM4_5", "itc-priority", 0x77, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PIR_8TM_SIF0, "P8TM_SIF0", "itc-priority", 0x77, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PIR_SIF1_ADC, "PSIF1_ADC", "itc-priority", 0x77, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PIR_CTM, "PCTM", "itc-priority", 0x07, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PIR_PORT4_5, "PPORT4_5", "itc-priority", 0x77, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PIR_PORT6_7, "PPORT6_7", "itc-priority", 0x77, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_EIR0_KEY_PORT0_3, "EIR0_KEY_PORT0_3", "itc-enable", 0x3f, 0x00,
      BBK9288S_REG_NONE },
    { 0x271, "EIR1_DMA", "itc-enable", 0x1f, 0x00, BBK9288S_REG_NONE },
    { BBK9288S_EIR2_16TM0_1, "EIR2_16TM0_1", "itc-enable", 0xcc, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_EIR3_16TM2_3, "EIR3_16TM2_3", "itc-enable", 0xcc, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_EIR4_16TM4_5, "EIR4_16TM4_5", "itc-enable", 0xcc, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_EIR5_8TM0_3, "EIR5_8TM0_3", "itc-enable", 0x0f, 0x00,
      BBK9288S_REG_NONE },
    { 0x276, "EIR6_SIF0_1", "itc-enable", 0x3f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_EIR7_PORT4_7_CTM_ADC, "EIR7_PORT4_7_CTM_ADC",
      "itc-enable", 0x3f, 0x00, BBK9288S_REG_NONE },
    { 0x278, "EIR8_8TM4_5", "itc-enable", 0x03, 0x00,
      BBK9288S_REG_NONE },
    { 0x279, "EIR9_SIF2_3", "itc-enable", 0x3f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_FIR0_KEY_PORT0_3, "FIR0_KEY_PORT0_3", "itc-flag", 0x3f, 0x00,
      BBK9288S_REG_RESET_ONE_TO_CLEAR },
    { 0x281, "FIR1_DMA", "itc-flag", 0x1f, 0x00,
      BBK9288S_REG_RESET_ONE_TO_CLEAR },
    { BBK9288S_FIR2_16TM0_1, "FIR2_16TM0_1", "itc-flag", 0xcc, 0x00,
      BBK9288S_REG_RESET_ONE_TO_CLEAR },
    { BBK9288S_FIR3_16TM2_3, "FIR3_16TM2_3", "itc-flag", 0xcc, 0x00,
      BBK9288S_REG_RESET_ONE_TO_CLEAR },
    { BBK9288S_FIR4_16TM4_5, "FIR4_16TM4_5", "itc-flag", 0xcc, 0x00,
      BBK9288S_REG_RESET_ONE_TO_CLEAR },
    { BBK9288S_FIR5_8TM0_3, "FIR5_8TM0_3", "itc-flag", 0x0f, 0x00,
      BBK9288S_REG_RESET_ONE_TO_CLEAR },
    { 0x286, "FIR6_SIF0_1", "itc-flag", 0x3f, 0x00,
      BBK9288S_REG_RESET_ONE_TO_CLEAR },
    { BBK9288S_FIR7_PORT4_7_CTM_ADC, "FIR7_PORT4_7_CTM_ADC",
      "itc-flag", 0x3f, 0x00, BBK9288S_REG_RESET_ONE_TO_CLEAR },
    { 0x288, "FIR8_8TM4_5", "itc-flag", 0x03, 0x00,
      BBK9288S_REG_RESET_ONE_TO_CLEAR },
    { 0x289, "FIR9_SIF2_3", "itc-flag", 0x3f, 0x00,
      BBK9288S_REG_RESET_ONE_TO_CLEAR },
    { 0x2a3, "PIR_USB_SPI", "itc-priority", 0x77, 0x00,
      BBK9288S_REG_NONE },
    { 0x2a6, "EIR_LCDC_USB_SPI", "itc-enable", 0xe0, 0x00,
      BBK9288S_REG_NONE },
    { 0x2a9, "FIR_LCDC_USB_SPI", "itc-flag", 0xe0, 0x00,
      BBK9288S_REG_RESET_ONE_TO_CLEAR },
    { BBK9288S_K5_FUNC_SELECT, "CFK5", "key-input-port", 0x1f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_K5_DATA, "K5D", "key-input-port", 0x1f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_K6_FUNC_SELECT, "CFK6", "adc-touch", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_K6_DATA, "K6D", "key-input-port", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PORT_INT_SELECT1, "SPT0_3", "port-input-int", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PORT_INT_SELECT2, "SPT4_7", "port-input-int", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PORT_INT_POLARITY, "SPPT0_7", "port-input-int", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PORT_INT_EDGE_LEVEL, "SEPT0_7", "port-input-int", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_KEY_INPUT_SELECT, "SPPK", "key-input-int", 0x0f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_KEY_INPUT_CONDITION0, "SCPK0", "key-input-int", 0x1f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_KEY_INPUT_CONDITION1, "SCPK1", "key-input-int", 0x0f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_KEY_INPUT_MASK0, "SMPK0", "key-input-int", 0x1f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_KEY_INPUT_MASK1, "SMPK1", "key-input-int", 0x0f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PORT0_FUNC, "CFP0", "gpio-port", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PORT0_DATA, "P0D", "gpio-port", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PORT0_IOCTRL, "IOC0", "gpio-port", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PORT1_FUNC, "CFP1", "gpio-port", 0x7f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PORT1_DATA, "P1D", "gpio-port", 0x7f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PORT1_IOCTRL, "IOC1", "gpio-port", 0x7f, 0x00,
      BBK9288S_REG_NONE },
    { 0x2d7, "CFSIO", "gpio-port", 0x0f, 0x00, BBK9288S_REG_NONE },
    { BBK9288S_PORT2_FUNC, "CFP2", "gpio-port", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PORT2_DATA, "P2D", "gpio-port", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PORT2_IOCTRL, "IOC2", "gpio-port", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { 0x2db, "CFSIO2", "gpio-port", 0x0f, 0x00, BBK9288S_REG_NONE },
    { BBK9288S_PORT3_FUNC, "CFP3", "gpio-port", 0x3f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PORT3_DATA, "P3D", "gpio-port", 0x3f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_PORT3_IOCTRL, "IOC3", "gpio-port", 0x3f, 0x00,
      BBK9288S_REG_NONE },
    { 0x2df, "CFEX", "port-function", 0xff, 0x03, BBK9288S_REG_NONE },
    { BBK9288S_HSDMA_TRIG01, "HSDMA_TRIG01", "hsdma-trigger", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_HSDMA_TRIG23, "HSDMA_TRIG23", "hsdma-trigger", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_HSDMA_SW_TRIG, "HSDMA_SW_TRIG", "hsdma-trigger", 0x0f, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_TBRP_OFFSET, "TBRP", "trap-table", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_TTBR_OFFSET + 0, "TTBR0", "trap-table", 0x00, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_TTBR_OFFSET + 1, "TTBR1", "trap-table", 0xfc, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_TTBR_OFFSET + 2, "TTBR2", "trap-table", 0xff, 0x00,
      BBK9288S_REG_NONE },
    { BBK9288S_TTBR_OFFSET + 3, "TTBR3", "trap-table", 0x0f, 0x02,
      BBK9288S_REG_NONE },
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_A(0) + 0,
                      "T16_CMPA0_LO", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_A(0) + 1,
                      "T16_CMPA0_HI", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_B(0) + 0,
                      "T16_CMPB0_LO", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_B(0) + 1,
                      "T16_CMPB0_HI", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COUNTER(0) + 0,
                      "T16_CNT0_LO", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COUNTER(0) + 1,
                      "T16_CNT0_HI", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_CTRL(0), "T16_CTRL0", 0x7f),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_A(1) + 0,
                      "T16_CMPA1_LO", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_A(1) + 1,
                      "T16_CMPA1_HI", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_B(1) + 0,
                      "T16_CMPB1_LO", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_B(1) + 1,
                      "T16_CMPB1_HI", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COUNTER(1) + 0,
                      "T16_CNT1_LO", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COUNTER(1) + 1,
                      "T16_CNT1_HI", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_CTRL(1), "T16_CTRL1", 0x7f),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_A(2) + 0,
                      "T16_CMPA2_LO", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_A(2) + 1,
                      "T16_CMPA2_HI", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_B(2) + 0,
                      "T16_CMPB2_LO", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_B(2) + 1,
                      "T16_CMPB2_HI", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COUNTER(2) + 0,
                      "T16_CNT2_LO", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COUNTER(2) + 1,
                      "T16_CNT2_HI", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_CTRL(2), "T16_CTRL2", 0x7f),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_A(3) + 0,
                      "T16_CMPA3_LO", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_A(3) + 1,
                      "T16_CMPA3_HI", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_B(3) + 0,
                      "T16_CMPB3_LO", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_B(3) + 1,
                      "T16_CMPB3_HI", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COUNTER(3) + 0,
                      "T16_CNT3_LO", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COUNTER(3) + 1,
                      "T16_CNT3_HI", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_CTRL(3), "T16_CTRL3", 0x7f),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_A(4) + 0,
                      "T16_CMPA4_LO", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_A(4) + 1,
                      "T16_CMPA4_HI", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_B(4) + 0,
                      "T16_CMPB4_LO", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_B(4) + 1,
                      "T16_CMPB4_HI", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COUNTER(4) + 0,
                      "T16_CNT4_LO", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COUNTER(4) + 1,
                      "T16_CNT4_HI", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_CTRL(4), "T16_CTRL4", 0x7f),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_A(5) + 0,
                      "T16_CMPA5_LO", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_A(5) + 1,
                      "T16_CMPA5_HI", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_B(5) + 0,
                      "T16_CMPB5_LO", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COMPARE_B(5) + 1,
                      "T16_CMPB5_HI", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COUNTER(5) + 0,
                      "T16_CNT5_LO", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_COUNTER(5) + 1,
                      "T16_CNT5_HI", 0xff),
    BBK9288S_16TM_REG(BBK9288S_16TM_CTRL(5), "T16_CTRL5", 0x7f),
};

static const BBK9288SITCPair bbk9288s_itc_pairs[] = {
    { BBK9288S_EIR0_KEY_PORT0_3, BBK9288S_FIR0_KEY_PORT0_3, 0x3f,
      "key-port0-3" },
    { 0x271, 0x281, 0x1f, "dma" },
    { BBK9288S_EIR2_16TM0_1, BBK9288S_FIR2_16TM0_1, 0xcc, "16tm0-1" },
    { BBK9288S_EIR3_16TM2_3, BBK9288S_FIR3_16TM2_3, 0xcc, "16tm2-3" },
    { BBK9288S_EIR4_16TM4_5, BBK9288S_FIR4_16TM4_5, 0xcc, "16tm4-5" },
    { 0x275, 0x285, 0x0f, "8tm0-3" },
    { 0x276, 0x286, 0x3f, "sif0-1" },
    { 0x277, 0x287, 0x3f, "port4-7-ctm-adc" },
    { 0x278, 0x288, 0x03, "8tm4-5" },
    { 0x279, 0x289, 0x3f, "sif2-3" },
    { 0x2a6, 0x2a9, 0xe0, "lcdc-usb-spi" },
};

static const BBK9288SRegInfo *bbk9288s_find_reg(hwaddr offset)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(bbk9288s_regs); i++) {
        if (bbk9288s_regs[i].offset == offset) {
            return &bbk9288s_regs[i];
        }
    }

    return NULL;
}

static bool bbk9288s_is_hsdma_reg(hwaddr offset)
{
    return offset >= BBK9288S_HSDMA_BASE &&
           offset < BBK9288S_HSDMA_BASE +
                    BBK9288S_HSDMA_CHANNELS * BBK9288S_HSDMA_CH_SIZE;
}

static bool bbk9288s_is_adc_reg(hwaddr offset)
{
    return offset >= BBK9288S_ADC_DATA_LO && offset <= BBK9288S_ADC_MODE;
}

static const char *bbk9288s_adc_reg_name(hwaddr offset)
{
    if (offset >= BBK9288S_ADC_BUFFER_BASE &&
        offset < BBK9288S_ADC_BUFFER_BASE + BBK9288S_ADC_CHANNELS * 2) {
        return (offset & 1) ? "ADBUF_HI" : "ADBUF_LO";
    }

    return "unknown";
}

static const char *bbk9288s_hsdma_reg_name(hwaddr offset)
{
    switch (offset & 0xf) {
    case 0x0:
    case 0x1:
        return "HSDMA_COUNT";
    case 0x2:
    case 0x3:
        return "HSDMA_CTRL";
    case 0x4:
    case 0x5:
        return "HSDMA_SRC_LO";
    case 0x6:
    case 0x7:
        return "HSDMA_SRC_HI";
    case 0x8:
    case 0x9:
        return "HSDMA_DST_LO";
    case 0xa:
    case 0xb:
        return "HSDMA_DST_HI";
    case 0xc:
        return "HSDMA_ENABLE";
    case 0xe:
        return "HSDMA_TRIGGER_FLAG";
    default:
        return "HSDMA_RESERVED";
    }
}

static const char *bbk9288s_io_group_name(hwaddr offset)
{
    const BBK9288SRegInfo *reg = bbk9288s_find_reg(offset);

    if (reg != NULL) {
        return reg->group;
    }

    if (bbk9288s_is_hsdma_reg(offset)) {
        return "hsdma";
    }
    if (bbk9288s_is_adc_reg(offset)) {
        return "adc-touch";
    }
    if (offset >= 0x260 && offset <= 0x2af) {
        return "itc";
    }
    if (offset >= 0x2c0 && offset <= 0x2ff) {
        return "gpio-key";
    }
    if (offset >= 0x8120 && offset <= 0x813f) {
        return "trap-table";
    }

    switch ((offset >> 8) & 0xff) {
    case 0x00:
        return "system";
    case 0x01:
        return "timer";
    case 0x03:
        return "adc-touch";
    case 0x04:
    case 0x05:
        return "lcdc";
    case 0x06:
    case 0x07:
        return "nand";
    default:
        return "unknown";
    }
}

static const char *bbk9288s_io_reg_name(hwaddr offset)
{
    const BBK9288SRegInfo *reg = bbk9288s_find_reg(offset);

    if (reg != NULL) {
        return reg->name;
    }
    if (bbk9288s_is_hsdma_reg(offset)) {
        return bbk9288s_hsdma_reg_name(offset);
    }
    if (bbk9288s_is_adc_reg(offset)) {
        return bbk9288s_adc_reg_name(offset);
    }
    return "unknown";
}

static const char *bbk9288s_lcdc_reg_name(hwaddr offset)
{
    switch (offset) {
    case BBK9288S_LCDC_HNDP:
        return "HNDP";
    case BBK9288S_LCDC_HSIZE:
        return "HSIZE";
    case BBK9288S_LCDC_VNDP:
        return "VNDP";
    case BBK9288S_LCDC_VSIZE:
        return "VSIZE";
    case BBK9288S_LCDC_CTRL:
        return "LCDC_CTRL";
    case BBK9288S_LCDC_SADDR:
        return "SADDR0";
    case BBK9288S_LCDC_SADDR + 1:
        return "SADDR1";
    default:
        if (offset >= 0x0400 && offset < 0x0800) {
            return "LUT";
        }
        return "unknown";
    }
}

static uint32_t bbk9288s_current_pc(void)
{
    CPUState *cs = current_cpu;

    if (cs == NULL) {
        return 0xffffffffu;
    }

    return S1C33_CPU(cs)->env.pc;
}

static bool bbk9288s_range_touches(hwaddr offset, unsigned size,
                                   hwaddr base, unsigned base_size)
{
    return offset < base + base_size && offset + size > base;
}

static uint32_t bbk9288s_ttbr_from_regs(BBK9288SState *s)
{
    return ((uint32_t)s->io_regs[BBK9288S_TTBR_OFFSET + 0]) |
           ((uint32_t)s->io_regs[BBK9288S_TTBR_OFFSET + 1] << 8) |
           ((uint32_t)s->io_regs[BBK9288S_TTBR_OFFSET + 2] << 16) |
           ((uint32_t)s->io_regs[BBK9288S_TTBR_OFFSET + 3] << 24);
}

static void bbk9288s_set_ttbr(BBK9288SState *s, uint32_t value)
{
    uint32_t ttbr = value & S1C33_TTBR_MASK;

    s->io_regs[BBK9288S_TTBR_OFFSET + 0] = ttbr & 0xff;
    s->io_regs[BBK9288S_TTBR_OFFSET + 1] = (ttbr >> 8) & 0xff;
    s->io_regs[BBK9288S_TTBR_OFFSET + 2] = (ttbr >> 16) & 0xff;
    s->io_regs[BBK9288S_TTBR_OFFSET + 3] = (ttbr >> 24) & 0xff;
    if (s->cpu != NULL) {
        S1C33_CPU(s->cpu)->env.ttbr = ttbr;
    }
}

static uint8_t bbk9288s_irq_level_or_one(uint8_t level)
{
    return level != 0 ? level : 1;
}

static bool bbk9288s_select_irq(BBK9288SState *s, uint8_t *vector,
                                uint8_t *level, const char **name)
{
    static const char * const port0_3_names[] = {
        "port0", "port1", "port2", "port3",
    };
    static const char * const port4_7_names[] = {
        "port4", "port5", "port6", "port7",
    };
    uint8_t key_port0_3_active =
        s->io_regs[BBK9288S_EIR0_KEY_PORT0_3] &
        s->io_regs[BBK9288S_FIR0_KEY_PORT0_3] & 0x3f;
    uint8_t timer16_0_1_active =
        s->io_regs[BBK9288S_EIR2_16TM0_1] &
        s->io_regs[BBK9288S_FIR2_16TM0_1] & 0xcc;
    uint8_t timer2b_active =
        s->io_regs[BBK9288S_EIR3_16TM2_3] &
        s->io_regs[BBK9288S_FIR3_16TM2_3] &
        BBK9288S_16TM2B_IRQ_BIT;
    uint8_t timer3b_active =
        s->io_regs[BBK9288S_EIR3_16TM2_3] &
        s->io_regs[BBK9288S_FIR3_16TM2_3] &
        BBK9288S_16TM3B_IRQ_BIT;
    uint8_t timer8_active =
        s->io_regs[BBK9288S_EIR5_8TM0_3] &
        s->io_regs[BBK9288S_FIR5_8TM0_3] &
        BBK9288S_8TM0_3_MASK;
    uint8_t adc_active =
        s->io_regs[BBK9288S_EIR7_PORT4_7_CTM_ADC] &
        s->io_regs[BBK9288S_FIR7_PORT4_7_CTM_ADC] &
        BBK9288S_ADC_IRQ_BIT;
    uint8_t ctm_active =
        s->io_regs[BBK9288S_EIR7_PORT4_7_CTM_ADC] &
        s->io_regs[BBK9288S_FIR7_PORT4_7_CTM_ADC] &
        BBK9288S_CTM_IRQ_BIT;
    uint8_t port4_active =
        s->io_regs[BBK9288S_EIR7_PORT4_7_CTM_ADC] &
        s->io_regs[BBK9288S_FIR7_PORT4_7_CTM_ADC] &
        0x3c;
    uint8_t active = s->io_regs[0x2a6] & s->io_regs[0x2a9] & 0xe0;
    unsigned port;
    unsigned timer8;

    for (port = 0; port <= 3; port++) {
        if ((key_port0_3_active & (1u << port)) == 0) {
            continue;
        }
        *vector = BBK9288S_PORT0_VECTOR + port;
        if (port == 0) {
            *level = bbk9288s_irq_level_or_one(
                s->io_regs[BBK9288S_PIR_PORT0_1] & 0x07);
        } else if (port == 1) {
            *level = bbk9288s_irq_level_or_one(
                (s->io_regs[BBK9288S_PIR_PORT0_1] >> 4) & 0x07);
        } else if (port == 2) {
            *level = bbk9288s_irq_level_or_one(
                s->io_regs[BBK9288S_PIR_PORT2_3] & 0x07);
        } else {
            *level = bbk9288s_irq_level_or_one(
                (s->io_regs[BBK9288S_PIR_PORT2_3] >> 4) & 0x07);
        }
        *name = port0_3_names[port];
        return true;
    }
    if ((key_port0_3_active & BBK9288S_KEY0_IRQ_BIT) != 0) {
        *vector = BBK9288S_KEY0_VECTOR;
        *level = bbk9288s_irq_level_or_one(
            s->io_regs[BBK9288S_PIR_KEY_PORT0_3] & 0x07);
        *name = "key-input0";
        return true;
    }
    if ((key_port0_3_active & BBK9288S_KEY1_IRQ_BIT) != 0) {
        *vector = BBK9288S_KEY1_VECTOR;
        *level = bbk9288s_irq_level_or_one(
            (s->io_regs[BBK9288S_PIR_KEY_PORT0_3] >> 4) & 0x07);
        *name = "key-input1";
        return true;
    }
    if ((timer16_0_1_active & BBK9288S_16TM0B_IRQ_BIT) != 0) {
        *vector = BBK9288S_16TM0B_VECTOR;
        *level = bbk9288s_irq_level_or_one(
            s->io_regs[BBK9288S_PIR2_16TM0_1] & 0x07);
        *name = "16tm0-b";
        return true;
    }
    if ((timer16_0_1_active & BBK9288S_16TM0A_IRQ_BIT) != 0) {
        *vector = BBK9288S_16TM0A_VECTOR;
        *level = bbk9288s_irq_level_or_one(
            s->io_regs[BBK9288S_PIR2_16TM0_1] & 0x07);
        *name = "16tm0-a";
        return true;
    }
    if (timer3b_active != 0 &&
        (timer2b_active == 0 ||
         ((s->io_regs[BBK9288S_PIR3_16TM2_3] >> 4) & 0x07) >
         (s->io_regs[BBK9288S_PIR3_16TM2_3] & 0x07))) {
        *vector = BBK9288S_16TM3B_VECTOR;
        *level = bbk9288s_irq_level_or_one(
            (s->io_regs[BBK9288S_PIR3_16TM2_3] >> 4) & 0x07);
        *name = "16tm3-b";
        return true;
    }
    if (timer2b_active != 0) {
        *vector = BBK9288S_16TM2B_VECTOR;
        *level = bbk9288s_irq_level_or_one(
            s->io_regs[BBK9288S_PIR3_16TM2_3] & 0x07);
        *name = "16tm2-b";
        return true;
    }
    if (timer3b_active != 0) {
        *vector = BBK9288S_16TM3B_VECTOR;
        *level = bbk9288s_irq_level_or_one(
            (s->io_regs[BBK9288S_PIR3_16TM2_3] >> 4) & 0x07);
        *name = "16tm3-b";
        return true;
    }
    for (timer8 = 0; timer8 < BBK9288S_8TM_CHANNELS; timer8++) {
        if ((timer8_active & (1u << timer8)) == 0) {
            continue;
        }
        *vector = BBK9288S_8TM0_3_VECTOR;
        *level = bbk9288s_irq_level_or_one(
            s->io_regs[BBK9288S_PIR_8TM_SIF0] & 0x07);
        *name = "8tm0-3";
        return true;
    }
    if (adc_active != 0) {
        *vector = BBK9288S_ADC_VECTOR;
        *level = bbk9288s_irq_level_or_one(
            (s->io_regs[BBK9288S_PIR_SIF1_ADC] >> 4) & 0x07);
        *name = "adc";
        return true;
    }
    if (ctm_active != 0) {
        *vector = BBK9288S_CTM_VECTOR;
        *level = bbk9288s_irq_level_or_one(
            s->io_regs[BBK9288S_PIR_CTM] & 0x07);
        *name = "clock-timer";
        return true;
    }
    for (port = 4; port <= 7; port++) {
        if ((port4_active & (1u << (port - 2))) == 0) {
            continue;
        }
        *vector = BBK9288S_PORT4_VECTOR + port - 4;
        if (port == 4) {
            *level = bbk9288s_irq_level_or_one(
                s->io_regs[BBK9288S_PIR_PORT4_5] & 0x07);
        } else if (port == 5) {
            *level = bbk9288s_irq_level_or_one(
                (s->io_regs[BBK9288S_PIR_PORT4_5] >> 4) & 0x07);
        } else if (port == 6) {
            *level = bbk9288s_irq_level_or_one(
                s->io_regs[BBK9288S_PIR_PORT6_7] & 0x07);
        } else {
            *level = bbk9288s_irq_level_or_one(
                (s->io_regs[BBK9288S_PIR_PORT6_7] >> 4) & 0x07);
        }
        *name = port4_7_names[port - 4];
        return true;
    }
    if (active & 0x40) {
        *vector = 113;
        *level = s->io_regs[0x2a3] & 0x07;
        *name = "usb";
        return true;
    }
    if (active & 0x80) {
        *vector = 114;
        *level = (s->io_regs[0x2a3] >> 4) & 0x07;
        *name = "spi";
        return true;
    }
    if (active & 0x20) {
        *vector = 112;
        *level = 1;
        *name = "lcdc";
        return true;
    }

    return false;
}

static void bbk9288s_update_irq(BBK9288SState *s)
{
    uint32_t pending = 0;
    uint8_t vector = 0;
    uint8_t level = 0;
    const char *source = NULL;
    bool have_irq;
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(bbk9288s_itc_pairs); i++) {
        const BBK9288SITCPair *pair = &bbk9288s_itc_pairs[i];
        uint8_t active = s->io_regs[pair->enable_offset] &
                         s->io_regs[pair->flag_offset] & pair->mask;

        if (active != 0) {
            pending |= 1u << i;
        }
    }

    have_irq = bbk9288s_select_irq(s, &vector, &level, &source);
    if (pending == s->itc_pending_mask &&
        vector == s->itc_irq_vector &&
        level == s->itc_irq_level) {
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "bbk9288s-itc: pending 0x%08x -> 0x%08x\n",
                  s->itc_pending_mask, pending);
    for (i = 0; i < ARRAY_SIZE(bbk9288s_itc_pairs); i++) {
        uint32_t bit = 1u << i;

        if ((pending ^ s->itc_pending_mask) & bit) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bbk9288s-itc: %s %s\n",
                          bbk9288s_itc_pairs[i].name,
                          (pending & bit) ? "pending" : "clear");
        }
    }
    if (have_irq) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-itc: deliver source=%s vector=%u level=%u\n",
                      source, vector, level);
    } else if (pending != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-itc: pending source has no implemented "
                      "vector yet\n");
    }

    s->itc_pending_mask = pending;
    s->itc_irq_vector = have_irq ? vector : 0;
    s->itc_irq_level = have_irq ? level : 0;
    if (s->cpu != NULL) {
        s1c33_cpu_set_irq(s->cpu, have_irq, vector, level);
    }
}

static void bbk9288s_raise_usb_factor(BBK9288SState *s, const char *reason)
{
    s->io_regs[0x2a9] |= BBK9288S_USB_IRQ_BIT;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "bbk9288s-itc: USB factor set by %s "
                  "eir=0x%02x fir=0x%02x pir=0x%02x\n",
                  reason, s->io_regs[0x2a6], s->io_regs[0x2a9],
                  s->io_regs[0x2a3]);
    bbk9288s_update_irq(s);
}

static void bbk9288s_raise_adc_factor(BBK9288SState *s, const char *reason,
                                      unsigned channel)
{
    uint8_t priority = (s->io_regs[BBK9288S_PIR_SIF1_ADC] >> 4) & 0x07;

    s->io_regs[BBK9288S_FIR7_PORT4_7_CTM_ADC] |= BBK9288S_ADC_IRQ_BIT;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "bbk9288s-adc: factor set by %s ch=%u "
                  "eir=0x%02x fir=0x%02x pir=0x%02x level=%u\n",
                  reason, channel,
                  s->io_regs[BBK9288S_EIR7_PORT4_7_CTM_ADC],
                  s->io_regs[BBK9288S_FIR7_PORT4_7_CTM_ADC],
                  s->io_regs[BBK9288S_PIR_SIF1_ADC],
                  bbk9288s_irq_level_or_one(priority));
    bbk9288s_update_irq(s);
}

static void bbk9288s_clock_timer_reset_counters(BBK9288SState *s)
{
    memset(&s->io_regs[BBK9288S_CTM_COUNTER_BASE], 0,
           BBK9288S_CTM_COUNTER_SIZE);
    s->io_regs[BBK9288S_CTM_DIVIDER] = 0;
    s->ctm_subsecond = 0;
}

static bool bbk9288s_is_leap_year(unsigned year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static void bbk9288s_clock_timer_init_rtc(BBK9288SState *s)
{
    struct tm now;
    unsigned year;
    uint32_t days = 0;

    qemu_get_timedate(&now, 0);
    year = now.tm_year + 1900;
    if (year < 1901) {
        year = 1901;
        now.tm_yday = 0;
    } else if (year > 2049) {
        year = 2049;
        now.tm_yday = 364;
    }
    for (unsigned y = 1901; y < year; y++) {
        days += bbk9288s_is_leap_year(y) ? 366 : 365;
    }
    days += now.tm_yday;

    s->io_regs[BBK9288S_CTM_SEC] = now.tm_sec;
    s->io_regs[BBK9288S_CTM_MIN] = now.tm_min;
    s->io_regs[BBK9288S_CTM_HOUR] = now.tm_hour;
    s->io_regs[BBK9288S_CTM_DAY_LO] = days & 0xff;
    s->io_regs[BBK9288S_CTM_DAY_HI] = days >> 8;
    info_report("BBK9288S battery RTC initialized: "
                "%04u-%02u-%02u %02u:%02u:%02u day=%u",
                year, now.tm_mon + 1, now.tm_mday, now.tm_hour,
                now.tm_min, now.tm_sec, days);
}

static uint8_t bbk9288s_clock_timer_increment(BBK9288SState *s)
{
    uint8_t events = BBK9288S_CTM_EV_32HZ;
    uint16_t day;

    s->io_regs[BBK9288S_CTM_DIVIDER]++;
    s->ctm_subsecond++;
    if ((s->ctm_subsecond & 0x03) == 0) {
        events |= BBK9288S_CTM_EV_8HZ;
    }
    if ((s->ctm_subsecond & 0x0f) == 0) {
        events |= BBK9288S_CTM_EV_2HZ;
    }
    if (s->ctm_subsecond < BBK9288S_CTM_TICKS_PER_SECOND) {
        return events;
    }
    s->ctm_subsecond = 0;
    events |= BBK9288S_CTM_EV_1HZ;

    s->io_regs[BBK9288S_CTM_SEC]++;
    if (s->io_regs[BBK9288S_CTM_SEC] < 60) {
        return events;
    }
    s->io_regs[BBK9288S_CTM_SEC] = 0;
    events |= BBK9288S_CTM_EV_1MIN;

    s->io_regs[BBK9288S_CTM_MIN]++;
    if (s->io_regs[BBK9288S_CTM_MIN] < 60) {
        return events;
    }
    s->io_regs[BBK9288S_CTM_MIN] = 0;
    events |= BBK9288S_CTM_EV_1HOUR;

    s->io_regs[BBK9288S_CTM_HOUR]++;
    if (s->io_regs[BBK9288S_CTM_HOUR] < 24) {
        return events;
    }
    s->io_regs[BBK9288S_CTM_HOUR] = 0;
    events |= BBK9288S_CTM_EV_1DAY;

    day = ((uint16_t)s->io_regs[BBK9288S_CTM_DAY_HI] << 8) |
          s->io_regs[BBK9288S_CTM_DAY_LO];
    day++;
    s->io_regs[BBK9288S_CTM_DAY_LO] = day & 0xff;
    s->io_regs[BBK9288S_CTM_DAY_HI] = day >> 8;
    return events;
}

static uint8_t bbk9288s_clock_timer_periodic_event(BBK9288SState *s)
{
    switch ((s->io_regs[BBK9288S_CTM_INT_CTRL] >> 5) & 0x07) {
    case 0:
        return BBK9288S_CTM_EV_32HZ;
    case 1:
        return BBK9288S_CTM_EV_8HZ;
    case 2:
        return BBK9288S_CTM_EV_2HZ;
    case 3:
        return BBK9288S_CTM_EV_1HZ;
    case 4:
        return BBK9288S_CTM_EV_1MIN;
    case 5:
        return BBK9288S_CTM_EV_1HOUR;
    case 6:
        return BBK9288S_CTM_EV_1DAY;
    default:
        return 0;
    }
}

static bool bbk9288s_clock_timer_alarm_match(BBK9288SState *s)
{
    uint8_t tcase = (s->io_regs[BBK9288S_CTM_INT_CTRL] >> 2) & 0x07;
    uint16_t day = ((uint16_t)s->io_regs[BBK9288S_CTM_DAY_HI] << 8) |
                   s->io_regs[BBK9288S_CTM_DAY_LO];

    if (tcase == 0) {
        return false;
    }
    if ((tcase & 0x01) &&
        ((s->io_regs[BBK9288S_CTM_MIN_CMP] ^ s->io_regs[BBK9288S_CTM_MIN]) &
         0x3f) != 0) {
        return false;
    }
    if ((tcase & 0x02) &&
        ((s->io_regs[BBK9288S_CTM_HOUR_CMP] ^ s->io_regs[BBK9288S_CTM_HOUR]) &
         0x1f) != 0) {
        return false;
    }
    if ((tcase & 0x04) &&
        ((s->io_regs[BBK9288S_CTM_DAY_CMP] ^ day) & 0x1f) != 0) {
        return false;
    }
    return true;
}

static void bbk9288s_raise_clock_timer_factor(BBK9288SState *s,
                                              const char *reason,
                                              uint8_t tcint_flag)
{
    uint8_t priority = s->io_regs[BBK9288S_PIR_CTM] & 0x07;

    s->io_regs[BBK9288S_CTM_INT_CTRL] |= tcint_flag;
    s->io_regs[BBK9288S_FIR7_PORT4_7_CTM_ADC] |= BBK9288S_CTM_IRQ_BIT;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "bbk9288s-ctm: factor set by %s "
                  "tcint=0x%02x eir=0x%02x fir=0x%02x pir=0x%02x "
                  "level=%u sec=%u min=%u hour=%u day=0x%02x%02x\n",
                  reason, s->io_regs[BBK9288S_CTM_INT_CTRL],
                  s->io_regs[BBK9288S_EIR7_PORT4_7_CTM_ADC],
                  s->io_regs[BBK9288S_FIR7_PORT4_7_CTM_ADC],
                  s->io_regs[BBK9288S_PIR_CTM],
                  bbk9288s_irq_level_or_one(priority),
                  s->io_regs[BBK9288S_CTM_SEC],
                  s->io_regs[BBK9288S_CTM_MIN],
                  s->io_regs[BBK9288S_CTM_HOUR],
                  s->io_regs[BBK9288S_CTM_DAY_HI],
                  s->io_regs[BBK9288S_CTM_DAY_LO]);
    bbk9288s_update_irq(s);
}

static bool bbk9288s_clock_timer_armed(BBK9288SState *s)
{
    return (s->io_regs[BBK9288S_CTM_RUN_CTRL] & BBK9288S_CTM_TCRUN) != 0;
}

static void bbk9288s_clock_timer_update(BBK9288SState *s)
{
    if (!bbk9288s_clock_timer_armed(s)) {
        if (s->clock_timer != NULL) {
            timer_del(s->clock_timer);
        }
        return;
    }

    if (s->clock_timer == NULL) {
        s->clock_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME, bbk9288s_clock_timer_cb, s);
    }
    if (!timer_pending(s->clock_timer)) {
        timer_mod(s->clock_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  BBK9288S_CTM_TICK_MS);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-ctm: tick scheduled after %u ms "
                      "tcint=0x%02x eir=0x%02x fir=0x%02x\n",
                      BBK9288S_CTM_TICK_MS,
                      s->io_regs[BBK9288S_CTM_INT_CTRL],
                      s->io_regs[BBK9288S_EIR7_PORT4_7_CTM_ADC],
                      s->io_regs[BBK9288S_FIR7_PORT4_7_CTM_ADC]);
    }
}

static void bbk9288s_clock_timer_cb(void *opaque)
{
    BBK9288SState *s = opaque;
    uint8_t events;
    uint8_t periodic_event;

    if ((s->io_regs[BBK9288S_CTM_RUN_CTRL] & BBK9288S_CTM_TCRUN) == 0) {
        bbk9288s_clock_timer_update(s);
        return;
    }

    events = bbk9288s_clock_timer_increment(s);
    periodic_event = bbk9288s_clock_timer_periodic_event(s);
    if (periodic_event != 0 && (events & periodic_event) != 0 &&
        (s->io_regs[BBK9288S_CTM_INT_CTRL] & BBK9288S_CTM_TCIF) == 0 &&
        (s->io_regs[BBK9288S_FIR7_PORT4_7_CTM_ADC] &
         BBK9288S_CTM_IRQ_BIT) == 0) {
        bbk9288s_raise_clock_timer_factor(s, "periodic-tick",
                                          BBK9288S_CTM_TCIF);
    }
    if ((events & (BBK9288S_CTM_EV_1MIN | BBK9288S_CTM_EV_1HOUR |
                   BBK9288S_CTM_EV_1DAY)) != 0 &&
        bbk9288s_clock_timer_alarm_match(s) &&
        (s->io_regs[BBK9288S_CTM_INT_CTRL] & BBK9288S_CTM_TCAF) == 0 &&
        (s->io_regs[BBK9288S_FIR7_PORT4_7_CTM_ADC] &
         BBK9288S_CTM_IRQ_BIT) == 0) {
        bbk9288s_raise_clock_timer_factor(s, "alarm",
                                          BBK9288S_CTM_TCAF);
    }
    bbk9288s_clock_timer_update(s);
}

static void bbk9288s_raise_port4_factor(BBK9288SState *s, const char *reason)
{
    uint8_t priority = s->io_regs[BBK9288S_PIR_PORT4_5] & 0x07;

    s->io_regs[BBK9288S_FIR7_PORT4_7_CTM_ADC] |= BBK9288S_PORT4_IRQ_BIT;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "bbk9288s-itc: port4 factor set by %s "
                  "eir=0x%02x fir=0x%02x pir=0x%02x level=%u\n",
                  reason, s->io_regs[BBK9288S_EIR7_PORT4_7_CTM_ADC],
                  s->io_regs[BBK9288S_FIR7_PORT4_7_CTM_ADC],
                  s->io_regs[BBK9288S_PIR_PORT4_5],
                  bbk9288s_irq_level_or_one(priority));
    bbk9288s_update_irq(s);
}

static uint8_t bbk9288s_port_input_select(BBK9288SState *s, unsigned fpt)
{
    uint8_t reg = s->io_regs[fpt < 4 ? BBK9288S_PORT_INT_SELECT1 :
                                      BBK9288S_PORT_INT_SELECT2];

    return (reg >> ((fpt & 3) * 2)) & 0x03;
}

static bool bbk9288s_external_port_high(BBK9288SState *s, unsigned port,
                                        unsigned bit)
{
    uint8_t low = bbk9288s_gpio_input_low_mask(s, port) |
                  s->port_int_input_low[port];

    return (low & (1u << bit)) == 0;
}

static bool bbk9288s_k5_high(BBK9288SState *s, unsigned bit)
{
    return (s->k5_input_low & (1u << bit)) == 0;
}

static bool bbk9288s_k6_high(BBK9288SState *s, unsigned bit)
{
    return (s->k6_input_low & (1u << bit)) == 0;
}

static bool bbk9288s_port_input_signal_high(BBK9288SState *s, unsigned fpt)
{
    switch (bbk9288s_port_input_select(s, fpt)) {
    case 0:
        return bbk9288s_k6_high(s, fpt);
    case 1:
        if (fpt <= 4) {
            return bbk9288s_k5_high(s, fpt);
        }
        return bbk9288s_external_port_high(s, 3, fpt - 4);
    case 2:
        return bbk9288s_external_port_high(s, 0, fpt);
    case 3:
        return bbk9288s_external_port_high(s, 2, fpt);
    default:
        g_assert_not_reached();
    }
}

static void bbk9288s_port_input_update(BBK9288SState *s, const char *reason,
                                       bool allow_edges, bool force_level)
{
    uint8_t new_high = s->port_input_high;
    bool changed = false;
    unsigned fpt;

    for (fpt = 4; fpt <= 7; fpt++) {
        bool high = bbk9288s_port_input_signal_high(s, fpt);
        bool old_high = (s->port_input_high & (1u << fpt)) != 0;
        bool have_old = s->port_input_high_valid;
        bool edge = have_old && old_high != high;
        bool polarity_high =
            (s->io_regs[BBK9288S_PORT_INT_POLARITY] & (1u << fpt)) != 0;
        bool edge_mode =
            (s->io_regs[BBK9288S_PORT_INT_EDGE_LEVEL] & (1u << fpt)) != 0;
        bool selected = polarity_high ? high : !high;
        bool trigger = edge_mode ? (allow_edges && edge && selected) :
                                   (selected && (force_level || edge));

        if (high) {
            new_high |= 1u << fpt;
        } else {
            new_high &= ~(1u << fpt);
        }

        if (!trigger) {
            continue;
        }

        if ((s->io_regs[BBK9288S_FIR7_PORT4_7_CTM_ADC] &
             (1u << (fpt - 2))) == 0) {
            changed = true;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bbk9288s-port: FPT%u factor set by %s "
                          "spt=%u sept=%u sppt=%u input=%s\n",
                          fpt, reason, bbk9288s_port_input_select(s, fpt),
                          edge_mode ? 1 : 0, polarity_high ? 1 : 0,
                          high ? "high" : "low");
        }
        s->io_regs[BBK9288S_FIR7_PORT4_7_CTM_ADC] |= 1u << (fpt - 2);
    }

    s->port_input_high = new_high;
    s->port_input_high_valid = true;
    if (changed) {
        bbk9288s_update_irq(s);
    }
}

static bool bbk9288s_keyboard_any_down(BBK9288SState *s)
{
    unsigned i;

    for (i = 0; i < BBK9288S_KEY_LINES; i++) {
        if (s->keyboard_line_down_count[i] != 0) {
            return true;
        }
    }
    return false;
}

static void bbk9288s_update_port_wake_line(BBK9288SState *s,
                                           const char *reason)
{
    uint8_t old_k5 = s->k5_input_low;
    uint8_t old_k6 = s->k6_input_low;
    uint8_t old_p0_port_int = s->port_int_input_low[0];
    uint8_t touch_bit = 1u << s->touch_fpt;
    uint8_t k5_managed_bits = s->touch_k5_low_mask & 0x1f;
    uint8_t managed_bits = BBK9288S_WAKE_K6_BIT | touch_bit;
    uint8_t new_low = 0;
    bool keyboard_active = bbk9288s_keyboard_any_down(s);
    bool touch_active = s->touch_down;
    uint8_t new_k5_low = touch_active ? k5_managed_bits : 0;

    if (keyboard_active) {
        new_low |= BBK9288S_WAKE_K6_BIT;
    }
    if (touch_active) {
        new_low |= touch_bit;
    }

    s->k5_input_low = (s->k5_input_low & ~k5_managed_bits) | new_k5_low;
    s->k6_input_low = (s->k6_input_low & ~managed_bits) | new_low;
    s->port_int_input_low[0] =
        (s->port_int_input_low[0] & ~managed_bits) | new_low;

    if (old_k5 == s->k5_input_low &&
        old_k6 == s->k6_input_low &&
        old_p0_port_int == s->port_int_input_low[0]) {
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "bbk9288s-port: wake line %s by %s "
                  "touch-fpt=%u k5_low=0x%02x k6_low=0x%02x "
                  "p0_int_low=0x%02x\n",
                  (keyboard_active || touch_active) ? "low" : "high",
                  reason, s->touch_fpt, s->k5_input_low,
                  s->k6_input_low, s->port_int_input_low[0]);
    if (!keyboard_active || touch_active) {
        bbk9288s_key_input_update(s, reason, true);
    }
    bbk9288s_port_input_update(s, reason, true, true);
}

static unsigned bbk9288s_keyboard_line_for_qcode(int qcode)
{
    switch (qcode) {
    case Q_KEY_CODE_RET:
    case Q_KEY_CODE_KP_ENTER:
    case Q_KEY_CODE_SPC:
        return 5;
    case Q_KEY_CODE_ESC:
    case Q_KEY_CODE_BACKSPACE:
        return 4;
    case Q_KEY_CODE_UP:
        return 3;
    case Q_KEY_CODE_DOWN:
        return 2;
    case Q_KEY_CODE_LEFT:
        return 1;
    case Q_KEY_CODE_RIGHT:
        return 0;
    default:
        return BBK9288S_KEY_LINES;
    }
}

static const char *bbk9288s_keyboard_line_name(unsigned line)
{
    static const char *const names[BBK9288S_KEY_LINES] = {
        "K5.0", "K5.1", "K5.2", "K5.3", "P0.4", "P0.5",
    };

    if (line >= BBK9288S_KEY_LINES) {
        return "invalid";
    }
    return names[line];
}

static void bbk9288s_keyboard_refresh_lines(BBK9288SState *s)
{
    uint8_t k5_low = 0;
    uint8_t p0_low = 0;
    unsigned i;

    for (i = 0; i < 4; i++) {
        if (s->keyboard_line_down_count[i] != 0) {
            k5_low |= 1u << i;
        }
    }
    for (i = 4; i < BBK9288S_KEY_LINES; i++) {
        if (s->keyboard_line_down_count[i] != 0) {
            p0_low |= 1u << i;
        }
    }

    s->k5_input_low = (s->k5_input_low & ~0x0f) | k5_low;
    s->gpio_input_low[BBK9288S_KEY_GPIO_PORT] =
        (s->gpio_input_low[BBK9288S_KEY_GPIO_PORT] & ~0x30) | p0_low;
    bbk9288s_update_port_wake_line(s, "keyboard");
}

static void bbk9288s_keyboard_event(DeviceState *dev, QemuConsole *src,
                                    InputEvent *evt)
{
    BBK9288SState *s = bbk9288s_active_machine;
    InputKeyEvent *key;
    int qcode;
    unsigned line;

    (void)dev;
    (void)src;

    if (s == NULL || evt->type != INPUT_EVENT_KIND_KEY) {
        return;
    }

    key = evt->u.key.data;
    qcode = qemu_input_key_value_to_qcode(key->key);
    if (qcode < 0 || qcode >= Q_KEY_CODE__MAX) {
        return;
    }

    line = bbk9288s_keyboard_line_for_qcode(qcode);
    if (line >= BBK9288S_KEY_LINES) {
        return;
    }

    if (key->down) {
        bool line_changed;

        if (s->keyboard_qcode_down[qcode]) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bbk9288s-input: key repeat qcode=%d "
                          "mapped=%s-low wake=none\n",
                          qcode, bbk9288s_keyboard_line_name(line));
            return;
        }

        line_changed = s->keyboard_line_down_count[line] == 0;
        s->keyboard_qcode_down[qcode] = true;
        s->keyboard_line_down_count[line]++;
        bbk9288s_keyboard_refresh_lines(s);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-input: key down qcode=%d "
                      "mapped=%s-low k5=0x%02x p0=0x%02x wake=%s\n",
                      qcode, bbk9288s_keyboard_line_name(line),
                      s->k5_input_low,
                      s->gpio_input_low[BBK9288S_KEY_GPIO_PORT],
                      line_changed ? "port-input" : "none");
    } else {
        const char *line_state;

        if (!s->keyboard_qcode_down[qcode]) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bbk9288s-input: key up qcode=%d "
                          "mapped=%s-ignored\n",
                          qcode, bbk9288s_keyboard_line_name(line));
            return;
        }

        s->keyboard_qcode_down[qcode] = false;
        if (s->keyboard_line_down_count[line] != 0) {
            s->keyboard_line_down_count[line]--;
        }
        bbk9288s_keyboard_refresh_lines(s);
        line_state = s->keyboard_line_down_count[line] ? "low" : "high";
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-input: key up qcode=%d "
                      "mapped=%s-%s k5=0x%02x p0=0x%02x\n",
                      qcode, bbk9288s_keyboard_line_name(line), line_state,
                      s->k5_input_low,
                      s->gpio_input_low[BBK9288S_KEY_GPIO_PORT]);
    }
}

static const QemuInputHandler bbk9288s_keyboard_handler = {
    .name = "bbk9288s-keyboard",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = bbk9288s_keyboard_event,
};

static uint16_t bbk9288s_touch_pixel_to_adc(int pixel, unsigned max_pixel)
{
    if (pixel < 0) {
        pixel = 0;
    } else if ((unsigned)pixel > max_pixel) {
        pixel = max_pixel;
    }

    if (max_pixel == 0) {
        return 0;
    }
    return ((uint32_t)pixel * 1023u + max_pixel / 2u) / max_pixel;
}

static void bbk9288s_pointer_event(DeviceState *dev, QemuConsole *src,
                                   InputEvent *evt)
{
    BBK9288SState *s = bbk9288s_active_machine;
    InputMoveEvent *move;
    InputBtnEvent *btn;

    (void)dev;
    (void)src;

    if (s == NULL) {
        return;
    }

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        move = evt->u.abs.data;
        if (move->axis == INPUT_AXIS_X) {
            int x = qemu_input_scale_axis(move->value, INPUT_EVENT_ABS_MIN,
                                          INPUT_EVENT_ABS_MAX, 0,
                                          BBK9288S_LCD_WIDTH - 1);

            s->adc_touch_x =
                bbk9288s_touch_pixel_to_adc(x, BBK9288S_LCD_WIDTH - 1);
            s->touch_pixel_x = x;
        } else if (move->axis == INPUT_AXIS_Y) {
            int y = qemu_input_scale_axis(move->value, INPUT_EVENT_ABS_MIN,
                                          INPUT_EVENT_ABS_MAX, 0,
                                          BBK9288S_LCD_HEIGHT - 1);

            s->adc_touch_y =
                bbk9288s_touch_pixel_to_adc(
                    BBK9288S_LCD_HEIGHT - 1 - y,
                    BBK9288S_LCD_HEIGHT - 1);
            s->touch_pixel_y = y;
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        btn = evt->u.btn.data;
        if (btn->button != INPUT_BUTTON_LEFT &&
            btn->button != INPUT_BUTTON_TOUCH) {
            return;
        }
        if (s->touch_down == btn->down) {
            return;
        }
        s->touch_down = btn->down;
        s->touch_serial_logged_channels = 0;
        bbk9288s_touch_serial_reset(s);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-touch: pen %s adc-x=%u adc-y=%u fpt=%u\n",
                      s->touch_down ? "down" : "up",
                      s->adc_touch_x, s->adc_touch_y, s->touch_fpt);
        bbk9288s_update_port_wake_line(s, "touch-pen");
        break;
    default:
        break;
    }
}

static const QemuInputHandler bbk9288s_pointer_handler = {
    .name = "bbk9288s-touch",
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = bbk9288s_pointer_event,
};

static void bbk9288s_debug_usb_wakeup_cb(void *opaque)
{
    bbk9288s_raise_usb_factor(opaque, "debug-usb-wakeup-ms");
}

static void bbk9288s_debug_port4_wakeup_cb(void *opaque)
{
    bbk9288s_raise_port4_factor(opaque, "debug-port4-wakeup-ms");
}

static void bbk9288s_debug_port5_wakeup_cb(void *opaque)
{
    BBK9288SState *s = opaque;

    s->port_int_input_low[0] |= 0x20;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "bbk9288s-port: P05 diagnostic wake line low "
                  "by debug-port5-wakeup-ms p0_int_low=0x%02x\n",
                  s->port_int_input_low[0]);
    bbk9288s_port_input_update(s, "debug-port5-wakeup-ms", true, true);
}

static void bbk9288s_debug_nmi_wakeup_cb(void *opaque)
{
    BBK9288SState *s = opaque;

    qemu_log_mask(LOG_GUEST_ERROR, "bbk9288s-itc: NMI raised by debug timer\n");
    if (s->cpu != NULL) {
        s1c33_cpu_raise_nmi(s->cpu);
    }
}

static uint16_t bbk9288s_timer16_lduw(BBK9288SState *s, hwaddr offset)
{
    return (uint16_t)s->io_regs[offset] |
           ((uint16_t)s->io_regs[offset + 1] << 8);
}

static void bbk9288s_timer16_stw(BBK9288SState *s, hwaddr offset,
                                 uint16_t value)
{
    s->io_regs[offset] = value & 0xff;
    s->io_regs[offset + 1] = value >> 8;
}

static bool bbk9288s_timer16_channel_running(BBK9288SState *s,
                                             unsigned channel)
{
    uint8_t ctrl = s->io_regs[BBK9288S_16TM_CTRL(channel)];

    if (channel != 0) {
        return false;
    }
    if ((ctrl & BBK9288S_16TM_CTRL_PRUN) == 0) {
        return false;
    }
    return (ctrl & BBK9288S_16TM_CTRL_CKSL) == 0;
}

static void bbk9288s_timer16_compare_match(BBK9288SState *s,
                                           unsigned channel,
                                           const char *which,
                                           uint8_t irq_bit,
                                           uint16_t count)
{
    uint8_t eir = s->io_regs[BBK9288S_EIR2_16TM0_1];
    uint8_t fir = s->io_regs[BBK9288S_FIR2_16TM0_1];
    uint8_t idma_request = s->io_regs[0x290] & irq_bit;
    uint8_t idma_enable = s->io_regs[0x294] & irq_bit;

    if (s->trace_key_scan || s->trace_io || s->debug_timer16_factors) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-timer: 16tm%u-%s compare match "
                      "count=0x%04x cmpa=0x%04x cmpb=0x%04x "
                      "eir=0x%02x fir=0x%02x idma-req=0x%02x "
                      "idma-en=0x%02x debug-factors=%u\n",
                      channel, which, count,
                      bbk9288s_timer16_lduw(s,
                                            BBK9288S_16TM_COMPARE_A(channel)),
                      bbk9288s_timer16_lduw(s,
                                            BBK9288S_16TM_COMPARE_B(channel)),
                      eir, fir, idma_request, idma_enable,
                      s->debug_timer16_factors ? 1 : 0);
    }

    if (!s->debug_timer16_factors) {
        return;
    }
    if (idma_request || idma_enable) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-timer: 16tm%u-%s factor suppressed because "
                      "IDMA routing is configured req=0x%02x en=0x%02x\n",
                      channel, which, idma_request, idma_enable);
        return;
    }

    s->io_regs[BBK9288S_FIR2_16TM0_1] |= irq_bit;
    bbk9288s_update_irq(s);
}

static void bbk9288s_timer16_tick_cb(void *opaque)
{
    BBK9288SState *s = opaque;
    uint16_t count;
    uint16_t cmpa;
    uint16_t cmpb;

    if (!bbk9288s_timer16_channel_running(s, 0)) {
        return;
    }

    count = bbk9288s_timer16_lduw(s, BBK9288S_16TM0_COUNTER);
    cmpa = bbk9288s_timer16_lduw(s, BBK9288S_16TM0_COMPARE_A);
    cmpb = bbk9288s_timer16_lduw(s, BBK9288S_16TM0_COMPARE_B);
    count++;
    bbk9288s_timer16_stw(s, BBK9288S_16TM0_COUNTER, count);

    if (cmpa != 0 && count == cmpa) {
        bbk9288s_timer16_compare_match(s, 0, "a",
                                       BBK9288S_16TM0A_IRQ_BIT, count);
    }
    if (cmpb != 0 && count == cmpb) {
        bbk9288s_timer16_compare_match(s, 0, "b",
                                       BBK9288S_16TM0B_IRQ_BIT, count);
        bbk9288s_timer16_stw(s, BBK9288S_16TM0_COUNTER, 0);
    }

    bbk9288s_timer16_update(s);
}

static void bbk9288s_timer16_update(BBK9288SState *s)
{
    if (!bbk9288s_timer16_channel_running(s, 0)) {
        if (s->timer16_tick_timer != NULL) {
            timer_del(s->timer16_tick_timer);
        }
        return;
    }
    if (s->timer16_tick_timer == NULL) {
        s->timer16_tick_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME, bbk9288s_timer16_tick_cb, s);
    }
    if (!timer_pending(s->timer16_tick_timer)) {
        timer_mod(s->timer16_tick_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  BBK9288S_16TM_TICK_MS);
    }
}

static bool bbk9288s_timer2b_armed(BBK9288SState *s)
{
    uint8_t clock_ctrl =
        s->io_regs[BBK9288S_16TM_CLOCK_CTRL(2)];
    uint8_t timer_ctrl = s->io_regs[BBK9288S_16TM_CTRL(2)];

    return (clock_ctrl & 0x08) != 0 &&
           (timer_ctrl & BBK9288S_16TM_CTRL_PRUN) != 0 &&
           (timer_ctrl & BBK9288S_16TM_CTRL_CKSL) == 0 &&
           bbk9288s_timer16_lduw(s, BBK9288S_16TM_COMPARE_B(2)) != 0;
}

static uint64_t bbk9288s_timer2b_period_ms(BBK9288SState *s)
{
    static const uint16_t divisors[8] = {
        4096, 1024, 256, 64, 16, 4, 2, 1,
    };
    uint16_t compare =
        bbk9288s_timer16_lduw(s, BBK9288S_16TM_COMPARE_B(2));
    uint8_t clock_ctrl =
        s->io_regs[BBK9288S_16TM_CLOCK_CTRL(2)];
    uint64_t cycles = (uint64_t)compare * divisors[clock_ctrl & 0x07];

    /* The 9288S selects OSC1 as the prescaler source for its system tick. */
    if ((s->io_regs[BBK9288S_PRESCALER_CLOCK_SELECT] &
         BBK9288S_PRESCALER_OSC1) == 0) {
        return 1;
    }
    return MAX(DIV_ROUND_UP(cycles * 1000, BBK9288S_OSC1_HZ), 1);
}

static void bbk9288s_timer2b_compare_cb(void *opaque)
{
    BBK9288SState *s = opaque;

    if (!bbk9288s_timer2b_armed(s)) {
        bbk9288s_timer2b_update(s);
        return;
    }

    /* A comparison-B match raises F16TU2 and resets the counter to zero. */
    bbk9288s_timer16_stw(s, BBK9288S_16TM_COUNTER(2), 0);
    s->io_regs[BBK9288S_FIR3_16TM2_3] |= BBK9288S_16TM2B_IRQ_BIT;
    if (s->trace_io) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-timer: 16tm2-b factor set "
                      "cmpb=0x%04x eir=0x%02x fir=0x%02x "
                      "pir=0x%02x period=%" PRIu64 "ms\n",
                      bbk9288s_timer16_lduw(
                          s, BBK9288S_16TM_COMPARE_B(2)),
                      s->io_regs[BBK9288S_EIR3_16TM2_3],
                      s->io_regs[BBK9288S_FIR3_16TM2_3],
                      s->io_regs[BBK9288S_PIR3_16TM2_3],
                      bbk9288s_timer2b_period_ms(s));
    }
    bbk9288s_update_irq(s);
    bbk9288s_timer2b_update(s);
}

static void bbk9288s_timer2b_update(BBK9288SState *s)
{
    if (!bbk9288s_timer2b_armed(s)) {
        if (s->timer2b_compare_timer != NULL) {
            timer_del(s->timer2b_compare_timer);
        }
        return;
    }
    if (s->timer2b_compare_timer == NULL) {
        s->timer2b_compare_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME,
                         bbk9288s_timer2b_compare_cb, s);
    }
    if (!timer_pending(s->timer2b_compare_timer)) {
        timer_mod(s->timer2b_compare_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  bbk9288s_timer2b_period_ms(s));
    }
}

static void bbk9288s_timer3b_wakeup_cb(void *opaque)
{
    BBK9288SState *s = opaque;
    uint8_t priority = (s->io_regs[BBK9288S_PIR3_16TM2_3] >> 4) & 0x07;

    if ((s->io_regs[BBK9288S_16TM_CTRL(3)] &
         BBK9288S_16TM_CTRL_PRUN) == 0 ||
        (s->io_regs[BBK9288S_EIR3_16TM2_3] &
         BBK9288S_16TM3B_IRQ_BIT) == 0) {
        bbk9288s_timer3b_update(s);
        return;
    }
    s->io_regs[BBK9288S_FIR3_16TM2_3] |= BBK9288S_16TM3B_IRQ_BIT;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "bbk9288s-timer: 16tm3-b factor set "
                  "eir=0x%02x fir=0x%02x pir=0x%02x level=%u\n",
                  s->io_regs[BBK9288S_EIR3_16TM2_3],
                  s->io_regs[BBK9288S_FIR3_16TM2_3],
                  s->io_regs[BBK9288S_PIR3_16TM2_3],
                  bbk9288s_irq_level_or_one(priority));
    bbk9288s_update_irq(s);
}

static void bbk9288s_timer3b_update(BBK9288SState *s)
{
    bool enabled =
        (s->io_regs[BBK9288S_EIR3_16TM2_3] & BBK9288S_16TM3B_IRQ_BIT) != 0;
    bool running =
        (s->io_regs[BBK9288S_16TM_CTRL(3)] &
         BBK9288S_16TM_CTRL_PRUN) != 0;
    bool pending =
        (s->io_regs[BBK9288S_FIR3_16TM2_3] & BBK9288S_16TM3B_IRQ_BIT) != 0;

    if (!enabled || !running) {
        if (s->timer3b_wakeup_timer != NULL) {
            timer_del(s->timer3b_wakeup_timer);
        }
        return;
    }
    if (pending) {
        return;
    }
    if (s->timer3b_wakeup_timer == NULL) {
        s->timer3b_wakeup_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME, bbk9288s_timer3b_wakeup_cb, s);
    }
    if (!timer_pending(s->timer3b_wakeup_timer)) {
        timer_mod(s->timer3b_wakeup_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  BBK9288S_16TM3B_WAKE_MS);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-timer: 16tm3-b wake scheduled after %u ms\n",
                      BBK9288S_16TM3B_WAKE_MS);
    }
}

static uint8_t bbk9288s_timer8_running_mask(BBK9288SState *s)
{
    uint8_t running = 0;
    unsigned channel;

    for (channel = 0; channel < BBK9288S_8TM_CHANNELS; channel++) {
        hwaddr ctrl = BBK9288S_8TM_CTRL_BASE +
                      channel * BBK9288S_8TM_STRIDE;

        if ((s->io_regs[ctrl] & 0x01) != 0) {
            running |= 1u << channel;
        }
    }
    return running;
}

static bool bbk9288s_timer8_sleep_resume_enabled(BBK9288SState *s,
                                                 uint8_t underflow)
{
    S1C33CPU *cpu;

    if ((underflow & BBK9288S_8TM1_IRQ_BIT) == 0 || s->cpu == NULL) {
        return false;
    }

    cpu = S1C33_CPU(s->cpu);
    if (!cpu->env.in_sleep) {
        return false;
    }

    return (s->io_regs[BBK9288S_CLG_CLOCK_OPT] &
            BBK9288S_CLOCK_OPT_8T1ON_OFF) == 0;
}

static void bbk9288s_timer8_update(BBK9288SState *s)
{
    uint8_t armed = bbk9288s_timer8_running_mask(s) &
                    ~s->io_regs[BBK9288S_FIR5_8TM0_3] &
                    BBK9288S_8TM0_3_MASK;

    if (armed == 0) {
        if (s->timer8_underflow_timer != NULL) {
            timer_del(s->timer8_underflow_timer);
        }
        return;
    }

    if (s->timer8_underflow_timer == NULL) {
        s->timer8_underflow_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME, bbk9288s_timer8_underflow_cb, s);
    }
    if (!timer_pending(s->timer8_underflow_timer)) {
        timer_mod(s->timer8_underflow_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  BBK9288S_8TM_UNDERFLOW_MS);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-timer: 8tm0-3 underflow scheduled "
                      "after %u ms running=0x%02x fir=0x%02x\n",
                      BBK9288S_8TM_UNDERFLOW_MS,
                      bbk9288s_timer8_running_mask(s),
                      s->io_regs[BBK9288S_FIR5_8TM0_3]);
    }
}

static void bbk9288s_timer8_underflow_cb(void *opaque)
{
    BBK9288SState *s = opaque;
    uint8_t underflow = bbk9288s_timer8_running_mask(s) &
                        ~s->io_regs[BBK9288S_FIR5_8TM0_3] &
                        BBK9288S_8TM0_3_MASK;
    bool sleep_resume = bbk9288s_timer8_sleep_resume_enabled(s, underflow);

    if (underflow == 0) {
        return;
    }

    s->io_regs[BBK9288S_FIR5_8TM0_3] |= underflow;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "bbk9288s-timer: 8tm0-3 underflow factor set "
                  "mask=0x%02x eir=0x%02x fir=0x%02x pir=0x%02x "
                  "level=%u\n",
                  underflow, s->io_regs[BBK9288S_EIR5_8TM0_3],
                  s->io_regs[BBK9288S_FIR5_8TM0_3],
                  s->io_regs[BBK9288S_PIR_8TM_SIF0],
                  bbk9288s_irq_level_or_one(
                      s->io_regs[BBK9288S_PIR_8TM_SIF0] & 0x07));
    if (sleep_resume) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-timer: 8tm1 underflow releases sleep "
                      "clock-opt=0x%02x eir=0x%02x\n",
                      s->io_regs[BBK9288S_CLG_CLOCK_OPT],
                      s->io_regs[BBK9288S_EIR5_8TM0_3]);
        s1c33_cpu_resume_from_sleep(s->cpu, "8tm1-osc3-stabilize");
    }
    bbk9288s_update_irq(s);
    bbk9288s_timer8_update(s);
}

static uint16_t bbk9288s_touch_channel_value(BBK9288SState *s,
                                             unsigned channel)
{
    if (channel == 0) {
        return BBK9288S_ADC_BATTERY_OK;
    }

    if (!s->touch_down) {
        return 0x03ff;
    }

    switch (channel) {
    case 1:
        return s->adc_touch_x & 0x03ff;
    case 2:
        return s->adc_touch_y & 0x03ff;
    case 3:
        return 0x0100;
    default:
        return 0x0200;
    }
}

static bool bbk9288s_adc_buffer_info(hwaddr offset, unsigned *channel)
{
    if (offset < BBK9288S_ADC_BUFFER_BASE ||
        offset >= BBK9288S_ADC_BUFFER_BASE + BBK9288S_ADC_CHANNELS * 2) {
        return false;
    }

    *channel = (offset - BBK9288S_ADC_BUFFER_BASE) / 2;
    return true;
}

static void bbk9288s_adc_store_result(BBK9288SState *s, unsigned channel,
                                      uint16_t value)
{
    hwaddr buffer = BBK9288S_ADC_BUFFER_BASE + channel * 2;

    value &= 0x03ff;
    s->io_regs[BBK9288S_ADC_DATA_LO] = value & 0xff;
    s->io_regs[BBK9288S_ADC_DATA_HI] = (value >> 8) & 0x03;
    s->io_regs[buffer] = value & 0xff;
    s->io_regs[buffer + 1] = (value >> 8) & 0x03;
    s->io_regs[BBK9288S_ADC_FLAGS] |= 1u << channel;
    s->adc_last_channel = channel;
}

static bool bbk9288s_adc_completion_irq_enabled(BBK9288SState *s,
                                                unsigned channel)
{
    if ((s->io_regs[BBK9288S_ADC_MODE] & 1) == 0) {
        return true;
    }
    if ((s->io_regs[BBK9288S_ADC_ENABLE] & 0x10) == 0) {
        return false;
    }
    return (s->io_regs[BBK9288S_ADC_INTMASK] & (1u << channel)) != 0;
}

static void bbk9288s_adc_complete(BBK9288SState *s, const char *reason)
{
    unsigned start = s->io_regs[BBK9288S_ADC_CHANNEL] & 0x07;
    unsigned end = (s->io_regs[BBK9288S_ADC_CHANNEL] >> 3) & 0x07;
    unsigned channel = start;
    unsigned count;
    unsigned irq_channel = start;
    bool set_irq = false;

    for (count = 0; count < BBK9288S_ADC_CHANNELS; count++) {
        uint16_t value = bbk9288s_touch_channel_value(s, channel);

        bbk9288s_adc_store_result(s, channel, value);
        s->io_regs[BBK9288S_ADC_TRIGGER] =
            (s->io_regs[BBK9288S_ADC_TRIGGER] & 0x38) | channel;
        if (bbk9288s_adc_completion_irq_enabled(s, channel)) {
            set_irq = true;
            irq_channel = channel;
        }
        if (channel == end) {
            break;
        }
        channel = (channel + 1) & 0x07;
    }

    s->io_regs[BBK9288S_ADC_ENABLE] |= 0x08;
    if ((s->io_regs[BBK9288S_ADC_TRIGGER] & 0x20) == 0) {
        s->io_regs[BBK9288S_ADC_ENABLE] &= ~0x02;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "bbk9288s-adc: complete %s ch=%u..%u last=%u "
                  "add=0x%03x flags=0x%02x touch=%s x=%u y=%u\n",
                  reason, start, end, s->adc_last_channel,
                  ((uint16_t)s->io_regs[BBK9288S_ADC_DATA_HI] << 8) |
                  s->io_regs[BBK9288S_ADC_DATA_LO],
                  s->io_regs[BBK9288S_ADC_FLAGS],
                  s->touch_down ? "down" : "up",
                  s->adc_touch_x, s->adc_touch_y);
    if (set_irq) {
        bbk9288s_raise_adc_factor(s, reason, irq_channel);
    }
}

static uint8_t bbk9288s_adc_read_byte(BBK9288SState *s, hwaddr offset)
{
    const BBK9288SRegInfo *reg = bbk9288s_find_reg(offset);
    uint8_t value = s->io_regs[offset];
    unsigned channel;

    switch (offset) {
    case BBK9288S_ADC_DATA_LO:
    case BBK9288S_ADC_DATA_HI:
        s->io_regs[BBK9288S_ADC_ENABLE] &= ~0x09;
        return offset == BBK9288S_ADC_DATA_HI ? value & 0x03 : value;
    case BBK9288S_ADC_FLAGS:
    case BBK9288S_ADC_OVERWRITE:
        return value;
    default:
        break;
    }

    if (bbk9288s_adc_buffer_info(offset, &channel)) {
        s->io_regs[BBK9288S_ADC_FLAGS] &= ~(1u << channel);
        s->io_regs[BBK9288S_ADC_OVERWRITE] &= ~(1u << channel);
        if (s->io_regs[BBK9288S_ADC_OVERWRITE] == 0) {
            s->io_regs[BBK9288S_ADC_ENABLE] &= ~0x01;
        }
        return (offset & 1) ? value & 0x03 : value;
    }

    return reg != NULL ? value & reg->mask : value;
}

static bool bbk9288s_adc_write_byte(BBK9288SState *s, hwaddr offset,
                                    uint8_t value)
{
    const BBK9288SRegInfo *reg;

    if (!bbk9288s_is_adc_reg(offset)) {
        return false;
    }

    switch (offset) {
    case BBK9288S_ADC_DATA_LO:
    case BBK9288S_ADC_DATA_HI:
        return true;
    case BBK9288S_ADC_TRIGGER:
        s->io_regs[offset] =
            (s->io_regs[offset] & 0x07) | (value & 0x38);
        return true;
    case BBK9288S_ADC_CHANNEL:
        s->io_regs[offset] = value & 0x3f;
        return true;
    case BBK9288S_ADC_ENABLE: {
        uint8_t flags = s->io_regs[offset] & 0x09;

        if ((value & 0x01) == 0) {
            flags &= ~0x01;
        }
        s->io_regs[offset] = flags | (value & 0x76);
        if ((s->io_regs[offset] & 0x04) == 0) {
            s->io_regs[offset] &= ~0x02;
        }
        if ((value & 0x02) != 0 && (s->io_regs[offset] & 0x04) != 0) {
            if ((s->io_regs[BBK9288S_ADC_TRIGGER] & 0x18) == 0) {
                bbk9288s_adc_complete(s, "software-trigger");
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "bbk9288s-adc: ADST ignored, trigger mode "
                              "ADTRG=0x%02x is not software\n",
                              s->io_regs[BBK9288S_ADC_TRIGGER]);
            }
        }
        return true;
    }
    case BBK9288S_ADC_SAMPLING:
        s->io_regs[offset] = value & 0xf3;
        return true;
    case BBK9288S_ADC_FLAGS:
        return true;
    case BBK9288S_ADC_OVERWRITE:
        s->io_regs[offset] &= value;
        if (s->io_regs[offset] == 0) {
            s->io_regs[BBK9288S_ADC_ENABLE] &= ~0x01;
        }
        return true;
    default:
        if (offset >= BBK9288S_ADC_BUFFER_BASE &&
            offset < BBK9288S_ADC_BUFFER_BASE + BBK9288S_ADC_CHANNELS * 2) {
            return true;
        }
        reg = bbk9288s_find_reg(offset);
        s->io_regs[offset] = reg != NULL ? value & reg->mask : value;
        return true;
    }
}

static bool bbk9288s_is_clock_timer_reg(hwaddr offset)
{
    return offset >= BBK9288S_CTM_RUN_CTRL &&
           offset <= BBK9288S_CTM_DAY_CMP;
}

static uint8_t bbk9288s_clock_timer_read_byte(BBK9288SState *s,
                                              hwaddr offset)
{
    const BBK9288SRegInfo *reg = bbk9288s_find_reg(offset);
    uint8_t value = s->io_regs[offset];

    switch (offset) {
    case BBK9288S_CTM_RUN_CTRL:
        return value & BBK9288S_CTM_TCRUN;
    case BBK9288S_CTM_INT_CTRL:
    case BBK9288S_CTM_DIVIDER:
        return value;
    default:
        return reg != NULL ? value & reg->mask : value;
    }
}

static bool bbk9288s_clock_timer_write_byte(BBK9288SState *s, hwaddr offset,
                                            uint8_t value)
{
    const BBK9288SRegInfo *reg;

    if (!bbk9288s_is_clock_timer_reg(offset)) {
        return false;
    }

    switch (offset) {
    case BBK9288S_CTM_RUN_CTRL:
        if ((value & BBK9288S_CTM_TCRST) != 0 &&
            (value & BBK9288S_CTM_TCRUN) == 0 &&
            (s->io_regs[offset] & BBK9288S_CTM_TCRUN) == 0) {
            bbk9288s_clock_timer_reset_counters(s);
        }
        s->io_regs[offset] = value & BBK9288S_CTM_TCRUN;
        return true;
    case BBK9288S_CTM_INT_CTRL: {
        uint8_t flags = s->io_regs[offset] &
                        (BBK9288S_CTM_TCIF | BBK9288S_CTM_TCAF);

        flags &= ~(value & (BBK9288S_CTM_TCIF | BBK9288S_CTM_TCAF));
        s->io_regs[offset] = (value & BBK9288S_CTM_SELECT_MASK) | flags;
        return true;
    }
    case BBK9288S_CTM_DIVIDER:
    case BBK9288S_CTM_SEC:
        return true;
    default:
        reg = bbk9288s_find_reg(offset);
        s->io_regs[offset] = reg != NULL ? value & reg->mask : value;
        return true;
    }
}

static uint16_t bbk9288s_io_lduw_raw(BBK9288SState *s, hwaddr offset)
{
    return (uint16_t)s->io_regs[offset] |
           ((uint16_t)s->io_regs[offset + 1] << 8);
}

static void bbk9288s_io_stw_raw(BBK9288SState *s, hwaddr offset,
                                uint16_t value)
{
    s->io_regs[offset] = value & 0xff;
    s->io_regs[offset + 1] = value >> 8;
}

static bool bbk9288s_gpio_data_info(hwaddr offset, hwaddr *ctrl_offset,
                                    uint8_t *mask, unsigned *port_index)
{
    switch (offset) {
    case BBK9288S_PORT0_DATA:
        *ctrl_offset = BBK9288S_PORT0_IOCTRL;
        *mask = 0xff;
        *port_index = 0;
        return true;
    case BBK9288S_PORT1_DATA:
        *ctrl_offset = BBK9288S_PORT1_IOCTRL;
        *mask = 0x7f;
        *port_index = 1;
        return true;
    case BBK9288S_PORT2_DATA:
        *ctrl_offset = BBK9288S_PORT2_IOCTRL;
        *mask = 0xff;
        *port_index = 2;
        return true;
    case BBK9288S_PORT3_DATA:
        *ctrl_offset = BBK9288S_PORT3_IOCTRL;
        *mask = 0x3f;
        *port_index = 3;
        return true;
    default:
        return false;
    }
}

static uint8_t bbk9288s_touch_gpio_low_mask(BBK9288SState *s)
{
    if (!s->touch_p0_low || !s->touch_down || s->touch_fpt > 7) {
        return 0;
    }

    return 1u << s->touch_fpt;
}

static uint8_t bbk9288s_gpio_input_low_mask(BBK9288SState *s,
                                            unsigned port_index)
{
    uint8_t input_low = s->gpio_input_low[port_index];
    uint8_t touch_low = 0;

    if (port_index == BBK9288S_KEY_GPIO_PORT) {
        touch_low = bbk9288s_touch_gpio_low_mask(s) & BBK9288S_KEY_P0_MASK;
        input_low |= s->debug_key_p0_low_mask & BBK9288S_KEY_P0_MASK;
    }

    /*
     * The firmware scan loop writes P1D bit 3 low before reading P0D. Until
     * the real matrix is mapped, expose host keys only during that strobe.
     */
    if (port_index == BBK9288S_KEY_GPIO_PORT &&
        (s->io_regs[BBK9288S_PORT1_DATA] & BBK9288S_KEY_STROBE_BIT)) {
        input_low = 0;
    }

    if (port_index == BBK9288S_KEY_GPIO_PORT) {
        input_low |= touch_low;
    }

    return input_low;
}

static uint8_t bbk9288s_gpio_data_read(BBK9288SState *s, hwaddr offset,
                                        hwaddr ctrl_offset, uint8_t mask,
                                        unsigned port_index)
{
    uint8_t latch = s->io_regs[offset] & mask;
    uint8_t output = s->io_regs[ctrl_offset] & mask;
    uint8_t input = mask & ~bbk9288s_gpio_input_low_mask(s, port_index);

    return (latch & output) | (input & ~output);
}

static uint8_t bbk9288s_k_input_data_read(BBK9288SState *s, hwaddr offset)
{
    switch (offset) {
    case BBK9288S_K5_DATA:
        return (~s->k5_input_low) &
               ~s->io_regs[BBK9288S_K5_FUNC_SELECT] & 0x1f;
    case BBK9288S_K6_DATA:
        return (~s->k6_input_low) &
               ~s->io_regs[BBK9288S_K6_FUNC_SELECT] & 0xff;
    default:
        g_assert_not_reached();
    }
}

static uint8_t bbk9288s_key_input_gpio_value(BBK9288SState *s, unsigned port)
{
    switch (port) {
    case 0:
        return bbk9288s_gpio_data_read(s, BBK9288S_PORT0_DATA,
                                       BBK9288S_PORT0_IOCTRL, 0xff, 0);
    case 2:
        return bbk9288s_gpio_data_read(s, BBK9288S_PORT2_DATA,
                                       BBK9288S_PORT2_IOCTRL, 0xff, 2);
    default:
        g_assert_not_reached();
    }
}

static uint8_t bbk9288s_key_input_selected_value(BBK9288SState *s,
                                                 unsigned key,
                                                 const char **source)
{
    uint8_t select = (s->io_regs[BBK9288S_KEY_INPUT_SELECT] >>
                      (key * 2)) & 0x03;

    if (key == 0) {
        switch (select) {
        case 0:
            *source = "K5[4:0]";
            return bbk9288s_k_input_data_read(s, BBK9288S_K5_DATA) & 0x1f;
        case 1:
            *source = "K6[4:0]";
            return bbk9288s_k_input_data_read(s, BBK9288S_K6_DATA) & 0x1f;
        case 2:
            *source = "P0[4:0]";
            return bbk9288s_key_input_gpio_value(s, 0) & 0x1f;
        case 3:
            *source = "P2[4:0]";
            return bbk9288s_key_input_gpio_value(s, 2) & 0x1f;
        default:
            g_assert_not_reached();
        }
    }

    switch (select) {
    case 0:
        *source = "K6[3:0]";
        return bbk9288s_k_input_data_read(s, BBK9288S_K6_DATA) & 0x0f;
    case 1:
        *source = "K6[7:4]";
        return (bbk9288s_k_input_data_read(s, BBK9288S_K6_DATA) >> 4) & 0x0f;
    case 2:
        *source = "P0[7:4]";
        return (bbk9288s_key_input_gpio_value(s, 0) >> 4) & 0x0f;
    case 3:
        *source = "P2[7:4]";
        return (bbk9288s_key_input_gpio_value(s, 2) >> 4) & 0x0f;
    default:
        g_assert_not_reached();
    }
}

static uint8_t bbk9288s_key_input_condition(BBK9288SState *s, unsigned key)
{
    return key == 0 ? s->io_regs[BBK9288S_KEY_INPUT_CONDITION0] & 0x1f :
                      s->io_regs[BBK9288S_KEY_INPUT_CONDITION1] & 0x0f;
}

static uint8_t bbk9288s_key_input_mask(BBK9288SState *s, unsigned key)
{
    return key == 0 ? s->io_regs[BBK9288S_KEY_INPUT_MASK0] & 0x1f :
                      s->io_regs[BBK9288S_KEY_INPUT_MASK1] & 0x0f;
}

static void bbk9288s_key_input_update(BBK9288SState *s, const char *reason,
                                      bool allow_edges)
{
    bool changed = false;
    unsigned key;

    for (key = 0; key < 2; key++) {
        const char *source = NULL;
        uint8_t current =
            bbk9288s_key_input_selected_value(s, key, &source);
        uint8_t previous = s->key_input_high[key];
        uint8_t compare = bbk9288s_key_input_condition(s, key);
        uint8_t mask = bbk9288s_key_input_mask(s, key);
        uint8_t was_matched = ~(previous ^ compare) & mask;
        uint8_t now_unmatched = (current ^ compare) & mask;
        uint8_t trigger = was_matched & now_unmatched;
        uint8_t irq_bit = key == 0 ? BBK9288S_KEY0_IRQ_BIT :
                                     BBK9288S_KEY1_IRQ_BIT;

        if (!s->key_input_high_valid[key]) {
            s->key_input_high[key] = current;
            s->key_input_high_valid[key] = true;
            continue;
        }

        if (allow_edges && trigger != 0) {
            if ((s->io_regs[BBK9288S_FIR0_KEY_PORT0_3] & irq_bit) == 0) {
                changed = true;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "bbk9288s-keyinput: FK%u factor set by %s "
                              "source=%s old=0x%02x new=0x%02x "
                              "compare=0x%02x mask=0x%02x edge=0x%02x "
                              "eir=0x%02x fir=0x%02x pir=0x%02x "
                              "level=%u\n",
                              key, reason, source, previous, current,
                              compare, mask, trigger,
                              s->io_regs[BBK9288S_EIR0_KEY_PORT0_3],
                              s->io_regs[BBK9288S_FIR0_KEY_PORT0_3] | irq_bit,
                              s->io_regs[BBK9288S_PIR_KEY_PORT0_3],
                              bbk9288s_irq_level_or_one(
                                  key == 0 ?
                                  s->io_regs[BBK9288S_PIR_KEY_PORT0_3] & 0x07 :
                                  (s->io_regs[BBK9288S_PIR_KEY_PORT0_3] >> 4) &
                                  0x07));
            }
            s->io_regs[BBK9288S_FIR0_KEY_PORT0_3] |= irq_bit;
        } else if (s->trace_key_scan && previous != current) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bbk9288s-keyinput: FK%u line change by %s "
                          "source=%s old=0x%02x new=0x%02x "
                          "compare=0x%02x mask=0x%02x allow_edges=%u\n",
                          key, reason, source, previous, current, compare,
                          mask, allow_edges ? 1 : 0);
        }

        s->key_input_high[key] = current;
    }

    if (changed) {
        bbk9288s_update_irq(s);
    }
}

static const char *bbk9288s_key_scan_reg_name(hwaddr offset)
{
    switch (offset) {
    case BBK9288S_PORT0_DATA:
        return "P0D";
    case BBK9288S_PORT0_IOCTRL:
        return "IOC0";
    case BBK9288S_PORT1_DATA:
        return "P1D";
    case BBK9288S_PORT1_IOCTRL:
        return "IOC1";
    case BBK9288S_K5_DATA:
        return "K5D";
    case BBK9288S_K6_DATA:
        return "K6D";
    default:
        return NULL;
    }
}

static void bbk9288s_key_scan_trace(BBK9288SState *s, bool is_write,
                                    hwaddr offset, uint8_t value)
{
    const char *reg = bbk9288s_key_scan_reg_name(offset);
    uint8_t host_low;
    uint8_t debug_low;
    uint8_t touch_low;
    uint8_t raw_low;
    uint8_t gated_low;
    uint8_t p1_latch;

    if (!s->trace_key_scan || reg == NULL) {
        return;
    }

    host_low = s->gpio_input_low[BBK9288S_KEY_GPIO_PORT] &
               BBK9288S_KEY_P0_MASK;
    debug_low = s->debug_key_p0_low_mask & BBK9288S_KEY_P0_MASK;
    touch_low = bbk9288s_touch_gpio_low_mask(s) & BBK9288S_KEY_P0_MASK;
    raw_low = host_low | debug_low | touch_low;
    gated_low = bbk9288s_gpio_input_low_mask(s, BBK9288S_KEY_GPIO_PORT) &
                BBK9288S_KEY_P0_MASK;
    p1_latch = s->io_regs[BBK9288S_PORT1_DATA] & 0x7f;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "bbk9288s-keyscan: %s pc=0x%08x reg=%s value=0x%02x "
                  "p1-latch=0x%02x p1-ioc=0x%02x strobe=%s "
                  "p0-latch=0x%02x p0-ioc=0x%02x host-low=0x%02x "
                  "debug-low=0x%02x touch-low=0x%02x raw-low=0x%02x "
                  "gated-low=0x%02x k5-low=0x%02x cfk5=0x%02x "
                  "k6-low=0x%02x cfk6=0x%02x touch=%s\n",
                  is_write ? "write" : "read", bbk9288s_current_pc(),
                  reg, value, p1_latch,
                  s->io_regs[BBK9288S_PORT1_IOCTRL] & 0x7f,
                  (p1_latch & BBK9288S_KEY_STROBE_BIT) ? "off" : "on",
                  s->io_regs[BBK9288S_PORT0_DATA],
                  s->io_regs[BBK9288S_PORT0_IOCTRL],
                  host_low, debug_low, touch_low, raw_low, gated_low,
                  s->k5_input_low,
                  s->io_regs[BBK9288S_K5_FUNC_SELECT] & 0x1f,
                  s->k6_input_low,
                  s->io_regs[BBK9288S_K6_FUNC_SELECT],
                  s->touch_down ? "down" : "up");
}

static hwaddr bbk9288s_hsdma_channel_base(unsigned channel)
{
    return BBK9288S_HSDMA_BASE + channel * BBK9288S_HSDMA_CH_SIZE;
}

static uint32_t bbk9288s_hsdma_address(uint16_t low, uint16_t high)
{
    return ((uint32_t)(high & 0x0fff) << 16) | low;
}

static uint32_t bbk9288s_hsdma_next_address(uint32_t addr, unsigned control,
                                            unsigned unit_size)
{
    switch (control) {
    case 3: /* Increment without initialization. */
    case 2: /* Increment with initialization. */
        return addr + unit_size;
    case 1:
        return addr - unit_size;
    default:
        return addr;
    }
}

static void bbk9288s_ivram_stats_add(BBK9288SIVRAMStats *stats,
                                     uint32_t addr, const uint8_t *buf,
                                     unsigned size)
{
    unsigned i;

    for (i = 0; i < size; i++) {
        uint32_t phys = addr + i;
        uint32_t offset;
        uint8_t value;

        if (phys < BBK9288S_IVRAM_BASE ||
            phys >= BBK9288S_IVRAM_BASE + BBK9288S_IVRAM_SIZE) {
            continue;
        }

        offset = phys - BBK9288S_IVRAM_BASE;
        value = buf[i];
        if (stats->bytes == 0) {
            stats->first_offset = offset;
        }
        stats->last_offset = offset;
        stats->bytes++;
        stats->checksum += value;
        if (value != 0) {
            stats->nonzero++;
        }
    }
}

static void bbk9288s_hsdma_log_ivram_stats(unsigned channel, const char *side,
                                           const BBK9288SIVRAMStats *stats)
{
    if (stats->bytes == 0) {
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "bbk9288s-hsdma: ch%u ivram-%s bytes=%u nonzero=%u "
                  "checksum=0x%08x range=0x%04x..0x%04x\n",
                  channel, side, stats->bytes, stats->nonzero,
                  stats->checksum, stats->first_offset, stats->last_offset);
}

static void bbk9288s_hsdma_finish_channel(BBK9288SState *s, unsigned channel,
                                          hwaddr base, uint16_t ctrl)
{
    s->io_regs[base + 0x0c] &= ~1u;
    s->io_regs[base + 0x0e] &= ~1u;
    bbk9288s_io_stw_raw(s, base + 0x00, 0);
    bbk9288s_io_stw_raw(s, base + 0x02, ctrl & 0xff00);
    s->io_regs[0x281] |= 1u << channel;
    bbk9288s_update_irq(s);
}

static void bbk9288s_hsdma_run_channel(BBK9288SState *s, unsigned channel)
{
    hwaddr base = bbk9288s_hsdma_channel_base(channel);
    uint16_t count_reg = bbk9288s_io_lduw_raw(s, base + 0x00);
    uint16_t ctrl = bbk9288s_io_lduw_raw(s, base + 0x02);
    uint16_t src_low = bbk9288s_io_lduw_raw(s, base + 0x04);
    uint16_t src_high = bbk9288s_io_lduw_raw(s, base + 0x06);
    uint16_t dst_low = bbk9288s_io_lduw_raw(s, base + 0x08);
    uint16_t dst_high = bbk9288s_io_lduw_raw(s, base + 0x0a);
    bool dual = (ctrl & 0x8000) != 0;
    unsigned mode = (dst_high >> 14) & 0x3;
    unsigned unit_size = (src_high & 0x4000) ? 2 : 1;
    unsigned src_control = (src_high >> 12) & 0x3;
    unsigned dst_control = (dst_high >> 12) & 0x3;
    uint32_t src = bbk9288s_hsdma_address(src_low, src_high);
    uint32_t dst = bbk9288s_hsdma_address(dst_low, dst_high);
    uint32_t units;
    uint8_t buf[2];
    BBK9288SIVRAMStats src_ivram = { 0 };
    BBK9288SIVRAMStats dst_ivram = { 0 };
    bool log_transfer;
    uint32_t i;

    s->io_regs[base + 0x0e] &= ~1u;
    if (!dual) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-hsdma: ch%u single-address mode is stubbed; "
                      "complete only ctrl=0x%04x count=0x%04x\n",
                      channel, ctrl, count_reg);
        bbk9288s_hsdma_finish_channel(s, channel, base, ctrl);
        return;
    }

    if (mode == 1) {
        units = ((uint32_t)(ctrl & 0xff) << 16) | count_reg;
    } else if (mode == 0) {
        units = 1;
    } else if (mode == 2) {
        units = count_reg & 0xff;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-hsdma: ch%u reserved mode %u; complete only\n",
                      channel, mode);
        bbk9288s_hsdma_finish_channel(s, channel, base, ctrl);
        return;
    }

    if (units == 0 || units > (1 << 20)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-hsdma: ch%u unsupported unit count %u; "
                      "complete only\n",
                      channel, units);
        bbk9288s_hsdma_finish_channel(s, channel, base, ctrl);
        return;
    }

    s->hsdma_transfer_count[channel]++;
    log_transfer = s->trace_io || s->hsdma_transfer_count[channel] <= 16 ||
                   (s->hsdma_transfer_count[channel] & 0xfff) == 0;
    if (log_transfer) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-hsdma: ch%u transfer=%" PRIu64 " dual "
                      "mode=%u units=%u unit=%u src=0x%08x dst=0x%08x "
                      "sctl=%u dctl=%u\n",
                      channel, s->hsdma_transfer_count[channel], mode, units,
                      unit_size, src, dst, src_control, dst_control);
    }
    for (i = 0; i < units; i++) {
        cpu_physical_memory_read(src, buf, unit_size);
        if (log_transfer) {
            bbk9288s_ivram_stats_add(&src_ivram, src, buf, unit_size);
            bbk9288s_ivram_stats_add(&dst_ivram, dst, buf, unit_size);
        }
        cpu_physical_memory_write(dst, buf, unit_size);
        src = bbk9288s_hsdma_next_address(src, src_control, unit_size);
        dst = bbk9288s_hsdma_next_address(dst, dst_control, unit_size);
    }

    if (log_transfer) {
        bbk9288s_hsdma_log_ivram_stats(channel, "src", &src_ivram);
        bbk9288s_hsdma_log_ivram_stats(channel, "dst", &dst_ivram);
    }
    bbk9288s_hsdma_finish_channel(s, channel, base, ctrl);
}

static bool bbk9288s_hsdma_uses_software_trigger(BBK9288SState *s,
                                                 unsigned channel)
{
    uint8_t setup = channel < 2 ? s->io_regs[BBK9288S_HSDMA_TRIG01] :
                                  s->io_regs[BBK9288S_HSDMA_TRIG23];
    unsigned shift = (channel & 1) ? 4 : 0;

    return ((setup >> shift) & 0xf) == 0;
}

static void bbk9288s_hsdma_software_trigger(BBK9288SState *s, uint8_t value)
{
    unsigned channel;

    for (channel = 0; channel < BBK9288S_HSDMA_CHANNELS; channel++) {
        hwaddr base = bbk9288s_hsdma_channel_base(channel);

        if ((value & (1u << channel)) == 0) {
            continue;
        }
        if (!bbk9288s_hsdma_uses_software_trigger(s, channel)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bbk9288s-hsdma: ch%u software trigger ignored; "
                          "trigger setup is 0x%02x/0x%02x\n",
                          channel, s->io_regs[BBK9288S_HSDMA_TRIG01],
                          s->io_regs[BBK9288S_HSDMA_TRIG23]);
            continue;
        }

        s->io_regs[base + 0x0e] |= 1;
        if (s->io_regs[base + 0x0c] & 1) {
            bbk9288s_hsdma_run_channel(s, channel);
        }
    }
}

static uint8_t bbk9288s_hsdma_read_byte(BBK9288SState *s, hwaddr offset)
{
    if (offset == BBK9288S_HSDMA_SW_TRIG) {
        return 0;
    }
    if (!bbk9288s_is_hsdma_reg(offset)) {
        return s->io_regs[offset];
    }

    switch (offset & 0xf) {
    case 0x0c:
    case 0x0e:
        return s->io_regs[offset] & 1;
    default:
        return s->io_regs[offset];
    }
}

static bool bbk9288s_hsdma_write_byte(BBK9288SState *s, hwaddr offset,
                                      uint8_t value)
{
    if (offset == BBK9288S_HSDMA_SW_TRIG) {
        bbk9288s_hsdma_software_trigger(s, value & 0x0f);
        s->io_regs[offset] = 0;
        return true;
    }
    if (!bbk9288s_is_hsdma_reg(offset)) {
        return false;
    }

    switch (offset & 0xf) {
    case 0x0c:
        s->io_regs[offset] = value & 1;
        break;
    case 0x0e:
        if (value & 1) {
            s->io_regs[offset] &= ~1u;
        }
        break;
    default:
        s->io_regs[offset] = value;
        break;
    }
    return true;
}

static bool bbk9288s_timer16_clock_ctrl_channel(hwaddr offset,
                                                unsigned *channel)
{
    if (offset < BBK9288S_16TM_CLOCK_CTRL_BASE ||
        offset >= BBK9288S_16TM_CLOCK_CTRL_BASE + BBK9288S_16TM_CHANNELS) {
        return false;
    }
    *channel = offset - BBK9288S_16TM_CLOCK_CTRL_BASE;
    return true;
}

static bool bbk9288s_timer16_reg_channel(hwaddr offset, unsigned *channel)
{
    if (offset < BBK9288S_16TM_BASE ||
        offset >= BBK9288S_16TM_BASE +
                  BBK9288S_16TM_CHANNELS * BBK9288S_16TM_STRIDE) {
        return false;
    }
    *channel = (offset - BBK9288S_16TM_BASE) / BBK9288S_16TM_STRIDE;
    return true;
}

static bool bbk9288s_timer16_read_byte(BBK9288SState *s, hwaddr offset,
                                       uint8_t *value)
{
    const BBK9288SRegInfo *reg = bbk9288s_find_reg(offset);
    unsigned channel;
    hwaddr rel;

    if (bbk9288s_timer16_clock_ctrl_channel(offset, &channel)) {
        *value = reg != NULL ? s->io_regs[offset] & reg->mask :
                               s->io_regs[offset];
        return true;
    }
    if (!bbk9288s_timer16_reg_channel(offset, &channel)) {
        return false;
    }

    rel = (offset - BBK9288S_16TM_BASE) % BBK9288S_16TM_STRIDE;
    if (rel == 6) {
        *value = s->io_regs[offset] & BBK9288S_16TM_CTRL_READ_MASK;
    } else {
        *value = reg != NULL ? s->io_regs[offset] & reg->mask :
                               s->io_regs[offset];
    }
    return true;
}

static bool bbk9288s_timer16_write_byte(BBK9288SState *s, hwaddr offset,
                                        uint8_t value)
{
    const BBK9288SRegInfo *reg = bbk9288s_find_reg(offset);
    unsigned channel;
    hwaddr rel;
    uint8_t mask;

    if (bbk9288s_timer16_clock_ctrl_channel(offset, &channel)) {
        s->io_regs[offset] = reg != NULL ? value & reg->mask : value;
        if (channel == 0) {
            bbk9288s_timer16_update(s);
        } else if (channel == 2) {
            bbk9288s_timer2b_update(s);
        }
        return true;
    }
    if (!bbk9288s_timer16_reg_channel(offset, &channel)) {
        return false;
    }

    rel = (offset - BBK9288S_16TM_BASE) % BBK9288S_16TM_STRIDE;
    if (rel == 4 || rel == 5) {
        return true;
    }
    if (rel == 6) {
        mask = reg != NULL ? reg->mask : 0x7f;
        if ((value & BBK9288S_16TM_CTRL_PRESET) != 0) {
            bbk9288s_timer16_stw(s, BBK9288S_16TM_COUNTER(channel), 0);
            if (s->trace_io || s->trace_key_scan) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "bbk9288s-timer: 16tm%u preset counter=0 "
                              "ctrl-write=0x%02x\n", channel, value);
            }
        }
        s->io_regs[offset] =
            (value & mask) & ~BBK9288S_16TM_CTRL_PRESET;
        if (channel == 0) {
            bbk9288s_timer16_update(s);
        } else if (channel == 2) {
            bbk9288s_timer2b_update(s);
        }
        return true;
    }

    s->io_regs[offset] = reg != NULL ? value & reg->mask : value;
    if (channel == 0) {
        bbk9288s_timer16_update(s);
    } else if (channel == 2) {
        bbk9288s_timer2b_update(s);
    }
    return true;
}

static uint8_t bbk9288s_io_read_byte(BBK9288SState *s, hwaddr offset)
{
    const BBK9288SRegInfo *reg = bbk9288s_find_reg(offset);
    uint8_t value = s->io_regs[offset];
    hwaddr ctrl_offset;
    uint8_t gpio_mask;
    unsigned gpio_port;

    if (bbk9288s_is_adc_reg(offset)) {
        return bbk9288s_adc_read_byte(s, offset);
    }
    if (offset == BBK9288S_HSDMA_SW_TRIG || bbk9288s_is_hsdma_reg(offset)) {
        return bbk9288s_hsdma_read_byte(s, offset);
    }
    if (bbk9288s_timer16_read_byte(s, offset, &value)) {
        return value;
    }
    if (bbk9288s_is_clock_timer_reg(offset)) {
        return bbk9288s_clock_timer_read_byte(s, offset);
    }
    if (offset == BBK9288S_K5_DATA || offset == BBK9288S_K6_DATA) {
        value = bbk9288s_k_input_data_read(s, offset);
        bbk9288s_key_scan_trace(s, false, offset, value);
        return value;
    }
    if (bbk9288s_gpio_data_info(offset, &ctrl_offset, &gpio_mask,
                                &gpio_port)) {
        value = bbk9288s_gpio_data_read(s, offset, ctrl_offset, gpio_mask,
                                        gpio_port);
        bbk9288s_key_scan_trace(s, false, offset, value);
        return value;
    }
    return reg != NULL ? value & reg->mask : value;
}

static void bbk9288s_io_write_byte(BBK9288SState *s, hwaddr offset,
                                   uint8_t value)
{
    const BBK9288SRegInfo *reg = bbk9288s_find_reg(offset);

    if (bbk9288s_adc_write_byte(s, offset, value)) {
        return;
    }
    if (bbk9288s_hsdma_write_byte(s, offset, value)) {
        return;
    }
    if (bbk9288s_timer16_write_byte(s, offset, value)) {
        return;
    }
    if (bbk9288s_clock_timer_write_byte(s, offset, value)) {
        return;
    }
    if (offset == BBK9288S_K5_DATA || offset == BBK9288S_K6_DATA) {
        bbk9288s_key_scan_trace(s, true, offset,
                                bbk9288s_k_input_data_read(s, offset));
        return;
    }
    if (reg == NULL) {
        s->io_regs[offset] = value;
        bbk9288s_key_scan_trace(s, true, offset, s->io_regs[offset]);
        return;
    }

    if (reg->flags & BBK9288S_REG_RESET_ONE_TO_CLEAR) {
        s->io_regs[offset] &= ~(value & reg->mask);
    } else {
        s->io_regs[offset] = value & reg->mask;
    }
    bbk9288s_key_scan_trace(s, true, offset, s->io_regs[offset]);
}

static uint64_t bbk9288s_io_load_masked(BBK9288SState *s, hwaddr offset,
                                        unsigned size)
{
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        value |= (uint64_t)bbk9288s_io_read_byte(s, offset + i) << (i * 8);
    }

    return value;
}

static void bbk9288s_io_store_masked(BBK9288SState *s, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    unsigned i;

    for (i = 0; i < size; i++) {
        bbk9288s_io_write_byte(s, offset + i, value >> (i * 8));
    }

    if (bbk9288s_range_touches(offset, size, BBK9288S_TTBR_OFFSET, 4)) {
        bbk9288s_set_ttbr(s, bbk9288s_ttbr_from_regs(s));
    }

    if (bbk9288s_range_touches(offset, size,
                               BBK9288S_PORT_INT_SELECT2, 1) ||
        bbk9288s_range_touches(offset, size,
                               BBK9288S_PORT_INT_POLARITY, 1) ||
        bbk9288s_range_touches(offset, size,
                               BBK9288S_PORT_INT_EDGE_LEVEL, 1) ||
        bbk9288s_range_touches(offset, size,
                               BBK9288S_EIR7_PORT4_7_CTM_ADC, 1) ||
        bbk9288s_range_touches(offset, size,
                               BBK9288S_FIR7_PORT4_7_CTM_ADC, 1)) {
        bbk9288s_port_input_update(s, "port-input-config", false, true);
    }
    if (bbk9288s_range_touches(offset, size, BBK9288S_PORT1_DATA, 1)) {
        bbk9288s_port_input_update(s, "gpio-strobe", true, true);
    }
    if (bbk9288s_range_touches(offset, size,
                               BBK9288S_KEY_INPUT_SELECT, 1) ||
        bbk9288s_range_touches(offset, size,
                               BBK9288S_KEY_INPUT_CONDITION0, 1) ||
        bbk9288s_range_touches(offset, size,
                               BBK9288S_KEY_INPUT_CONDITION1, 1) ||
        bbk9288s_range_touches(offset, size, BBK9288S_KEY_INPUT_MASK0, 1) ||
        bbk9288s_range_touches(offset, size, BBK9288S_KEY_INPUT_MASK1, 1)) {
        bbk9288s_key_input_update(s, "key-input-config", false);
    }
    if (bbk9288s_range_touches(offset, size, BBK9288S_K5_FUNC_SELECT, 1) ||
        bbk9288s_range_touches(offset, size, BBK9288S_K6_FUNC_SELECT, 1) ||
        bbk9288s_range_touches(offset, size, BBK9288S_PORT0_DATA, 1) ||
        bbk9288s_range_touches(offset, size, BBK9288S_PORT0_IOCTRL, 1) ||
        bbk9288s_range_touches(offset, size, BBK9288S_PORT2_DATA, 1) ||
        bbk9288s_range_touches(offset, size, BBK9288S_PORT2_IOCTRL, 1)) {
        bbk9288s_key_input_update(s, "key-input-source", true);
    }

    if (offset <= 0x2a9 && offset + size > 0x260) {
        bbk9288s_update_irq(s);
    }
    if (bbk9288s_range_touches(offset, size, BBK9288S_PIR3_16TM2_3, 1) ||
        bbk9288s_range_touches(offset, size, BBK9288S_EIR3_16TM2_3, 1) ||
        bbk9288s_range_touches(offset, size, BBK9288S_FIR3_16TM2_3, 1) ||
        bbk9288s_range_touches(offset, size, BBK9288S_16TM_CTRL(3), 1)) {
        bbk9288s_timer2b_update(s);
        bbk9288s_timer3b_update(s);
    }
    if (bbk9288s_range_touches(offset, size, BBK9288S_PIR_8TM_SIF0, 1) ||
        bbk9288s_range_touches(offset, size, BBK9288S_EIR5_8TM0_3, 1) ||
        bbk9288s_range_touches(offset, size, BBK9288S_FIR5_8TM0_3, 1) ||
        bbk9288s_range_touches(offset, size, BBK9288S_8TM_CTRL_BASE,
                               BBK9288S_8TM_CHANNELS *
                               BBK9288S_8TM_STRIDE)) {
        bbk9288s_timer8_update(s);
    }
    if (bbk9288s_range_touches(offset, size, BBK9288S_CTM_RUN_CTRL,
                               BBK9288S_CTM_DAY_CMP -
                               BBK9288S_CTM_RUN_CTRL + 1) ||
        bbk9288s_range_touches(offset, size, BBK9288S_PIR_CTM, 1) ||
        bbk9288s_range_touches(offset, size,
                               BBK9288S_EIR7_PORT4_7_CTM_ADC, 1) ||
        bbk9288s_range_touches(offset, size,
                               BBK9288S_FIR7_PORT4_7_CTM_ADC, 1)) {
        bbk9288s_clock_timer_update(s);
    }
}

static void bbk9288s_io_reset(BBK9288SState *s)
{
    unsigned i;

    memset(s->io_regs, 0, sizeof(s->io_regs));
    memset(s->gpio_input_low, 0, sizeof(s->gpio_input_low));
    memset(s->port_int_input_low, 0, sizeof(s->port_int_input_low));
    s->k5_input_low = 0;
    s->k6_input_low = 0;
    memset(s->key_input_high, 0, sizeof(s->key_input_high));
    memset(s->key_input_high_valid, 0, sizeof(s->key_input_high_valid));
    s->port_input_high = 0xf0;
    s->port_input_high_valid = false;
    memset(s->keyboard_qcode_down, 0, sizeof(s->keyboard_qcode_down));
    memset(s->keyboard_line_down_count, 0,
           sizeof(s->keyboard_line_down_count));
    s->adc_touch_x = 512;
    s->adc_touch_y = 512;
    s->touch_pixel_x = BBK9288S_LCD_WIDTH / 2;
    s->touch_pixel_y = BBK9288S_LCD_HEIGHT / 2;
    s->adc_last_channel = 0;
    s->touch_down = false;
    s->ctm_subsecond = 0;
    if (s->timer16_tick_timer != NULL) {
        timer_del(s->timer16_tick_timer);
    }
    if (s->timer2b_compare_timer != NULL) {
        timer_del(s->timer2b_compare_timer);
    }
    if (s->clock_timer != NULL) {
        timer_del(s->clock_timer);
    }
    for (i = 0; i < ARRAY_SIZE(bbk9288s_regs); i++) {
        const BBK9288SRegInfo *reg = &bbk9288s_regs[i];

        s->io_regs[reg->offset] = reg->reset & reg->mask;
    }
    bbk9288s_clock_timer_init_rtc(s);
}

static void bbk9288s_board_io_reset(BBK9288SState *s)
{
    memset(s->board_regs, 0, sizeof(s->board_regs));
    memset(s->board_seen, 0, sizeof(s->board_seen));
    s->board_data_latch_reads = 0;
    s->board_data_latch_pending = false;

    /*
     * The boot self-test reads 0x01000084/85 as a big-endian 16-bit status
     * word and expects 0x0004 after its command latch sequence.
     */
    s->board_regs[BBK9288S_BOARD_STATUS_H] = 0x00;
    s->board_regs[BBK9288S_BOARD_STATUS_L] = 0x04;
}

static void bbk9288s_record_mmio(BBK9288SState *s, bool is_write,
                                 hwaddr offset, uint64_t value,
                                 unsigned size)
{
    BBK9288SMMIOTrace *trace;
    uint8_t seen_mask = is_write ? 2 : 1;
    uint32_t pc = bbk9288s_current_pc();
    bool first;

    trace = &s->mmio_trace[s->mmio_trace_pos];
    trace->seq = ++s->mmio_trace_seq;
    trace->pc = pc;
    trace->addr = BBK9288S_IO_BASE + offset;
    trace->value = value;
    trace->size = size;
    trace->is_write = is_write;
    s->mmio_trace_pos = (s->mmio_trace_pos + 1) % BBK9288S_MMIO_TRACE_LEN;

    first = (s->io_seen[offset] & seen_mask) == 0;
    if (first) {
        s->io_seen[offset] |= seen_mask;
    }
    if (first || s->trace_io) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-mmio: %s %s pc=0x%08x "
                      "addr=0x%08" HWADDR_PRIx " size=%u value=0x%0*" PRIx64
                      " reg=%s group=%s\n",
                      first ? "first" : "trace",
                      is_write ? "write" : "read", pc,
                      BBK9288S_IO_BASE + offset, size, size * 2, value,
                      bbk9288s_io_reg_name(offset),
                      bbk9288s_io_group_name(offset));
    }
}

static void bbk9288s_dump_mmio_trace(BBK9288SState *s, const char *reason)
{
    unsigned count = MIN(s->mmio_trace_seq, (uint64_t)BBK9288S_MMIO_TRACE_LEN);
    unsigned start;
    unsigned i;

    if (count == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-mmio: no accesses recorded before %s\n",
                      reason);
        return;
    }

    start = s->mmio_trace_seq > BBK9288S_MMIO_TRACE_LEN ?
            s->mmio_trace_pos : 0;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "bbk9288s-mmio: last %u accesses before %s\n",
                  count, reason);
    for (i = 0; i < count; i++) {
        unsigned idx = (start + i) % BBK9288S_MMIO_TRACE_LEN;
        BBK9288SMMIOTrace *trace = &s->mmio_trace[idx];

        qemu_log_mask(LOG_GUEST_ERROR,
                      "  #%06" PRIu64 " pc=0x%08x %c%u "
                      "addr=0x%08" HWADDR_PRIx " value=0x%0*" PRIx64
                      " reg=%s group=%s\n",
                      trace->seq, trace->pc,
                      trace->is_write ? 'W' : 'R', trace->size,
                      trace->addr, trace->size * 2, trace->value,
                      bbk9288s_io_reg_name(trace->addr - BBK9288S_IO_BASE),
                      bbk9288s_io_group_name(trace->addr - BBK9288S_IO_BASE));
    }
}

static void bbk9288s_exit_notify(Notifier *notifier, void *data)
{
    BBK9288SState *s = container_of(notifier, BBK9288SState, exit);

    (void)data;
    if (s->debug_usb_wakeup_timer != NULL) {
        timer_del(s->debug_usb_wakeup_timer);
    }
    if (s->debug_nmi_wakeup_timer != NULL) {
        timer_del(s->debug_nmi_wakeup_timer);
    }
    if (s->debug_port4_wakeup_timer != NULL) {
        timer_del(s->debug_port4_wakeup_timer);
    }
    if (s->debug_port5_wakeup_timer != NULL) {
        timer_del(s->debug_port5_wakeup_timer);
    }
    if (s->debug_lcd_dump_timer != NULL) {
        timer_del(s->debug_lcd_dump_timer);
    }
    if (s->timer3b_wakeup_timer != NULL) {
        timer_del(s->timer3b_wakeup_timer);
    }
    if (s->timer16_tick_timer != NULL) {
        timer_del(s->timer16_tick_timer);
    }
    if (s->timer2b_compare_timer != NULL) {
        timer_del(s->timer2b_compare_timer);
    }
    if (s->timer8_underflow_timer != NULL) {
        timer_del(s->timer8_underflow_timer);
    }
    if (s->clock_timer != NULL) {
        timer_del(s->clock_timer);
    }
    if (s->kbd_handler != NULL) {
        qemu_input_handler_unregister(s->kbd_handler);
        s->kbd_handler = NULL;
    }
    if (s->ptr_handler != NULL) {
        qemu_input_handler_unregister(s->ptr_handler);
        s->ptr_handler = NULL;
    }
    if (bbk9288s_active_machine == s) {
        bbk9288s_active_machine = NULL;
    }
    bbk9288s_nand_storage_save(s);
    g_free(s->nand_storage);
    s->nand_storage = NULL;
    g_free(s->nand_image_path);
    s->nand_image_path = NULL;
    g_free(s->debug_lcd_dump_path);
    s->debug_lcd_dump_path = NULL;
    bbk9288s_dump_mmio_trace(s, "qemu exit");
}

static uint64_t bbk9288s_io_read(void *opaque, hwaddr offset, unsigned size)
{
    BBK9288SState *s = opaque;
    uint64_t value;

    if (offset + size > BBK9288S_IO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-mmio: read outside io window "
                      "addr=0x%08" HWADDR_PRIx " size=%u\n",
                      BBK9288S_IO_BASE + offset, size);
        return 0;
    }

    value = bbk9288s_io_load_masked(s, offset, size);
    bbk9288s_record_mmio(s, false, offset, value, size);
    return value;
}

static void bbk9288s_io_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    BBK9288SState *s = opaque;

    if (offset + size > BBK9288S_IO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-mmio: write outside io window "
                      "addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64
                      "\n",
                      BBK9288S_IO_BASE + offset, size, value);
        return;
    }

    bbk9288s_io_store_masked(s, offset, value, size);
    bbk9288s_record_mmio(s, true, offset, value, size);
}

static const MemoryRegionOps bbk9288s_io_ops = {
    .read = bbk9288s_io_read,
    .write = bbk9288s_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void bbk9288s_touch_serial_set_miso(BBK9288SState *s, bool high)
{
    if (high) {
        s->touch_io_regs[BBK9288S_TOUCH_SERIAL] |= BBK9288S_TOUCH_MISO;
    } else {
        s->touch_io_regs[BBK9288S_TOUCH_SERIAL] &= ~BBK9288S_TOUCH_MISO;
    }
}

static void bbk9288s_touch_serial_reset(BBK9288SState *s)
{
    s->touch_serial_phase = BBK9288S_TOUCH_COMMAND;
    s->touch_serial_command = 0;
    s->touch_serial_data = 0;
    s->touch_serial_bits = 0;
    s->touch_serial_trailer_clocks = 0;
    s->touch_io_regs[BBK9288S_TOUCH_SERIAL] &=
        ~(BBK9288S_TOUCH_CLK | BBK9288S_TOUCH_MISO);
}

static uint16_t bbk9288s_touch_serial_sample(BBK9288SState *s,
                                             uint8_t command)
{
    switch (command & 0xf0) {
    case 0x90:
        return MIN(s->adc_touch_y << 2, 0x0fff);
    case 0xd0:
        return MIN(s->adc_touch_x << 2, 0x0fff);
    default:
        return 0;
    }
}

static void bbk9288s_touch_serial_clock_rise(BBK9288SState *s)
{
    uint8_t channel;

    switch (s->touch_serial_phase) {
    case BBK9288S_TOUCH_COMMAND:
        s->touch_serial_command = (s->touch_serial_command << 1) |
            ((s->io_regs[BBK9288S_PORT1_DATA] & BBK9288S_TOUCH_MOSI) != 0);
        if (++s->touch_serial_bits == 8) {
            s->touch_serial_data = bbk9288s_touch_serial_sample(
                s, s->touch_serial_command);
            s->touch_serial_phase = BBK9288S_TOUCH_DUMMY;
            s->touch_serial_bits = 0;
            bbk9288s_touch_serial_set_miso(s, false);
            channel = (s->touch_serial_command & 0xf0) == 0x90 ? 1 :
                      (s->touch_serial_command & 0xf0) == 0xd0 ? 2 : 0;
            if (s->trace_io ||
                (channel != 0 &&
                 (s->touch_serial_logged_channels & channel) == 0)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "bbk9288s-touch-adc: command=0x%02x "
                              "sample=0x%03x pen=%u pixel=%u,%u\n",
                              s->touch_serial_command, s->touch_serial_data,
                              s->touch_down, s->touch_pixel_x,
                              s->touch_pixel_y);
            }
            s->touch_serial_logged_channels |= channel;
        }
        break;
    case BBK9288S_TOUCH_DUMMY:
        s->touch_serial_phase = BBK9288S_TOUCH_DATA;
        s->touch_serial_bits = 0;
        bbk9288s_touch_serial_set_miso(s, false);
        break;
    case BBK9288S_TOUCH_DATA:
        bbk9288s_touch_serial_set_miso(
            s, (s->touch_serial_data &
                (1u << (11 - s->touch_serial_bits))) != 0);
        if (++s->touch_serial_bits == 12) {
            s->touch_serial_phase = BBK9288S_TOUCH_TRAILER;
            s->touch_serial_trailer_clocks = 0;
        }
        break;
    case BBK9288S_TOUCH_TRAILER:
        bbk9288s_touch_serial_set_miso(s, false);
        if (++s->touch_serial_trailer_clocks == 3) {
            bbk9288s_touch_serial_reset(s);
        }
        break;
    }
}

static uint64_t bbk9288s_touch_io_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    BBK9288SState *s = opaque;
    uint64_t value = 0;
    unsigned i;

    if (offset + size > BBK9288S_TOUCH_IO_SIZE) {
        return 0;
    }
    for (i = 0; i < size; i++) {
        value |= (uint64_t)s->touch_io_regs[offset + i] << (i * 8);
    }
    return value;
}

static void bbk9288s_touch_io_write_byte(BBK9288SState *s, hwaddr offset,
                                         uint8_t value)
{
    uint8_t old;

    if (offset == BBK9288S_NAND_ECC_READY) {
        if (value & 1) {
            memset(&s->touch_io_regs[BBK9288S_NAND_ECC_BASE], 0xff,
                   BBK9288S_NAND_ECC_SIZE);
        }
        s->touch_io_regs[offset] = 1;
        return;
    }
    if (offset != BBK9288S_TOUCH_SERIAL) {
        s->touch_io_regs[offset] = value;
        return;
    }

    old = s->touch_io_regs[offset];
    s->touch_io_regs[offset] =
        (value & ~BBK9288S_TOUCH_MISO) | (old & BBK9288S_TOUCH_MISO);
    if ((old & BBK9288S_TOUCH_CLK) == 0 &&
        (value & BBK9288S_TOUCH_CLK) != 0) {
        bbk9288s_touch_serial_clock_rise(s);
    }
}

static void bbk9288s_touch_io_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    BBK9288SState *s = opaque;
    unsigned i;

    if (offset + size > BBK9288S_TOUCH_IO_SIZE) {
        return;
    }
    for (i = 0; i < size; i++) {
        bbk9288s_touch_io_write_byte(s, offset + i, value >> (i * 8));
    }
}

static const MemoryRegionOps bbk9288s_touch_io_ops = {
    .read = bbk9288s_touch_io_read,
    .write = bbk9288s_touch_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void bbk9288s_nand_storage_init(BBK9288SState *s, const char *path)
{
    GError *gerr = NULL;
    char *contents = NULL;
    gsize length = 0;

    s->nand_storage_size = BBK9288S_NAND_STORAGE_SIZE;
    s->nand_storage = g_malloc(s->nand_storage_size);
    memset(s->nand_storage, 0xff, s->nand_storage_size);
    s->nand_image_path = g_strdup(path != NULL && path[0] != 0 ? path : NULL);

    if (s->nand_image_path == NULL) {
        info_report("BBK9288S NAND: volatile blank 256 MiB device");
        return;
    }
    if (!g_file_test(s->nand_image_path, G_FILE_TEST_EXISTS)) {
        info_report("BBK9288S NAND: new blank image %s (%zu raw bytes)",
                    s->nand_image_path, s->nand_storage_size);
        return;
    }
    if (!g_file_get_contents(s->nand_image_path, &contents, &length, &gerr)) {
        error_report("could not load BBK9288S NAND image '%s': %s",
                     s->nand_image_path, gerr->message);
        g_error_free(gerr);
        exit(1);
    }
    if (length != s->nand_storage_size) {
        error_report("BBK9288S NAND image '%s' has %zu bytes; expected %zu",
                     s->nand_image_path, (size_t)length,
                     s->nand_storage_size);
        g_free(contents);
        exit(1);
    }

    memcpy(s->nand_storage, contents, length);
    g_free(contents);
    info_report("BBK9288S NAND: loaded %s (%zu raw bytes)",
                s->nand_image_path, s->nand_storage_size);
}

static void bbk9288s_nand_storage_save(BBK9288SState *s)
{
    GError *gerr = NULL;

    if (!s->nand_dirty || s->nand_image_path == NULL) {
        return;
    }
    if (!g_file_set_contents(s->nand_image_path,
                             (const char *)s->nand_storage,
                             s->nand_storage_size, &gerr)) {
        error_report("could not save BBK9288S NAND image '%s': %s",
                     s->nand_image_path, gerr->message);
        g_error_free(gerr);
        return;
    }

    s->nand_dirty = false;
    info_report("BBK9288S NAND: saved %s (%zu raw bytes)",
                s->nand_image_path, s->nand_storage_size);
}

typedef struct BBK9288SFatBoot {
    BBK9288SState *machine;
    uint16_t *block_map;
    uint64_t fat_offset;
    uint64_t root_offset;
    uint64_t data_offset;
    uint32_t root_entries;
    uint32_t cluster_size;
    uint32_t cluster_count;
} BBK9288SFatBoot;

static unsigned bbk9288s_nand_tag_parity(unsigned value)
{
    unsigned parity = 0;

    while (value != 0) {
        parity ^= value & 1;
        value >>= 1;
    }
    return parity;
}

static bool bbk9288s_nand_block_tag(BBK9288SState *s,
                                    unsigned physical_block,
                                    uint16_t *result)
{
    uint16_t tags[8];
    unsigned tag_count = 0;
    unsigned best_count = 0;
    unsigned best = 0;
    unsigned page;
    unsigned sector;
    unsigned i;
    unsigned j;

    for (page = 0; page < 2; page++) {
        size_t offset = (size_t)physical_block *
                        BBK9288S_NAND_RAW_BLOCK_SIZE +
                        page * BBK9288S_NAND_RAW_PAGE_SIZE +
                        BBK9288S_NAND_PAGE_SIZE;
        const uint8_t *oob = s->nand_storage + offset;

        for (sector = 0; sector < 4; sector++) {
            const uint8_t *slot = oob + sector * 16;
            uint16_t first = lduw_le_p(slot + 6);
            uint16_t second = lduw_le_p(slot + 11);

            if (slot[1] == 0 && first != UINT16_MAX && first == second) {
                tags[tag_count++] = first;
            }
        }
    }

    for (i = 0; i < tag_count; i++) {
        unsigned count = 0;

        for (j = 0; j < tag_count; j++) {
            count += tags[j] == tags[i];
        }
        if (count > best_count) {
            best = i;
            best_count = count;
        }
    }
    if (best_count == 0) {
        return false;
    }

    *result = tags[best];
    return true;
}

static uint16_t *bbk9288s_nand_build_block_map(BBK9288SState *s)
{
    uint16_t *map = g_new(uint16_t, BBK9288S_NAND_LOGICAL_BLOCKS);
    unsigned physical;
    unsigned logical;

    for (logical = 0; logical < BBK9288S_NAND_LOGICAL_BLOCKS; logical++) {
        map[logical] = UINT16_MAX;
    }

    for (physical = BBK9288S_NAND_RESERVED_BLOCKS;
         physical < BBK9288S_NAND_BLOCKS; physical++) {
        uint16_t tag;

        if (!bbk9288s_nand_block_tag(s, physical, &tag)) {
            continue;
        }
        logical = tag >> 1;
        if (logical >= BBK9288S_NAND_LOGICAL_BLOCKS ||
            (tag & 1) != bbk9288s_nand_tag_parity(logical)) {
            continue;
        }

        /*
         * The FTL appends replacement blocks. Scanning in physical order
         * therefore selects the newest copy when a logical block is repeated.
         */
        map[logical] = physical;
    }
    return map;
}

static bool bbk9288s_nand_read_logical(BBK9288SState *s,
                                       const uint16_t *map,
                                       uint64_t offset,
                                       void *buffer,
                                       size_t length)
{
    uint8_t *out = buffer;
    uint64_t flat_size = (uint64_t)BBK9288S_NAND_LOGICAL_BLOCKS *
                         BBK9288S_NAND_BLOCK_DATA_SIZE;

    if (offset > flat_size || length > flat_size - offset) {
        return false;
    }

    while (length != 0) {
        unsigned logical = offset / BBK9288S_NAND_BLOCK_DATA_SIZE;
        size_t block_offset = offset % BBK9288S_NAND_BLOCK_DATA_SIZE;
        unsigned page = block_offset / BBK9288S_NAND_PAGE_SIZE;
        size_t page_offset = block_offset % BBK9288S_NAND_PAGE_SIZE;
        size_t chunk = MIN(length,
                           (size_t)BBK9288S_NAND_PAGE_SIZE - page_offset);
        uint16_t physical = map[logical];
        size_t raw_offset;

        if (physical == UINT16_MAX) {
            return false;
        }
        raw_offset = (size_t)physical * BBK9288S_NAND_RAW_BLOCK_SIZE +
                     page * BBK9288S_NAND_RAW_PAGE_SIZE + page_offset;
        memcpy(out, s->nand_storage + raw_offset, chunk);
        out += chunk;
        offset += chunk;
        length -= chunk;
    }
    return true;
}

static bool bbk9288s_fat_boot_init(BBK9288SFatBoot *fat,
                                   BBK9288SState *s,
                                   uint16_t *map)
{
    uint8_t mbr[BBK9288S_FAT_SECTOR_SIZE];
    uint8_t boot[BBK9288S_FAT_SECTOR_SIZE];
    uint32_t partition_lba;
    uint32_t total_sectors;
    uint32_t reserved_sectors;
    uint32_t fat_sectors;
    uint32_t root_sectors;
    uint32_t data_sectors;
    uint8_t sectors_per_cluster;
    uint8_t number_of_fats;

    if (!bbk9288s_nand_read_logical(s, map, 0, mbr, sizeof(mbr)) ||
        lduw_le_p(mbr + 510) != 0xaa55) {
        return false;
    }
    partition_lba = ldl_le_p(mbr + 446 + 8);
    if (!bbk9288s_nand_read_logical(
            s, map, (uint64_t)partition_lba * BBK9288S_FAT_SECTOR_SIZE,
            boot, sizeof(boot)) ||
        lduw_le_p(boot + 510) != 0xaa55 ||
        lduw_le_p(boot + 11) != BBK9288S_FAT_SECTOR_SIZE) {
        return false;
    }

    sectors_per_cluster = boot[13];
    reserved_sectors = lduw_le_p(boot + 14);
    number_of_fats = boot[16];
    fat->root_entries = lduw_le_p(boot + 17);
    total_sectors = lduw_le_p(boot + 19);
    if (total_sectors == 0) {
        total_sectors = ldl_le_p(boot + 32);
    }
    fat_sectors = lduw_le_p(boot + 22);
    if (sectors_per_cluster == 0 || reserved_sectors == 0 ||
        number_of_fats == 0 || fat->root_entries == 0 ||
        total_sectors == 0 || fat_sectors == 0) {
        return false;
    }

    root_sectors =
        (fat->root_entries * 32 + BBK9288S_FAT_SECTOR_SIZE - 1) /
        BBK9288S_FAT_SECTOR_SIZE;
    if (total_sectors <= reserved_sectors +
                         number_of_fats * fat_sectors + root_sectors) {
        return false;
    }
    data_sectors = total_sectors - reserved_sectors -
                   number_of_fats * fat_sectors - root_sectors;

    fat->machine = s;
    fat->block_map = map;
    fat->fat_offset =
        ((uint64_t)partition_lba + reserved_sectors) *
        BBK9288S_FAT_SECTOR_SIZE;
    fat->root_offset =
        fat->fat_offset +
        (uint64_t)number_of_fats * fat_sectors * BBK9288S_FAT_SECTOR_SIZE;
    fat->data_offset =
        fat->root_offset + (uint64_t)root_sectors * BBK9288S_FAT_SECTOR_SIZE;
    fat->cluster_size =
        sectors_per_cluster * BBK9288S_FAT_SECTOR_SIZE;
    fat->cluster_count = data_sectors / sectors_per_cluster;
    return fat->cluster_count != 0;
}

static uint16_t bbk9288s_fat_next_cluster(BBK9288SFatBoot *fat,
                                          uint16_t cluster)
{
    uint8_t entry[2];

    if (!bbk9288s_nand_read_logical(
            fat->machine, fat->block_map,
            fat->fat_offset + (uint64_t)cluster * sizeof(entry),
            entry, sizeof(entry))) {
        return UINT16_MAX;
    }
    return lduw_le_p(entry);
}

static bool bbk9288s_fat_kernel_name(const uint8_t *entry)
{
    static const char short_name[11] = "KERNEL  BIN";
    unsigned i;

    for (i = 0; i < sizeof(short_name); i++) {
        if (g_ascii_toupper(entry[i]) != short_name[i]) {
            return false;
        }
    }
    return true;
}

static bool bbk9288s_fat_find_kernel(BBK9288SFatBoot *fat,
                                     bool root,
                                     uint16_t first_cluster,
                                     unsigned depth,
                                     uint16_t *kernel_cluster,
                                     uint32_t *kernel_size)
{
    uint16_t cluster = first_cluster;
    uint32_t chain_length = 0;

    if (depth > BBK9288S_FAT_MAX_DEPTH) {
        return false;
    }

    do {
        uint64_t offset;
        uint32_t entries;
        uint32_t i;

        if (root) {
            offset = fat->root_offset;
            entries = fat->root_entries;
        } else {
            if (cluster < 2 ||
                cluster >= fat->cluster_count + 2 ||
                chain_length++ >= fat->cluster_count) {
                return false;
            }
            offset = fat->data_offset +
                     (uint64_t)(cluster - 2) * fat->cluster_size;
            entries = fat->cluster_size / 32;
        }

        for (i = 0; i < entries; i++) {
            uint8_t entry[32];
            uint8_t attributes;
            uint16_t entry_cluster;

            if (!bbk9288s_nand_read_logical(
                    fat->machine, fat->block_map, offset + i * sizeof(entry),
                    entry, sizeof(entry))) {
                return false;
            }
            if (entry[0] == 0) {
                return false;
            }
            if (entry[0] == 0xe5) {
                continue;
            }

            attributes = entry[11];
            if (attributes == 0x0f || (attributes & 0x08) != 0) {
                continue;
            }
            entry_cluster = lduw_le_p(entry + 26);

            if ((attributes & 0x10) == 0 &&
                bbk9288s_fat_kernel_name(entry)) {
                *kernel_cluster = entry_cluster;
                *kernel_size = ldl_le_p(entry + 28);
                return *kernel_cluster >= 2 && *kernel_size != 0;
            }
            if ((attributes & 0x10) != 0 && entry[0] != '.' &&
                bbk9288s_fat_find_kernel(
                    fat, false, entry_cluster, depth + 1,
                    kernel_cluster, kernel_size)) {
                return true;
            }
        }

        if (root) {
            break;
        }
        cluster = bbk9288s_fat_next_cluster(fat, cluster);
    } while (cluster < BBK9288S_FAT_CLUSTER_END);

    return false;
}

static uint8_t *bbk9288s_fat_read_file(BBK9288SFatBoot *fat,
                                       uint16_t cluster,
                                       uint32_t size)
{
    uint8_t *data = g_malloc(size);
    uint8_t *out = data;
    uint32_t remaining = size;
    uint32_t chain_length = 0;

    while (remaining != 0) {
        size_t chunk;
        uint64_t offset;

        if (cluster < 2 || cluster >= fat->cluster_count + 2 ||
            chain_length++ >= fat->cluster_count) {
            g_free(data);
            return NULL;
        }
        chunk = MIN(remaining, fat->cluster_size);
        offset = fat->data_offset +
                 (uint64_t)(cluster - 2) * fat->cluster_size;
        if (!bbk9288s_nand_read_logical(
                fat->machine, fat->block_map, offset, out, chunk)) {
            g_free(data);
            return NULL;
        }
        out += chunk;
        remaining -= chunk;
        if (remaining != 0) {
            cluster = bbk9288s_fat_next_cluster(fat, cluster);
            if (cluster >= BBK9288S_FAT_CLUSTER_END) {
                g_free(data);
                return NULL;
            }
        }
    }
    return data;
}

static uint8_t *bbk9288s_nand_extract_kernel(BBK9288SState *s,
                                             gsize *length,
                                             uint64_t ram_size)
{
    g_autofree uint16_t *map = NULL;
    BBK9288SFatBoot fat = { 0 };
    uint16_t cluster;
    uint32_t size;
    uint8_t *data;

    if (s->nand_image_path == NULL) {
        return NULL;
    }
    map = bbk9288s_nand_build_block_map(s);
    if (!bbk9288s_fat_boot_init(&fat, s, map) ||
        !bbk9288s_fat_find_kernel(
            &fat, true, 0, 0, &cluster, &size)) {
        return NULL;
    }
    if (size < BBK9288S_KERNEL_HEADER ||
        size - BBK9288S_KERNEL_HEADER > ram_size) {
        return NULL;
    }

    data = bbk9288s_fat_read_file(&fat, cluster, size);
    if (data == NULL) {
        return NULL;
    }
    *length = size;
    info_report("BBK9288S boot: loaded kernel.bin from NAND FAT16 "
                "(cluster=%u size=%u)", cluster, size);
    return data;
}

static void bbk9288s_touch_io_reset(BBK9288SState *s)
{
    memset(s->touch_io_regs, 0, sizeof(s->touch_io_regs));
    s->touch_io_regs[BBK9288S_NAND_CE_SELECT] = 0x80;
    s->touch_io_regs[BBK9288S_NAND_ECC_READY] = 1;
    memset(&s->touch_io_regs[BBK9288S_NAND_ECC_BASE], 0xff,
           BBK9288S_NAND_ECC_SIZE);
    s->touch_serial_logged_channels = 0;
    bbk9288s_touch_serial_reset(s);
    bbk9288s_nand_reset(s);
}

static void bbk9288s_nand_reset(BBK9288SState *s)
{
    s->nand_phase = BBK9288S_NAND_IDLE;
    s->nand_command = 0xff;
    s->nand_address_count = 0;
    s->nand_id_index = 0;
    s->nand_page = 0;
    s->nand_column = 0;
    memset(s->nand_page_cache, 0xff, sizeof(s->nand_page_cache));
    s->nand_program_address_cycles = 0;
    s->nand_page_cache_valid = false;
    s->nand_program_page_valid = false;
    s->nand_copyback_program = false;
    s->nand_data_reads = 0;
    s->nand_data_writes = 0;
}

static void bbk9288s_nand_decode_page_address(BBK9288SState *s)
{
    s->nand_column = s->nand_address[0] |
                     ((uint16_t)s->nand_address[1] << 8);
    s->nand_page = s->nand_address[2] |
                   ((uint32_t)s->nand_address[3] << 8) |
                   ((uint32_t)s->nand_address[4] << 16);
}

static bool bbk9288s_nand_log_sample(BBK9288SState *s, uint64_t count)
{
    return s->trace_io || count <= 16 || (count & 0xfff) == 0;
}

static void bbk9288s_nand_load_page_cache(BBK9288SState *s)
{
    if (s->nand_page < BBK9288S_NAND_PAGE_COUNT) {
        size_t offset = (size_t)s->nand_page *
                        BBK9288S_NAND_RAW_PAGE_SIZE;

        memcpy(s->nand_page_cache, s->nand_storage + offset,
               sizeof(s->nand_page_cache));
    } else {
        memset(s->nand_page_cache, 0xff, sizeof(s->nand_page_cache));
    }
    s->nand_page_cache_valid = true;
}

static void bbk9288s_nand_commit_page_cache(BBK9288SState *s)
{
    size_t offset;
    unsigned i;

    if (!s->nand_page_cache_valid || !s->nand_program_page_valid ||
        s->nand_page >= BBK9288S_NAND_PAGE_COUNT) {
        return;
    }

    offset = (size_t)s->nand_page * BBK9288S_NAND_RAW_PAGE_SIZE;
    for (i = 0; i < BBK9288S_NAND_RAW_PAGE_SIZE; i++) {
        uint8_t programmed = s->nand_storage[offset + i] &
                             s->nand_page_cache[i];

        if (programmed != s->nand_storage[offset + i]) {
            s->nand_storage[offset + i] = programmed;
            s->nand_dirty = true;
        }
    }
}

static void bbk9288s_nand_erase_block(BBK9288SState *s)
{
    uint32_t page;
    uint32_t first_page;
    size_t offset;
    size_t length;

    if (s->nand_address_count < 3) {
        return;
    }

    page = s->nand_address[0] |
           ((uint32_t)s->nand_address[1] << 8) |
           ((uint32_t)s->nand_address[2] << 16);
    first_page = page & ~(BBK9288S_NAND_PAGES_PER_BLOCK - 1);
    if (first_page >= BBK9288S_NAND_PAGE_COUNT) {
        return;
    }

    offset = (size_t)first_page * BBK9288S_NAND_RAW_PAGE_SIZE;
    length = BBK9288S_NAND_PAGES_PER_BLOCK * BBK9288S_NAND_RAW_PAGE_SIZE;
    memset(s->nand_storage + offset, 0xff, length);
    s->nand_dirty = true;
    s->nand_erase_count++;
    if (bbk9288s_nand_log_sample(s, s->nand_erase_count)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-nand: erase=%" PRIu64 " block=%u "
                      "first-page=%u\n",
                      s->nand_erase_count,
                      first_page / BBK9288S_NAND_PAGES_PER_BLOCK,
                      first_page);
    }
}

static void bbk9288s_nand_command(BBK9288SState *s, uint8_t command)
{
    s->nand_command = command;
    s->nand_command_count++;
    if (s->trace_io || s->nand_command_count <= 24 ||
        command == 0xff || command == 0x90) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-nand: command=%" PRIu64 " value=0x%02x "
                      "phase=%u addresses=%u\n",
                      s->nand_command_count, command, s->nand_phase,
                      s->nand_address_count);
    }

    switch (command) {
    case 0xff:
        bbk9288s_nand_reset(s);
        break;
    case 0x90:
        s->nand_phase = BBK9288S_NAND_READ_ID;
        s->nand_address_count = 0;
        s->nand_id_index = 0;
        break;
    case 0x70:
        s->nand_phase = BBK9288S_NAND_READ_STATUS;
        break;
    case 0x00:
        s->nand_phase = BBK9288S_NAND_READ_SETUP;
        s->nand_address_count = 0;
        s->nand_data_reads = 0;
        break;
    case 0x30:
        if (s->nand_phase == BBK9288S_NAND_READ_SETUP &&
            s->nand_address_count >= 5) {
            bbk9288s_nand_decode_page_address(s);
            s->nand_phase = BBK9288S_NAND_READ_DATA;
            s->nand_page_read_count++;
            if (bbk9288s_nand_log_sample(s, s->nand_page_read_count)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "bbk9288s-nand: read=%" PRIu64 " page=%u "
                              "column=%u\n",
                              s->nand_page_read_count, s->nand_page,
                              s->nand_column);
            }
        }
        break;
    case 0x35:
        if (s->nand_phase == BBK9288S_NAND_READ_SETUP &&
            s->nand_address_count >= 5) {
            bbk9288s_nand_decode_page_address(s);
            bbk9288s_nand_load_page_cache(s);
            s->nand_phase = BBK9288S_NAND_COPYBACK_READY;
            s->nand_page_read_count++;
            if (bbk9288s_nand_log_sample(s, s->nand_page_read_count)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "bbk9288s-nand: copyback-read=%" PRIu64
                              " page=%u\n",
                              s->nand_page_read_count, s->nand_page);
            }
        }
        break;
    case 0x60:
        s->nand_phase = BBK9288S_NAND_ERASE_SETUP;
        s->nand_address_count = 0;
        break;
    case 0xd0:
        if (s->nand_phase == BBK9288S_NAND_ERASE_SETUP) {
            bbk9288s_nand_erase_block(s);
            s->nand_phase = BBK9288S_NAND_READ_STATUS;
        }
        break;
    case 0x80:
        s->nand_phase = BBK9288S_NAND_PROGRAM_DATA;
        s->nand_address_count = 0;
        s->nand_data_writes = 0;
        memset(s->nand_page_cache, 0xff, sizeof(s->nand_page_cache));
        s->nand_page_cache_valid = true;
        s->nand_program_page_valid = false;
        s->nand_program_address_cycles = 5;
        s->nand_copyback_program = false;
        break;
    case 0x85:
        if (s->nand_phase == BBK9288S_NAND_COPYBACK_READY &&
            s->nand_page_cache_valid) {
            s->nand_phase = BBK9288S_NAND_PROGRAM_DATA;
            s->nand_address_count = 0;
            s->nand_data_writes = 0;
            s->nand_program_page_valid = false;
            s->nand_program_address_cycles = 5;
            s->nand_copyback_program = true;
        } else if (s->nand_phase == BBK9288S_NAND_PROGRAM_DATA &&
                   s->nand_page_cache_valid &&
                   s->nand_program_page_valid) {
            /* Random data input changes the column in the page register. */
            s->nand_address_count = 0;
            s->nand_program_address_cycles = 2;
        }
        break;
    case 0x10:
        if (s->nand_phase == BBK9288S_NAND_PROGRAM_DATA) {
            bbk9288s_nand_commit_page_cache(s);
            s->nand_program_count++;
            if (bbk9288s_nand_log_sample(s, s->nand_program_count)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "bbk9288s-nand: program=%" PRIu64 " page=%u "
                              "bytes=%u copyback=%u\n",
                              s->nand_program_count, s->nand_page,
                              s->nand_data_writes,
                              s->nand_copyback_program ? 1 : 0);
            }
        }
        s->nand_phase = BBK9288S_NAND_READ_STATUS;
        s->nand_page_cache_valid = false;
        s->nand_program_page_valid = false;
        s->nand_program_address_cycles = 0;
        s->nand_copyback_program = false;
        break;
    default:
        break;
    }
}

static void bbk9288s_nand_address(BBK9288SState *s, uint8_t value)
{
    if (s->nand_address_count < ARRAY_SIZE(s->nand_address)) {
        s->nand_address[s->nand_address_count++] = value;
    }
    if (s->nand_phase == BBK9288S_NAND_READ_ID) {
        s->nand_id_index = 0;
    } else if (s->nand_phase == BBK9288S_NAND_PROGRAM_DATA) {
        if (s->nand_program_address_cycles == 5 &&
            s->nand_address_count == 5) {
            bbk9288s_nand_decode_page_address(s);
            s->nand_program_page_valid = true;
        } else if (s->nand_program_address_cycles == 2 &&
                   s->nand_address_count == 2) {
            s->nand_column = s->nand_address[0] |
                             ((uint16_t)s->nand_address[1] << 8);
        }
    }
}

static uint8_t bbk9288s_nand_data_read_byte(BBK9288SState *s)
{
    static const uint8_t id[] = {
        BBK9288S_NAND_MFR_ID, BBK9288S_NAND_DEVICE_ID, 0x10, 0x95,
    };

    switch (s->nand_phase) {
    case BBK9288S_NAND_READ_ID:
        if (s->nand_id_index < ARRAY_SIZE(id)) {
            return id[s->nand_id_index++];
        }
        return 0xff;
    case BBK9288S_NAND_READ_STATUS:
        return 0xc0;
    case BBK9288S_NAND_READ_DATA: {
        uint8_t value = 0xff;

        if (s->nand_page < BBK9288S_NAND_PAGE_COUNT &&
            s->nand_column < BBK9288S_NAND_RAW_PAGE_SIZE) {
            size_t offset = (size_t)s->nand_page *
                            BBK9288S_NAND_RAW_PAGE_SIZE + s->nand_column;

            value = s->nand_storage[offset];
        }
        s->nand_data_reads++;
        s->nand_column++;
        return value;
    }
    default:
        return 0xff;
    }
}

static void bbk9288s_nand_data_write_byte(BBK9288SState *s, uint8_t value)
{
    if (s->nand_phase != BBK9288S_NAND_PROGRAM_DATA ||
        !s->nand_page_cache_valid || !s->nand_program_page_valid ||
        s->nand_address_count < s->nand_program_address_cycles) {
        return;
    }
    if (s->nand_page >= BBK9288S_NAND_PAGE_COUNT ||
        s->nand_column >= BBK9288S_NAND_RAW_PAGE_SIZE) {
        s->nand_column++;
        return;
    }

    s->nand_page_cache[s->nand_column] = value;
    s->nand_data_writes++;
    s->nand_column++;
}

static uint64_t bbk9288s_nand_io_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    BBK9288SState *s = opaque;
    uint64_t value = 0;
    unsigned i;

    if (s->touch_io_regs[0x22] & 0x02) {
        return MAKE_64BIT_MASK(0, size * 8);
    }
    for (i = 0; i < size; i++) {
        value |= (uint64_t)bbk9288s_nand_data_read_byte(s) << (i * 8);
    }
    return value;
}

static void bbk9288s_nand_io_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    BBK9288SState *s = opaque;
    uint8_t p2 = s->io_regs[BBK9288S_PORT2_DATA];
    unsigned i;

    if (s->touch_io_regs[0x22] & 0x02) {
        return;
    }
    for (i = 0; i < size; i++) {
        uint8_t byte = value >> (i * 8);

        if (p2 & BBK9288S_NAND_CLE) {
            bbk9288s_nand_command(s, byte);
        } else if (p2 & BBK9288S_NAND_ALE) {
            bbk9288s_nand_address(s, byte);
        } else {
            bbk9288s_nand_data_write_byte(s, byte);
        }
    }
}

static const MemoryRegionOps bbk9288s_nand_io_ops = {
    .read = bbk9288s_nand_io_read,
    .write = bbk9288s_nand_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void bbk9288s_board_record(BBK9288SState *s, bool is_write,
                                  hwaddr offset, uint64_t value,
                                  unsigned size)
{
    uint8_t seen_mask = is_write ? 2 : 1;
    bool first = (s->board_seen[offset] & seen_mask) == 0;

    if (!first && !s->trace_io) {
        return;
    }

    if (first) {
        s->board_seen[offset] |= seen_mask;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "bbk9288s-board: %s %s pc=0x%08x "
                  "addr=0x%08" HWADDR_PRIx " size=%u value=0x%0*" PRIx64
                  "\n",
                  first ? "first" : "trace",
                  is_write ? "write" : "read", bbk9288s_current_pc(),
                  BBK9288S_BOARD_IO_BASE + offset, size, size * 2, value);
}

static bool bbk9288s_board_is_known_reg(hwaddr offset)
{
    switch (offset) {
    case BBK9288S_BOARD_CTRL:
    case BBK9288S_BOARD_DATA:
    case BBK9288S_BOARD_STATUS_H:
    case BBK9288S_BOARD_STATUS_L:
        return true;
    default:
        return false;
    }
}

static uint8_t bbk9288s_board_read_byte(BBK9288SState *s, hwaddr offset)
{
    if (s->strict_board_io && !bbk9288s_board_is_known_reg(offset)) {
        return 0;
    }
    if (offset == BBK9288S_BOARD_DATA && s->board_data_latch_pending) {
        uint8_t phase = s->board_data_latch_reads++;
        uint8_t value =
            bbk9288s_board_latch_data[MIN(phase,
                                      ARRAY_SIZE(bbk9288s_board_latch_data) -
                                      1)];

        s->board_regs[BBK9288S_BOARD_DATA] = value;
        if (s->board_data_latch_reads >=
            ARRAY_SIZE(bbk9288s_board_latch_data)) {
            s->board_data_latch_pending = false;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-board: command latch data phase %u "
                      "pc=0x%08x value=0x%02x\n",
                      phase, bbk9288s_current_pc(), value);
        return value;
    }

    return s->board_regs[offset];
}

static void bbk9288s_board_write_byte(BBK9288SState *s, hwaddr offset,
                                      uint8_t value)
{
    uint8_t old = s->board_regs[offset];

    if (!s->strict_board_io || bbk9288s_board_is_known_reg(offset)) {
        s->board_regs[offset] = value;
    }
    if (offset == BBK9288S_BOARD_CTRL &&
        (old & 0x01) == 0 && (value & 0x01) != 0) {
        s->board_regs[BBK9288S_BOARD_DATA] = 0;
        s->board_data_latch_reads = 0;
        s->board_data_latch_pending = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-board: command latch complete pc=0x%08x "
                      "ctrl=0x%02x data=0x00\n",
                      bbk9288s_current_pc(), value);
    }
    if (offset == BBK9288S_BOARD_CTRL &&
        (old & 0x01) != 0 && (value & 0x01) == 0) {
        s->board_regs[BBK9288S_BOARD_STATUS_H] = 0;
        s->board_regs[BBK9288S_BOARD_STATUS_L] = 0;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-board: command latch status clear "
                      "pc=0x%08x ctrl=0x%02x status=0x0000\n",
                      bbk9288s_current_pc(), value);
    }
}

static uint64_t bbk9288s_board_io_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    BBK9288SState *s = opaque;
    uint64_t value = 0;
    unsigned i;

    if (offset + size > BBK9288S_BOARD_IO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-board: read outside board io "
                      "addr=0x%08" HWADDR_PRIx " size=%u\n",
                      BBK9288S_BOARD_IO_BASE + offset, size);
        return 0;
    }

    for (i = 0; i < size; i++) {
        value |= (uint64_t)bbk9288s_board_read_byte(s, offset + i) <<
                 (i * 8);
    }
    bbk9288s_board_record(s, false, offset, value, size);
    return value;
}

static void bbk9288s_board_io_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    BBK9288SState *s = opaque;
    unsigned i;

    if (offset + size > BBK9288S_BOARD_IO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-board: write outside board io "
                      "addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64
                      "\n",
                      BBK9288S_BOARD_IO_BASE + offset, size, value);
        return;
    }

    for (i = 0; i < size; i++) {
        bbk9288s_board_write_byte(s, offset + i, value >> (i * 8));
    }
    bbk9288s_board_record(s, true, offset, value, size);
}

static const MemoryRegionOps bbk9288s_board_io_ops = {
    .read = bbk9288s_board_io_read,
    .write = bbk9288s_board_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t bbk9288s_lcdc_io_load(BBK9288SState *s, hwaddr offset,
                                      unsigned size)
{
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        value |= (uint64_t)s->lcdc_regs[offset + i] << (i * 8);
    }

    return value;
}

static void bbk9288s_lcdc_io_store(BBK9288SState *s, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    unsigned i;

    for (i = 0; i < size; i++) {
        s->lcdc_regs[offset + i] = value >> (i * 8);
    }
}

static void bbk9288s_lcdc_record(BBK9288SState *s, bool is_write,
                                 hwaddr offset, uint64_t value,
                                 unsigned size)
{
    uint8_t seen_mask = is_write ? 2 : 1;
    bool first = (s->lcdc_seen[offset] & seen_mask) == 0;

    if (!first && !s->trace_io) {
        return;
    }

    if (first) {
        s->lcdc_seen[offset] |= seen_mask;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "bbk9288s-lcdc: %s %s pc=0x%08x "
                  "addr=0x%08" HWADDR_PRIx " size=%u value=0x%0*" PRIx64
                  " reg=%s\n",
                  first ? "first" : "trace",
                  is_write ? "write" : "read", bbk9288s_current_pc(),
                  BBK9288S_LCDC_IO_BASE + offset, size, size * 2, value,
                  bbk9288s_lcdc_reg_name(offset));
}

static uint64_t bbk9288s_lcdc_io_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    BBK9288SState *s = opaque;
    uint64_t value;

    if (offset + size > BBK9288S_LCDC_IO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-lcdc: read outside lcdc window "
                      "addr=0x%08" HWADDR_PRIx " size=%u\n",
                      BBK9288S_LCDC_IO_BASE + offset, size);
        return 0;
    }

    value = bbk9288s_lcdc_io_load(s, offset, size);
    bbk9288s_lcdc_record(s, false, offset, value, size);
    return value;
}

static void bbk9288s_lcdc_io_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    BBK9288SState *s = opaque;

    if (offset + size > BBK9288S_LCDC_IO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-lcdc: write outside lcdc window "
                      "addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64
                      "\n",
                      BBK9288S_LCDC_IO_BASE + offset, size, value);
        return;
    }

    bbk9288s_lcdc_io_store(s, offset, value, size);
    bbk9288s_lcdc_record(s, true, offset, value, size);
}

static const MemoryRegionOps bbk9288s_lcdc_io_ops = {
    .read = bbk9288s_lcdc_io_read,
    .write = bbk9288s_lcdc_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint8_t bbk9288s_lcd_gray_value(uint8_t pixel)
{
    static const uint8_t levels[4] = { 0x00, 0x55, 0xaa, 0xff };

    return levels[pixel & 0x03];
}

static uint8_t bbk9288s_lcd_vram_pixel(BBK9288SState *s, unsigned x,
                                       unsigned y)
{
    const uint8_t *src = s->ivram_ptr + y * BBK9288S_LCD_STRIDE;
    unsigned shift = 6 - (x & 3) * 2;

    return (src[x / 4] >> shift) & 0x03;
}

static uint32_t bbk9288s_lcd_pixel(DisplaySurface *surface, uint8_t gray)
{
    uint8_t level = bbk9288s_lcd_gray_value(gray);

    switch (surface_bits_per_pixel(surface)) {
    case 8:
        return rgb_to_pixel8(level, level, level);
    case 15:
        return rgb_to_pixel15(level, level, level);
    case 16:
        return rgb_to_pixel16(level, level, level);
    case 24:
        return rgb_to_pixel24(level, level, level);
    case 32:
        return rgb_to_pixel32(level, level, level);
    default:
        return 0;
    }
}

static void bbk9288s_lcd_store_pixel(DisplaySurface *surface, int x, int y,
                                     uint32_t color)
{
    int bpp = (surface_bits_per_pixel(surface) + 7) / 8;
    uint8_t *dst = surface_data(surface) + y * surface_stride(surface) +
                   x * bpp;

    switch (bpp) {
    case 1:
        dst[0] = color;
        break;
    case 2:
        *(uint16_t *)dst = color;
        break;
    case 3:
        dst[0] = color;
        dst[1] = color >> 8;
        dst[2] = color >> 16;
        break;
    case 4:
        *(uint32_t *)dst = color;
        break;
    }
}

static void bbk9288s_lcd_update(void *opaque)
{
    BBK9288SState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t colors[4];
    unsigned x;
    unsigned y;

    if (surface_bits_per_pixel(surface) == 0 || s->ivram_ptr == NULL) {
        return;
    }

    for (x = 0; x < ARRAY_SIZE(colors); x++) {
        colors[x] = bbk9288s_lcd_pixel(surface, x);
    }

    for (y = 0; y < BBK9288S_LCD_HEIGHT; y++) {
        for (x = 0; x < BBK9288S_LCD_WIDTH; x++) {
            uint8_t pixel = bbk9288s_lcd_vram_pixel(s, x, y);

            bbk9288s_lcd_store_pixel(surface, x, y, colors[pixel]);
        }
    }

    dpy_gfx_update(s->con, 0, 0, BBK9288S_LCD_WIDTH, BBK9288S_LCD_HEIGHT);
}

static void bbk9288s_lcd_invalidate(void *opaque)
{
    BBK9288SState *s = opaque;

    dpy_gfx_update(s->con, 0, 0, BBK9288S_LCD_WIDTH, BBK9288S_LCD_HEIGHT);
}

static const GraphicHwOps bbk9288s_lcd_ops = {
    .invalidate = bbk9288s_lcd_invalidate,
    .gfx_update = bbk9288s_lcd_update,
};

static bool bbk9288s_lcd_dump_pgm(BBK9288SState *s, const char *path)
{
    g_autofree char *header = NULL;
    GByteArray *out;
    GError *gerr = NULL;
    uint32_t counts[4] = { 0, 0, 0, 0 };
    bool ok;
    unsigned x;
    unsigned y;

    if (s->ivram_ptr == NULL) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-lcd: cannot dump; IVRAM is not mapped\n");
        return false;
    }
    if (path == NULL || path[0] == 0) {
        path = "bbk9288s-lcd.pgm";
    }

    header = g_strdup_printf("P5\n%u %u\n255\n",
                             BBK9288S_LCD_WIDTH, BBK9288S_LCD_HEIGHT);
    out = g_byte_array_sized_new(strlen(header) +
                                 BBK9288S_LCD_WIDTH * BBK9288S_LCD_HEIGHT);
    g_byte_array_append(out, (const uint8_t *)header, strlen(header));

    for (y = 0; y < BBK9288S_LCD_HEIGHT; y++) {
        for (x = 0; x < BBK9288S_LCD_WIDTH; x++) {
            uint8_t pixel = bbk9288s_lcd_vram_pixel(s, x, y);
            uint8_t gray = bbk9288s_lcd_gray_value(pixel);

            counts[pixel & 0x03]++;
            g_byte_array_append(out, &gray, 1);
        }
    }

    ok = g_file_set_contents(path, (const char *)out->data, out->len, &gerr);
    g_byte_array_unref(out);

    if (!ok) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bbk9288s-lcd: dump failed path=%s error=%s\n",
                      path, gerr != NULL ? gerr->message : "unknown");
        g_clear_error(&gerr);
        return false;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "bbk9288s-lcd: dumped %ux%u 2bpp framebuffer to %s "
                  "gray-counts=%u,%u,%u,%u\n",
                  BBK9288S_LCD_WIDTH, BBK9288S_LCD_HEIGHT, path,
                  counts[0], counts[1], counts[2], counts[3]);
    return true;
}

static void bbk9288s_debug_lcd_dump_cb(void *opaque)
{
    BBK9288SState *s = opaque;

    bbk9288s_lcd_dump_pgm(s, s->debug_lcd_dump_path);
}

static void bbk9288s_lcdc_io_reset(BBK9288SState *s)
{
    uint16_t hsize = BBK9288S_LCD_WIDTH / 8 - 1;
    uint16_t vsize = BBK9288S_LCD_HEIGHT - 1;

    memset(s->lcdc_regs, 0, sizeof(s->lcdc_regs));
    memset(s->lcdc_seen, 0, sizeof(s->lcdc_seen));

    /*
     * The board loader starts the KNL image directly without running the
     * original boot ROM. Seed the panel geometry so firmware LCD helper code
     * sees the 9288S's 160x240 screen while guest writes can still override it.
     */
    s->lcdc_regs[BBK9288S_LCDC_HSIZE] = hsize & 0xff;
    s->lcdc_regs[BBK9288S_LCDC_HSIZE + 1] = hsize >> 8;
    s->lcdc_regs[BBK9288S_LCDC_VSIZE] = vsize & 0xff;
    s->lcdc_regs[BBK9288S_LCDC_VSIZE + 1] = vsize >> 8;
}

static uint32_t bbk9288s_first_vector(const uint8_t *payload,
                                      uint32_t payload_size)
{
    uint32_t scan_size = MIN(payload_size, 256 * 4);
    uint32_t end = BBK9288S_SDRAM_BASE + payload_size;
    uint32_t off;

    for (off = 0; off + 4 <= scan_size; off += 4) {
        uint32_t value = ldl_le_p(payload + off);

        if (value >= BBK9288S_SDRAM_BASE && value < end &&
            (value & 1) == 0) {
            return value;
        }
    }

    return BBK9288S_SDRAM_BASE;
}

static void bbk9288s_load_kernel_data(S1C33CPU *cpu,
                                      const uint8_t *data,
                                      gsize len,
                                      const char *source,
                                      uint64_t ram_size)
{
    uint32_t body_size;
    uint32_t pc;

    if (len < BBK9288S_KERNEL_HEADER || memcmp(data, "KNL ", 4) != 0) {
        error_report("kernel from %s is not a BBK 9288S KNL image", source);
        exit(1);
    }

    body_size = ldl_le_p(data + 0x14);
    if (body_size > len - BBK9288S_KERNEL_HEADER) {
        error_report("kernel from %s has invalid body size 0x%x", source,
                     body_size);
        exit(1);
    }
    if (body_size > ram_size) {
        error_report("kernel body 0x%x is larger than RAM size 0x%" PRIx64,
                     body_size, ram_size);
        exit(1);
    }

    rom_add_blob_fixed("bbk9288s.kernel",
                       data + BBK9288S_KERNEL_HEADER,
                       body_size, BBK9288S_SDRAM_BASE);
    pc = bbk9288s_first_vector(data + BBK9288S_KERNEL_HEADER,
                               body_size);
    cpu_set_pc(CPU(cpu), pc);

    info_report("BBK9288S KNL %.16s: body=0x%x load=0x%08x pc=0x%08x",
                data + 4, body_size, BBK9288S_SDRAM_BASE, pc);
}

static void bbk9288s_load_kernel_file(S1C33CPU *cpu,
                                      const char *filename,
                                      uint64_t ram_size)
{
    g_autofree gchar *data = NULL;
    gsize len = 0;
    GError *gerr = NULL;

    if (!g_file_get_contents(filename, &data, &len, &gerr)) {
        error_report("could not load kernel '%s': %s", filename, gerr->message);
        g_error_free(gerr);
        exit(1);
    }
    bbk9288s_load_kernel_data(cpu, (const uint8_t *)data, len,
                              filename, ram_size);
}

static void bbk9288s_init(MachineState *machine)
{
    BBK9288SMachineState *bms = BBK9288S_MACHINE(machine);
    BBK9288SState *s = g_new0(BBK9288SState, 1);
    S1C33CPU *cpu;
    g_autofree uint8_t *nand_kernel = NULL;
    gsize nand_kernel_len = 0;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *iram = g_new(MemoryRegion, 1);
    const char *kernel = machine->kernel_filename ? machine->kernel_filename :
                                                 machine->firmware;

    s->trace_io = bms->trace_io;
    s->trace_key_scan = bms->trace_key_scan;
    s->strict_board_io = bms->strict_board_io;
    s->debug_key_p0_low_mask =
        bms->debug_key_p0_low_mask & BBK9288S_KEY_P0_MASK;
    s->touch_fpt = bms->touch_fpt;
    s->touch_k5_low_mask = bms->touch_k5_low_mask & 0x1f;
    s->touch_p0_low = bms->touch_p0_low;
    s->debug_timer16_factors = bms->debug_timer16_factors;
    if (s->touch_fpt < 4 || s->touch_fpt > 7) {
        warn_report("invalid bbk9288s touch-fpt=%u; using %u",
                    s->touch_fpt, BBK9288S_DEFAULT_TOUCH_FPT);
        s->touch_fpt = BBK9288S_DEFAULT_TOUCH_FPT;
    }
    s->debug_lcd_dump_path =
        g_strdup(bms->debug_lcd_dump_path != NULL &&
                 bms->debug_lcd_dump_path[0] != 0 ?
                 bms->debug_lcd_dump_path : "bbk9288s-lcd.pgm");
    bbk9288s_nand_storage_init(s, bms->nand_image_path);

    memory_region_init_ram(iram, NULL, "bbk9288s.iram", BBK9288S_IRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, 0, iram);
    rom_add_blob_fixed("bbk9288s.rtc-cookie", bbk9288s_iram_rtc_cookie,
                       sizeof(bbk9288s_iram_rtc_cookie),
                       BBK9288S_IRAM_RTC_COOKIE_OFFSET);
    memory_region_init_io(&s->touch_io, NULL, &bbk9288s_touch_io_ops, s,
                           "bbk9288s.touch-io", BBK9288S_TOUCH_IO_SIZE);
    bbk9288s_touch_io_reset(s);
    memory_region_add_subregion(sysmem, BBK9288S_TOUCH_IO_BASE, &s->touch_io);
    memory_region_init_io(&s->nand_io, NULL, &bbk9288s_nand_io_ops, s,
                          "bbk9288s.nand-data", BBK9288S_NAND_DATA_SIZE);
    memory_region_add_subregion(sysmem, BBK9288S_NAND_DATA_BASE, &s->nand_io);
    memory_region_init_ram(&s->ivram, NULL, "bbk9288s.ivram",
                           BBK9288S_IVRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, BBK9288S_IVRAM_BASE, &s->ivram);
    s->ivram_ptr = memory_region_get_ram_ptr(&s->ivram);
    s->con = graphic_console_init(NULL, 0, &bbk9288s_lcd_ops, s);
    qemu_console_resize(s->con, BBK9288S_LCD_WIDTH, BBK9288S_LCD_HEIGHT);
    memory_region_init_io(&s->board_io, NULL, &bbk9288s_board_io_ops, s,
                          "bbk9288s.board-io", BBK9288S_BOARD_IO_SIZE);
    bbk9288s_board_io_reset(s);
    memory_region_add_subregion(sysmem, BBK9288S_BOARD_IO_BASE, &s->board_io);
    memory_region_add_subregion(sysmem, BBK9288S_SDRAM_BASE, machine->ram);
    memory_region_init_io(&s->lcdc_io, NULL, &bbk9288s_lcdc_io_ops, s,
                          "bbk9288s.lcdc-io", BBK9288S_LCDC_IO_SIZE);
    bbk9288s_lcdc_io_reset(s);
    memory_region_add_subregion(sysmem, BBK9288S_LCDC_IO_BASE, &s->lcdc_io);
    memory_region_init_io(&s->io, NULL, &bbk9288s_io_ops, s,
                          "bbk9288s.s1c33l05-io", BBK9288S_IO_SIZE);
    bbk9288s_io_reset(s);
    memory_region_add_subregion(sysmem, BBK9288S_IO_BASE, &s->io);
    s->exit.notify = bbk9288s_exit_notify;
    qemu_add_exit_notifier(&s->exit);
    bbk9288s_active_machine = s;
    s->kbd_handler =
        qemu_input_handler_register(NULL, &bbk9288s_keyboard_handler);
    qemu_input_handler_activate(s->kbd_handler);
    s->ptr_handler =
        qemu_input_handler_register(NULL, &bbk9288s_pointer_handler);
    qemu_input_handler_activate(s->ptr_handler);

    cpu = S1C33_CPU(cpu_create(machine->cpu_type));
    s->cpu = CPU(cpu);
    cpu->env.sp = BBK9288S_BOOT_STACK_TOP;
    bbk9288s_set_ttbr(s, BBK9288S_SDRAM_BASE);

    if (kernel) {
        bbk9288s_load_kernel_file(cpu, kernel, machine->ram_size);
    } else {
        nand_kernel = bbk9288s_nand_extract_kernel(
            s, &nand_kernel_len, machine->ram_size);
        if (nand_kernel == NULL) {
            error_report("BBK9288S NAND does not contain a readable "
                         "FAT16 kernel.bin");
            exit(1);
        }
        bbk9288s_load_kernel_data(cpu, nand_kernel, nand_kernel_len,
                                  "NAND FAT16", machine->ram_size);
    }

    if (bms->debug_usb_wakeup_ms != 0) {
        s->debug_usb_wakeup_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME, bbk9288s_debug_usb_wakeup_cb, s);
        timer_mod(s->debug_usb_wakeup_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  bms->debug_usb_wakeup_ms);
        info_report("BBK9288S debug USB wakeup scheduled after %u ms",
                    bms->debug_usb_wakeup_ms);
    }
    if (bms->debug_nmi_wakeup_ms != 0) {
        s->debug_nmi_wakeup_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME, bbk9288s_debug_nmi_wakeup_cb, s);
        timer_mod(s->debug_nmi_wakeup_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  bms->debug_nmi_wakeup_ms);
        info_report("BBK9288S debug NMI wakeup scheduled after %u ms",
                    bms->debug_nmi_wakeup_ms);
    }
    if (bms->debug_port4_wakeup_ms != 0) {
        s->debug_port4_wakeup_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME,
                         bbk9288s_debug_port4_wakeup_cb, s);
        timer_mod(s->debug_port4_wakeup_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  bms->debug_port4_wakeup_ms);
        info_report("BBK9288S debug port4 wakeup scheduled after %u ms",
                    bms->debug_port4_wakeup_ms);
    }
    if (bms->debug_port5_wakeup_ms != 0) {
        s->debug_port5_wakeup_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME,
                         bbk9288s_debug_port5_wakeup_cb, s);
        timer_mod(s->debug_port5_wakeup_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  bms->debug_port5_wakeup_ms);
        info_report("BBK9288S debug port5 wakeup scheduled after %u ms",
                    bms->debug_port5_wakeup_ms);
    }
    if (bms->debug_lcd_dump_ms != 0) {
        s->debug_lcd_dump_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME,
                         bbk9288s_debug_lcd_dump_cb, s);
        timer_mod(s->debug_lcd_dump_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  bms->debug_lcd_dump_ms);
        info_report("BBK9288S debug LCD dump scheduled after %u ms to %s",
                    bms->debug_lcd_dump_ms, s->debug_lcd_dump_path);
    }
}

static bool bbk9288s_get_trace_io(Object *obj, Error **errp)
{
    return BBK9288S_MACHINE(obj)->trace_io;
}

static void bbk9288s_set_trace_io(Object *obj, bool value, Error **errp)
{
    BBK9288S_MACHINE(obj)->trace_io = value;
}

static bool bbk9288s_get_trace_key_scan(Object *obj, Error **errp)
{
    return BBK9288S_MACHINE(obj)->trace_key_scan;
}

static void bbk9288s_set_trace_key_scan(Object *obj, bool value, Error **errp)
{
    BBK9288S_MACHINE(obj)->trace_key_scan = value;
}

static char *bbk9288s_get_debug_lcd_dump_path(Object *obj, Error **errp)
{
    const char *path = BBK9288S_MACHINE(obj)->debug_lcd_dump_path;

    return g_strdup(path != NULL ? path : "");
}

static void bbk9288s_set_debug_lcd_dump_path(Object *obj, const char *value,
                                             Error **errp)
{
    BBK9288SMachineState *bms = BBK9288S_MACHINE(obj);

    g_free(bms->debug_lcd_dump_path);
    bms->debug_lcd_dump_path =
        g_strdup(value != NULL && value[0] != 0 ? value : "bbk9288s-lcd.pgm");
}

static char *bbk9288s_get_nand_image_path(Object *obj, Error **errp)
{
    const char *path = BBK9288S_MACHINE(obj)->nand_image_path;

    return g_strdup(path != NULL ? path : "");
}

static void bbk9288s_set_nand_image_path(Object *obj, const char *value,
                                         Error **errp)
{
    BBK9288SMachineState *bms = BBK9288S_MACHINE(obj);

    g_free(bms->nand_image_path);
    bms->nand_image_path =
        g_strdup(value != NULL && value[0] != 0 ? value : NULL);
}

static bool bbk9288s_get_strict_board_io(Object *obj, Error **errp)
{
    return BBK9288S_MACHINE(obj)->strict_board_io;
}

static void bbk9288s_set_strict_board_io(Object *obj, bool value,
                                         Error **errp)
{
    BBK9288S_MACHINE(obj)->strict_board_io = value;
}

static bool bbk9288s_get_touch_p0_low(Object *obj, Error **errp)
{
    return BBK9288S_MACHINE(obj)->touch_p0_low;
}

static void bbk9288s_set_touch_p0_low(Object *obj, bool value, Error **errp)
{
    BBK9288S_MACHINE(obj)->touch_p0_low = value;
}

static bool bbk9288s_get_debug_timer16_factors(Object *obj, Error **errp)
{
    return BBK9288S_MACHINE(obj)->debug_timer16_factors;
}

static void bbk9288s_set_debug_timer16_factors(Object *obj, bool value,
                                               Error **errp)
{
    BBK9288S_MACHINE(obj)->debug_timer16_factors = value;
}

static void bbk9288s_machine_instance_init(Object *obj)
{
    BBK9288SMachineState *bms = BBK9288S_MACHINE(obj);

    bms->debug_usb_wakeup_ms = 0;
    bms->debug_nmi_wakeup_ms = 0;
    bms->debug_port4_wakeup_ms = 0;
    bms->debug_port5_wakeup_ms = 0;
    bms->debug_key_p0_low_mask = 0;
    bms->debug_lcd_dump_ms = 0;
    bms->touch_fpt = BBK9288S_DEFAULT_TOUCH_FPT;
    bms->touch_k5_low_mask = 0;
    bms->debug_lcd_dump_path = g_strdup("bbk9288s-lcd.pgm");
    bms->nand_image_path = NULL;
    bms->touch_p0_low = true;
    bms->debug_timer16_factors = false;
    bms->trace_io = false;
    bms->trace_key_scan = false;
    bms->strict_board_io = false;
    object_property_add_uint32_ptr(obj, "debug-usb-wakeup-ms",
                                   &bms->debug_usb_wakeup_ms,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-usb-wakeup-ms",
                                    "Set the USB interrupt factor after this "
                                    "many host milliseconds; 0 disables it");
    object_property_add_uint32_ptr(obj, "debug-nmi-wakeup-ms",
                                   &bms->debug_nmi_wakeup_ms,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-nmi-wakeup-ms",
                                    "Raise NMI after this many host "
                                    "milliseconds; 0 disables it");
    object_property_add_uint32_ptr(obj, "debug-port4-wakeup-ms",
                                   &bms->debug_port4_wakeup_ms,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-port4-wakeup-ms",
                                    "Set the port input 4 interrupt factor "
                                    "after this many host milliseconds; "
                                    "0 disables it");
    object_property_add_uint32_ptr(obj, "debug-port5-wakeup-ms",
                                   &bms->debug_port5_wakeup_ms,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-port5-wakeup-ms",
                                    "Drive the diagnostic P05/FPT5 input low "
                                    "after this many host milliseconds; "
                                    "0 disables it");
    object_property_add_uint32_ptr(obj, "debug-key-p0-low-mask",
                                   &bms->debug_key_p0_low_mask,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-key-p0-low-mask",
                                    "Force selected P0 key-matrix input bits "
                                    "low from reset for scan tracing");
    object_property_add_uint32_ptr(obj, "touch-fpt",
                                   &bms->touch_fpt,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "touch-fpt",
                                     "Route QEMU touch pen-down to FPT4-FPT7; "
                                     "default is 6");
    object_property_add_uint32_ptr(obj, "touch-k5-low-mask",
                                   &bms->touch_k5_low_mask,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "touch-k5-low-mask",
                                    "Also drive selected K5 input bits low "
                                    "while QEMU touch is down; 0 disables it");
    object_property_add_bool(obj, "touch-p0-low",
                             bbk9288s_get_touch_p0_low,
                             bbk9288s_set_touch_p0_low);
    object_property_set_description(obj, "touch-p0-low",
                                    "Also expose QEMU touch pen-down as the "
                                    "selected P0/FPT bit low in GPIO reads; "
                                    "default is on");
    object_property_add_bool(obj, "debug-timer16-factors",
                             bbk9288s_get_debug_timer16_factors,
                             bbk9288s_set_debug_timer16_factors);
    object_property_set_description(obj, "debug-timer16-factors",
                                    "Diagnostic-only: let modeled 16-bit "
                                    "timer compare matches set FIR bits; "
                                    "default is off");
    object_property_add_uint32_ptr(obj, "debug-lcd-dump-ms",
                                   &bms->debug_lcd_dump_ms,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-lcd-dump-ms",
                                    "Dump the 160x240 LCD framebuffer as PGM "
                                    "after this many host milliseconds; "
                                    "0 disables it");
    object_property_add_str(obj, "debug-lcd-dump-path",
                            bbk9288s_get_debug_lcd_dump_path,
                            bbk9288s_set_debug_lcd_dump_path);
    object_property_set_description(obj, "debug-lcd-dump-path",
                                    "Path for debug-lcd-dump-ms output PGM");
    object_property_add_str(obj, "nand-image",
                            bbk9288s_get_nand_image_path,
                            bbk9288s_set_nand_image_path);
    object_property_set_description(obj, "nand-image",
                                    "Persistent raw 256 MiB NAND image, "
                                    "including 64-byte OOB per 2 KiB page");
    object_property_add_bool(obj, "trace-io",
                             bbk9288s_get_trace_io,
                             bbk9288s_set_trace_io);
    object_property_set_description(obj, "trace-io",
                                    "Log every BBK board, S1C33 IO, and LCDC "
                                    "IO access; default logs first accesses");
    object_property_add_bool(obj, "trace-key-scan",
                             bbk9288s_get_trace_key_scan,
                             bbk9288s_set_trace_key_scan);
    object_property_set_description(obj, "trace-key-scan",
                                    "Log compact P0/P1 keyboard scan state");
    object_property_add_bool(obj, "strict-board-io",
                             bbk9288s_get_strict_board_io,
                             bbk9288s_set_strict_board_io);
    object_property_set_description(obj, "strict-board-io",
                                    "Treat unknown board IO registers as "
                                    "non-loopback zeros for hardware-detect "
                                    "experiments");
}

static void bbk9288s_machine_instance_finalize(Object *obj)
{
    BBK9288SMachineState *bms = BBK9288S_MACHINE(obj);

    g_free(bms->debug_lcd_dump_path);
    bms->debug_lcd_dump_path = NULL;
    g_free(bms->nand_image_path);
    bms->nand_image_path = NULL;
}

static void bbk9288s_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "BBK 9288S (Epson S1C33L05, research)";
    mc->init = bbk9288s_init;
    mc->default_cpu_type = TYPE_C33L05_CPU;
    mc->default_ram_size = BBK9288S_DEFAULT_RAM;
    mc->default_ram_id = "bbk9288s.sdram";
    mc->max_cpus = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
}

static const TypeInfo bbk9288s_machine_type_info = {
    .name = TYPE_BBK9288S_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(BBK9288SMachineState),
    .instance_init = bbk9288s_machine_instance_init,
    .instance_finalize = bbk9288s_machine_instance_finalize,
    .class_init = bbk9288s_machine_class_init,
};

static void bbk9288s_machine_register_types(void)
{
    type_register_static(&bbk9288s_machine_type_info);
}

type_init(bbk9288s_machine_register_types)
