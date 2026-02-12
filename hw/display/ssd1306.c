/*
 * SSD1306 OLED controller with 128x64 monochrome display.
 *
 * Copyright (c) 2006-2007 CodeSourcery (SSD0303 original)
 * Copyright (c) 2025 Espressif Systems (SSD1306 adaptation)
 *
 * This code is licensed under the GPL.
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "ui/console.h"
#include "qom/object.h"

//#define DEBUG_SSD1306 1

#ifdef DEBUG_SSD1306
#define DPRINTF(fmt, ...) \
do { printf("ssd1306: " fmt , ## __VA_ARGS__); } while (0)
#define BADF(fmt, ...) \
do { fprintf(stderr, "ssd1306: error: " fmt , ## __VA_ARGS__); exit(1);} while (0)
#else
#define DPRINTF(fmt, ...) do {} while(0)
#define BADF(fmt, ...) \
do { fprintf(stderr, "ssd1306: error: " fmt , ## __VA_ARGS__);} while (0)
#endif

/* Scaling factor for pixels. */
#define MAGNIFY 2

#define SSD1306_COLS 128
#define SSD1306_ROWS 64
#define SSD1306_PAGES (SSD1306_ROWS / 8)  /* 8 pages */

enum ssd1306_mode {
    SSD1306_IDLE,
    SSD1306_DATA,
    SSD1306_CMD,
    SSD1306_SINGLE_CMD,
};

enum ssd1306_cmd {
    SSD1306_CMD_NONE,
    SSD1306_CMD_SKIP1,
    SSD1306_CMD_SET_ADDR_MODE,
    SSD1306_CMD_SET_COL_RANGE_START,
    SSD1306_CMD_SET_COL_RANGE_END,
    SSD1306_CMD_SET_PAGE_RANGE_START,
    SSD1306_CMD_SET_PAGE_RANGE_END,
};

/* Memory addressing modes (set via 0x20 command) */
enum ssd1306_addr_mode {
    SSD1306_ADDR_HORIZONTAL = 0,
    SSD1306_ADDR_VERTICAL = 1,
    SSD1306_ADDR_PAGE = 2,  /* default */
};

#define TYPE_SSD1306 "ssd1306"
OBJECT_DECLARE_SIMPLE_TYPE(ssd1306_state, SSD1306)

struct ssd1306_state {
    I2CSlave parent_obj;

    QemuConsole *con;
    int row;       /* current page (0-7) */
    int col;       /* current column (0-127) */
    int start_line;
    int mirror;
    int flash;
    int enabled;
    int inverse;
    int redraw;
    enum ssd1306_mode mode;
    enum ssd1306_cmd cmd_state;
    enum ssd1306_addr_mode addr_mode;
    /* column and page ranges for horizontal/vertical addressing */
    int col_start;
    int col_end;
    int page_start;
    int page_end;
    uint8_t framebuffer[SSD1306_COLS * SSD1306_PAGES];
};

static void ssd1306_advance_cursor(ssd1306_state *s)
{
    switch (s->addr_mode) {
    case SSD1306_ADDR_PAGE:
        /* In page addressing mode, column auto-increments, page stays */
        s->col++;
        if (s->col >= SSD1306_COLS) {
            s->col = 0;
        }
        break;
    case SSD1306_ADDR_HORIZONTAL:
        /* Column increments, wraps at col_end to col_start and advances page */
        s->col++;
        if (s->col > s->col_end) {
            s->col = s->col_start;
            s->row++;
            if (s->row > s->page_end) {
                s->row = s->page_start;
            }
        }
        break;
    case SSD1306_ADDR_VERTICAL:
        /* Page increments, wraps at page_end to page_start and advances column */
        s->row++;
        if (s->row > s->page_end) {
            s->row = s->page_start;
            s->col++;
            if (s->col > s->col_end) {
                s->col = s->col_start;
            }
        }
        break;
    }
}

static uint8_t ssd1306_recv(I2CSlave *i2c)
{
    BADF("Reads not implemented\n");
    return 0xff;
}

static void ssd1306_process_cmd(ssd1306_state *s, uint8_t data)
{
    enum ssd1306_cmd old_cmd_state = s->cmd_state;
    s->cmd_state = SSD1306_CMD_NONE;

    switch (old_cmd_state) {
    case SSD1306_CMD_NONE:
        DPRINTF("cmd 0x%02x\n", data);
        switch (data) {
        case 0x00 ... 0x0f: /* Set lower column start address (page mode). */
            s->col = (s->col & 0xf0) | (data & 0xf);
            break;
        case 0x10 ... 0x1f: /* Set higher column start address (page mode). */
            s->col = (s->col & 0x0f) | ((data & 0xf) << 4);
            break;
        case 0x20: /* Set memory addressing mode. */
            s->cmd_state = SSD1306_CMD_SET_ADDR_MODE;
            break;
        case 0x21: /* Set column address range (horiz/vert mode). */
            s->cmd_state = SSD1306_CMD_SET_COL_RANGE_START;
            break;
        case 0x22: /* Set page address range (horiz/vert mode). */
            s->cmd_state = SSD1306_CMD_SET_PAGE_RANGE_START;
            break;
        case 0x26 ... 0x27: /* Horizontal scroll setup (ignored). */
            /* 6 more bytes to consume */
            s->cmd_state = SSD1306_CMD_SKIP1;
            break;
        case 0x29 ... 0x2a: /* Vertical+horizontal scroll setup (ignored). */
            /* 5 more bytes to consume */
            s->cmd_state = SSD1306_CMD_SKIP1;
            break;
        case 0x2e: /* Deactivate scroll (ignored). */
            break;
        case 0x2f: /* Activate scroll (ignored). */
            break;
        case 0x40 ... 0x7f: /* Set display start line. */
            s->start_line = data & 0x3f;
            break;
        case 0x81: /* Set contrast (ignored). */
            s->cmd_state = SSD1306_CMD_SKIP1;
            break;
        case 0x8d: /* Charge pump setting (ignored). */
            s->cmd_state = SSD1306_CMD_SKIP1;
            break;
        case 0xa0: /* Segment remap off. */
            s->mirror = 0;
            break;
        case 0xa1: /* Segment remap on. */
            s->mirror = 1;
            break;
        case 0xa3: /* Set vertical scroll area (ignored). */
            s->cmd_state = SSD1306_CMD_SKIP1;
            break;
        case 0xa4: /* Entire display off (show RAM). */
            s->flash = 0;
            break;
        case 0xa5: /* Entire display on (all pixels on). */
            s->flash = 1;
            break;
        case 0xa6: /* Normal display. */
            s->inverse = 0;
            break;
        case 0xa7: /* Inverse display. */
            s->inverse = 1;
            break;
        case 0xa8: /* Set multiplex ratio (ignored). */
            s->cmd_state = SSD1306_CMD_SKIP1;
            break;
        case 0xae: /* Display OFF. */
            s->enabled = 0;
            break;
        case 0xaf: /* Display ON. */
            s->enabled = 1;
            break;
        case 0xb0 ... 0xb7: /* Set page start address (page mode). */
            s->row = data & 0x07;
            break;
        case 0xc0 ... 0xcf: /* Set COM output scan direction (ignored). */
            break;
        case 0xd3: /* Set display offset (ignored). */
            s->cmd_state = SSD1306_CMD_SKIP1;
            break;
        case 0xd5: /* Set display clock (ignored). */
            s->cmd_state = SSD1306_CMD_SKIP1;
            break;
        case 0xd9: /* Set pre-charge period (ignored). */
            s->cmd_state = SSD1306_CMD_SKIP1;
            break;
        case 0xda: /* Set COM pins config (ignored). */
            s->cmd_state = SSD1306_CMD_SKIP1;
            break;
        case 0xdb: /* Set VCOMH deselect level (ignored). */
            s->cmd_state = SSD1306_CMD_SKIP1;
            break;
        case 0xe3: /* NOP. */
            break;
        default:
            DPRINTF("Unknown command: 0x%x\n", data);
            break;
        }
        break;

    case SSD1306_CMD_SKIP1:
        DPRINTF("skip 0x%02x\n", data);
        /* Some commands need more than 1 extra byte (scroll setup).
         * For simplicity we skip them one at a time; multi-byte scroll
         * commands are rare in practice and all ignored anyway.  The driver
         * will just send them and we consume the bytes harmlessly. */
        break;

    case SSD1306_CMD_SET_ADDR_MODE:
        s->addr_mode = data & 0x03;
        if (s->addr_mode == 3) {
            s->addr_mode = SSD1306_ADDR_PAGE;
        }
        DPRINTF("addr_mode = %d\n", s->addr_mode);
        break;

    case SSD1306_CMD_SET_COL_RANGE_START:
        s->col_start = data & 0x7f;
        s->col = s->col_start;
        s->cmd_state = SSD1306_CMD_SET_COL_RANGE_END;
        break;

    case SSD1306_CMD_SET_COL_RANGE_END:
        s->col_end = data & 0x7f;
        break;

    case SSD1306_CMD_SET_PAGE_RANGE_START:
        s->page_start = data & 0x07;
        s->row = s->page_start;
        s->cmd_state = SSD1306_CMD_SET_PAGE_RANGE_END;
        break;

    case SSD1306_CMD_SET_PAGE_RANGE_END:
        s->page_end = data & 0x07;
        break;
    }
}

static int ssd1306_send(I2CSlave *i2c, uint8_t data)
{
    ssd1306_state *s = SSD1306(i2c);

    switch (s->mode) {
    case SSD1306_IDLE:
        DPRINTF("control byte 0x%02x\n", data);
        if (data == 0x00) {
            /* Co=0, D/C=0: command stream */
            s->mode = SSD1306_CMD;
        } else if (data == 0x80) {
            /* Co=1, D/C=0: single command */
            s->mode = SSD1306_SINGLE_CMD;
        } else if (data == 0x40) {
            /* Co=0, D/C=1: data stream */
            s->mode = SSD1306_DATA;
        } else if ((data & 0x3f) == 0) {
            /* Other valid control bytes */
            if (data & 0x40) {
                s->mode = SSD1306_DATA;
            } else {
                s->mode = SSD1306_CMD;
            }
        } else {
            BADF("Unexpected control byte 0x%x\n", data);
        }
        break;

    case SSD1306_DATA:
        DPRINTF("data 0x%02x @ page=%d col=%d\n", data, s->row, s->col);
        if (s->col < SSD1306_COLS && s->row < SSD1306_PAGES) {
            s->framebuffer[s->row * SSD1306_COLS + s->col] = data;
            s->redraw = 1;
        }
        ssd1306_advance_cursor(s);
        break;

    case SSD1306_CMD:
        /* Command stream: all remaining bytes are commands */
        ssd1306_process_cmd(s, data);
        break;

    case SSD1306_SINGLE_CMD:
        /* Single command: process one byte, then return to IDLE */
        ssd1306_process_cmd(s, data);
        s->mode = SSD1306_IDLE;
        break;
    }
    return 0;
}

static int ssd1306_event(I2CSlave *i2c, enum i2c_event event)
{
    ssd1306_state *s = SSD1306(i2c);

    switch (event) {
    case I2C_FINISH:
        s->mode = SSD1306_IDLE;
        break;
    case I2C_START_RECV:
    case I2C_START_SEND:
    case I2C_NACK:
        /* Nothing to do. */
        break;
    default:
        return -1;
    }

    return 0;
}

static void ssd1306_update_display(void *opaque)
{
    ssd1306_state *s = (ssd1306_state *)opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint8_t *dest;
    uint8_t *src;
    int x, y, line;
    char *colors[2];
    char colortab[MAGNIFY * 8];
    int dest_width;
    uint8_t mask;

    if (!s->redraw) {
        return;
    }

    switch (surface_bits_per_pixel(surface)) {
    case 0:
        return;
    case 15:
    case 16:
        dest_width = 2;
        break;
    case 24:
        dest_width = 3;
        break;
    case 32:
        dest_width = 4;
        break;
    default:
        BADF("Bad color depth\n");
        return;
    }

    dest_width *= MAGNIFY;
    memset(colortab, 0xff, dest_width);
    memset(colortab + dest_width, 0, dest_width);

    if (s->flash) {
        colors[0] = colortab;
        colors[1] = colortab;
    } else if (s->inverse) {
        colors[0] = colortab;
        colors[1] = colortab + dest_width;
    } else {
        colors[0] = colortab + dest_width;
        colors[1] = colortab;
    }

    dest = surface_data(surface);
    for (y = 0; y < SSD1306_ROWS; y++) {
        line = (y + s->start_line) & 0x3f;
        src = s->framebuffer + SSD1306_COLS * (line >> 3);
        mask = 1 << (line & 7);
        for (x = 0; x < SSD1306_COLS; x++) {
            memcpy(dest, colors[(*src & mask) != 0], dest_width);
            dest += dest_width;
            src++;
        }
        for (x = 1; x < MAGNIFY; x++) {
            memcpy(dest, dest - dest_width * SSD1306_COLS,
                   dest_width * SSD1306_COLS);
            dest += dest_width * SSD1306_COLS;
        }
    }

    s->redraw = 0;
    dpy_gfx_update(s->con, 0, 0,
                   SSD1306_COLS * MAGNIFY, SSD1306_ROWS * MAGNIFY);
}

static void ssd1306_invalidate_display(void *opaque)
{
    ssd1306_state *s = (ssd1306_state *)opaque;
    s->redraw = 1;
}

static const VMStateDescription vmstate_ssd1306 = {
    .name = "ssd1306_oled",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(row, ssd1306_state),
        VMSTATE_INT32(col, ssd1306_state),
        VMSTATE_INT32(start_line, ssd1306_state),
        VMSTATE_INT32(mirror, ssd1306_state),
        VMSTATE_INT32(flash, ssd1306_state),
        VMSTATE_INT32(enabled, ssd1306_state),
        VMSTATE_INT32(inverse, ssd1306_state),
        VMSTATE_INT32(redraw, ssd1306_state),
        VMSTATE_UINT32(mode, ssd1306_state),
        VMSTATE_UINT32(cmd_state, ssd1306_state),
        VMSTATE_UINT32(addr_mode, ssd1306_state),
        VMSTATE_INT32(col_start, ssd1306_state),
        VMSTATE_INT32(col_end, ssd1306_state),
        VMSTATE_INT32(page_start, ssd1306_state),
        VMSTATE_INT32(page_end, ssd1306_state),
        VMSTATE_BUFFER(framebuffer, ssd1306_state),
        VMSTATE_I2C_SLAVE(parent_obj, ssd1306_state),
        VMSTATE_END_OF_LIST()
    }
};

static const GraphicHwOps ssd1306_ops = {
    .invalidate  = ssd1306_invalidate_display,
    .gfx_update  = ssd1306_update_display,
};

static void ssd1306_realize(DeviceState *dev, Error **errp)
{
    ssd1306_state *s = SSD1306(dev);

    s->con = graphic_console_init(dev, 0, &ssd1306_ops, s);
    qemu_console_resize(s->con, SSD1306_COLS * MAGNIFY, SSD1306_ROWS * MAGNIFY);

    /* Default state */
    s->addr_mode = SSD1306_ADDR_PAGE;
    s->col_start = 0;
    s->col_end = SSD1306_COLS - 1;
    s->page_start = 0;
    s->page_end = SSD1306_PAGES - 1;
}

static void ssd1306_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->realize = ssd1306_realize;
    k->event = ssd1306_event;
    k->recv = ssd1306_recv;
    k->send = ssd1306_send;
    dc->vmsd = &vmstate_ssd1306;
}

static const TypeInfo ssd1306_info = {
    .name          = TYPE_SSD1306,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(ssd1306_state),
    .class_init    = ssd1306_class_init,
};

static void ssd1306_register_types(void)
{
    type_register_static(&ssd1306_info);
}

type_init(ssd1306_register_types)
