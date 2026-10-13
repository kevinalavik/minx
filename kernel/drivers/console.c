#include "console.h"

#include "fb.h"
#include "io.h"
#include "kmalloc.h"
#include "panic.h"
#include "percpu.h"
#include "spinlock.h"
#include "serial.h"
#include "string.h"
#include "types.h"

/* 8-bit per-channel palette used for the 16 ANSI colours. */
static const uint32_t palette_normal[8] = {
    0x000000, /* black   */
    0xaa0000, /* red     */
    0x00aa00, /* green   */
    0xaa5500, /* brown   */
    0x0000aa, /* blue    */
    0xaa00aa, /* magenta */
    0x00aaaa, /* cyan    */
    0xaaaaaa, /* white   */
};

static const uint32_t palette_bright[8] = {
    0x555555, /* bright black   */
    0xff5555, /* bright red     */
    0x55ff55, /* bright green   */
    0xffff55, /* bright yellow  */
    0x5555ff, /* bright blue    */
    0xff55ff, /* bright magenta */
    0x55ffff, /* bright cyan    */
    0xffffff, /* bright white   */
};

#define CONSOLE_DEFAULT_FG 0xaaaaaa
#define CONSOLE_DEFAULT_BG 0x0a0a0a

#define FONT_GLYPHS 256u
#define FONT_W      8u
#define FONT_H      16u
#define FONT_BYTES_PER_ROW (FONT_W / 8u)

/* The largest console this build can be asked for: 256 columns by 64 rows, which is what a
 * 1920x1080 framebuffer gives at this font.  The cell buffer is sized to it rather than
 * allocated, so that it is still there after the heap has come up -- see console_init(). */
#define CONSOLE_MAX_CELLS (256u * 64u)

extern const unsigned char g_font8x16[];

typedef struct {
    uint8_t       *cells;      /* row * cols + col -> font glyph index */
    uint16_t      *attrs;      /* row * cols + col -> attribute word  */
    uint32_t       rows;
    uint32_t       cols;
    uint32_t       cursor_row;
    uint32_t       cursor_col;
    uint32_t       saved_row;
    uint32_t       saved_col;
    console_color_t cur;
    bool           cursor_visible;
    bool           enabled;
    bool           fb_ok;
    uint32_t       font_scale;
    uint32_t       line_pixels;
    uint32_t       top;        /* scrolling region, inclusive */
    uint32_t       bottom;     /* scrolling region, inclusive */
} con_t;

static con_t con;

/* --- ANSI escape parsing ------------------------------------------------ */

enum {
    ESC_NONE = 0,
    ESC_START,
    ESC_CSI,
    ESC_OSC,
    ESC_OSC_ESC,
};

static uint8_t esc_state;
static char csi_buf[32];
static uint32_t csi_len;

/* --- geometry ----------------------------------------------------------- */

static void cell_index(uint32_t row, uint32_t col, uint32_t *idx) {
    *idx = row * con.cols + col;
}

static void cell_put(uint32_t row, uint32_t col, uint8_t glyph, uint16_t attr) {
    uint32_t idx;
    cell_index(row, col, &idx);
    con.cells[idx] = glyph;
    con.attrs[idx] = attr;
}

static uint16_t pack_attr(console_color_t c) {
    return (uint16_t)(c.attr & 0x3f);
}

static void draw_cell(uint32_t row, uint32_t col) {
    if (!con.fb_ok)
        return;

    uint32_t idx;
    cell_index(row, col, &idx);
    uint8_t glyph = con.cells[idx];
    uint16_t attr = con.attrs[idx];

    uint32_t fg = con.cur.fg;
    uint32_t bg = con.cur.bg;
    if (attr & CONSOLE_ATTR_INVERSE) {
        uint32_t t = fg;
        fg = bg;
        bg = t;
    }
    if (attr & CONSOLE_ATTR_BOLD) {
        /* Bold either brightens an ANSI colour or lightens a raw RGB one. */
        for (int i = 0; i < 8; i++) {
            if (fg == palette_normal[i])
                fg = palette_bright[i];
        }
    }

    const uint8_t *bitmap = &g_font8x16[(size_t)glyph * FONT_BYTES_PER_ROW *
                                        FONT_H];
    uint32_t px = col * FONT_W * con.font_scale;
    uint32_t py = row * con.line_pixels;

    fb_draw_glyph(px, py, bitmap, FONT_W, FONT_H, FONT_BYTES_PER_ROW, fg, bg,
                  false, con.font_scale, con.font_scale);
}

static void redraw_row(uint32_t row) {
    for (uint32_t col = 0; col < con.cols; col++)
        draw_cell(row, col);
}

static void draw_cursor(void) {
    if (!con.fb_ok || !con.cursor_visible || !con.enabled)
        return;
    if (con.cursor_row >= con.rows || con.cursor_col >= con.cols)
        return;

    uint32_t px = con.cursor_col * FONT_W * con.font_scale;
    uint32_t py = con.cursor_row * con.line_pixels;

    /* Invert the block under the cursor by drawing an inverted-space glyph
     * tinted with the current colours. */
    uint32_t fg = con.cur.bg;
    uint32_t bg = con.cur.fg;
    if (con.cur.attr & CONSOLE_ATTR_INVERSE) {
        fg = con.cur.fg;
        bg = con.cur.bg;
    }

    uint32_t w = FONT_W * con.font_scale;
    uint32_t h = FONT_H * con.font_scale;
    uint32_t bar = h > con.font_scale ? con.font_scale : 1;
    fb_draw_rect(px, py, w, bar, bg);
    fb_draw_rect(px, py + h - bar, w, bar, bg);
    fb_draw_rect(px + w - bar, py, bar, h, bg);
    fb_draw_rect(px, py, bar, h, bg);
    (void)fg;
}

static void clear_cursor(void) {
    /* The cell the cursor is sitting on, and nothing else.
     *
     * This used to repaint the cursor's whole row -- a hundred and sixty `draw_cell()`
     * calls -- before every character.  That is correct, and it is why typing felt broken:
     * the echo of a keystroke had to wait behind a hundred and sixty glyph blits to clear a
     * bar eight pixels wide, so a character appeared only once the *next* one had been
     * typed.  From the keyboard that is "the letter shows up when I press the next key".
     *
     * The cell under the cursor is the only thing the bar can be hiding, so repainting
     * that one cell is the whole job. */
    if (!con.fb_ok || !con.enabled)
        return;
    if (con.cursor_row < con.rows && con.cursor_col < con.cols)
        draw_cell(con.cursor_row, con.cursor_col);
}

static void move_cursor(uint32_t row, uint32_t col) {
    if (con.rows == 0 || con.cols == 0)
        return;
    if (row >= con.rows)
        row = con.rows - 1;
    if (col >= con.cols)
        col = con.cols - 1;
    con.cursor_row = row;
    con.cursor_col = col;
    draw_cursor();
}

static void scroll_up(uint32_t n) {
    if (con.rows == 0 || con.cols == 0)
        return;
    if (con.bottom < con.top)
        return;

    /* Clamped to the region, because the blanking loop below counts back from the bottom
     * and an `n` larger than the region's height would wrap that subtraction. */
    uint32_t region = con.bottom - con.top + 1;

    if (n > region)
        n = region;
    if (n == 0)
        return;

    for (uint32_t row = con.top; row + n <= con.bottom; row++) {
        for (uint32_t col = 0; col < con.cols; col++) {
            uint32_t dst, src;
            cell_index(row, col, &dst);
            cell_index(row + n, col, &src);
            con.cells[dst] = con.cells[src];
            con.attrs[dst] = con.attrs[src];
        }
    }
    /* Blank the freshly exposed lines. */
    for (uint32_t row = con.bottom + 1 - n; row <= con.bottom; row++) {
        for (uint32_t col = 0; col < con.cols; col++) {
            uint16_t attr = pack_attr(con.cur);
            cell_put(row, col, ' ', attr);
        }
    }

    if (con.fb_ok) {
        /* Move the pixels, not the glyphs.
         *
         * This repainted every cell of the scrolling region -- eight thousand glyph blits
         * for a fifty-line console -- for every line that went past the bottom.  The comment
         * it replaced claimed that "costs nothing at console speeds"; it costs nothing *at*
         * console speeds, and everything at typing speed, which is why scrolling was slow
         * enough to be the first thing anyone noticed about the console.
         *
         * Colours stay exactly right because a cell's colour lives in the glyphs rendered
         * from it, not in the framebuffer: moving a row of pixels moves the rendering of a
         * row of cells precisely as redrawing that row would.
         *
         * Only the columns the console uses are moved.  The framebuffer is normally wider
         * than the text area -- 1280 pixels for a 160-column console -- and scrolling the
         * whole line would drag along pixels that belong to nothing. */
        const fb_info_t *fb = fb_get_info();

        if (fb != NULL && fb->ready && fb->base != NULL && con.line_pixels > 0) {
            uint32_t used_px = con.cols * FONT_W * con.font_scale;
            uint32_t row_bytes;

            if (used_px > fb->width)
                used_px = fb->width;

            row_bytes = used_px * (fb->bpp / 8u);

            /* Destination is always above source and the two never share a row, so every
             * row can be moved independently, and moving them from the top down keeps each
             * `memmove` from reading a row some later iteration has already written. */
            for (uint32_t row = con.top; row + n <= con.bottom; row++) {
                uint8_t *dst = fb->base + (uint64_t)row * con.line_pixels * fb->pitch;
                uint8_t *src = fb->base + (uint64_t)(row + n) * con.line_pixels * fb->pitch;

                memmove(dst, src, row_bytes);
            }

            /* And blank what the move exposed, in the current background -- which is what a
             * row of spaces would have drawn. */
            uint32_t bg = (con.cur.attr & CONSOLE_ATTR_INVERSE) ? con.cur.fg : con.cur.bg;

            for (uint32_t row = con.bottom + 1 - n; row <= con.bottom; row++) {
                uint32_t y = row * con.line_pixels;

                fb_fill(y, 0, y + con.line_pixels, used_px, bg);
            }
        }
    }
}

/* --- SGR ---------------------------------------------------------------- */

static void sgr_reset(void) {
    con.cur.fg = CONSOLE_DEFAULT_FG;
    con.cur.bg = CONSOLE_DEFAULT_BG;
    con.cur.attr = CONSOLE_ATTR_NORMAL;
}

static void apply_sgr(void) {
    uint32_t params[8];
    uint32_t count = 0;
    uint32_t i = 0;

    while (i < csi_len && count < 8) {
        uint32_t value = 0;
        bool any = false;
        while (i < csi_len && csi_buf[i] >= '0' && csi_buf[i] <= '9') {
            value = value * 10 + (uint32_t)(csi_buf[i] - '0');
            i++;
            any = true;
        }
        params[count++] = any ? value : 0;
        if (i < csi_len && csi_buf[i] == ';')
            i++;
        else if (i < csi_len && csi_buf[i] == ':')
            i++;   /* treat sub-parameters as separate parameters */
        else
            break;
    }
    if (count == 0)
        params[count++] = 0;

    for (uint32_t p = 0; p < count; p++) {
        uint32_t v = params[p];
        switch (v) {
        case 0:
            sgr_reset();
            break;
        case 1: con.cur.attr |= CONSOLE_ATTR_BOLD; break;
        case 2: con.cur.attr |= CONSOLE_ATTR_DIM; break;
        case 3: con.cur.attr |= CONSOLE_ATTR_ITALIC; break;
        case 4: con.cur.attr |= CONSOLE_ATTR_UNDER; break;
        case 5: con.cur.attr |= CONSOLE_ATTR_BLINK; break;
        case 7: con.cur.attr |= CONSOLE_ATTR_INVERSE; break;
        case 22: con.cur.attr &= (uint16_t)~(CONSOLE_ATTR_BOLD | CONSOLE_ATTR_DIM); break;
        case 23: con.cur.attr &= (uint16_t)~CONSOLE_ATTR_ITALIC; break;
        case 24: con.cur.attr &= (uint16_t)~CONSOLE_ATTR_UNDER; break;
        case 25: con.cur.attr &= (uint16_t)~CONSOLE_ATTR_BLINK; break;
        case 27: con.cur.attr &= (uint16_t)~CONSOLE_ATTR_INVERSE; break;
        case 39: con.cur.fg = CONSOLE_DEFAULT_FG; break;
        case 49: con.cur.bg = CONSOLE_DEFAULT_BG; break;
        case 38:
            if (p + 2 < count && params[p + 1] == 2 && p + 4 < count) {
                con.cur.fg = ((params[p + 2] & 0xff) << 16) |
                             ((params[p + 3] & 0xff) << 8) |
                             (params[p + 4] & 0xff);
                p += 4;
            }
            break;
        case 48:
            if (p + 2 < count && params[p + 1] == 2 && p + 4 < count) {
                con.cur.bg = ((params[p + 2] & 0xff) << 16) |
                             ((params[p + 3] & 0xff) << 8) |
                             (params[p + 4] & 0xff);
                p += 4;
            }
            break;
        default:
            if (v >= 30 && v <= 37) {
                con.cur.fg = palette_normal[v - 30];
            } else if (v >= 40 && v <= 47) {
                con.cur.bg = palette_normal[v - 40];
            } else if (v >= 90 && v <= 97) {
                con.cur.fg = palette_bright[v - 90];
            } else if (v >= 100 && v <= 107) {
                con.cur.bg = palette_bright[v - 100];
            } else if (v >= 40 && v < 50) {
                /* already handled above */
            }
            break;
        }
    }
}

static void parse_csi(void) {
    if (csi_len == 0)
        return;

    char final = csi_buf[csi_len - 1];
    csi_buf[csi_len - 1] = '\0';

    uint32_t params[4] = { 0, 0, 0, 0 };
    uint32_t count = 0;
    uint32_t i = 0;
    while (i < csi_len - 1 && count < 4) {
        uint32_t value = 0;
        while (i < csi_len - 1 && csi_buf[i] >= '0' && csi_buf[i] <= '9')
            value = value * 10 + (uint32_t)(csi_buf[i++] - '0');
        params[count++] = value;
        if (i < csi_len - 1 && csi_buf[i] == ';')
            i++;
        else
            break;
    }

    uint32_t p0 = count > 0 ? params[0] : 0;
    if (p0 == 0)
        p0 = 1;   /* a zero or missing parameter means "one" for movement */

    /* Movement is computed in a signed type so that a huge parameter cannot
     * wrap around and land the cursor at the far edge of the screen. */
    int64_t row = con.cursor_row;
    int64_t col = con.cursor_col;

    switch (final) {
    case 'm':
        apply_sgr();
        break;
    case 'H':
    case 'f':
        row = (int64_t)(count > 0 ? params[0] : 1) - 1;
        col = (int64_t)(count > 1 ? params[1] : 1) - 1;
        break;
    case 'A': row -= p0; break;
    case 'B': row += p0; break;
    case 'C': col += p0; break;
    case 'D': col -= p0; break;
    case 'G': col = (int64_t)p0 - 1; break;
    case 'd': row = (int64_t)p0 - 1; break;
    case 'E': row += p0; col = 0; break;
    case 'F': row -= p0; col = 0; break;
    default:
        break;
    }

    if (row < 0)
        row = 0;
    if (col < 0)
        col = 0;
    if (row >= (int64_t)con.rows)
        row = (int64_t)con.rows - 1;
    if (col >= (int64_t)con.cols)
        col = (int64_t)con.cols - 1;

    switch (final) {
    case 'H':
    case 'f':
    case 'A':
    case 'B':
    case 'C':
    case 'D':
    case 'G':
    case 'd':
    case 'E':
    case 'F':
        clear_cursor();
        move_cursor((uint32_t)row, (uint32_t)col);
        break;
    case 'J': {
        uint32_t mode = count > 0 ? params[0] : 0;
        clear_cursor();
        for (uint32_t row = 0; row < con.rows; row++) {
            uint32_t from = (mode == 0 && row == con.cursor_row)
                                ? con.cursor_col + 1
                                : 0;
            if (row == con.cursor_row && (mode == 1 || mode == 2))
                from = 0;
            if (mode == 1 && row > con.cursor_row)
                continue;
            if (mode == 2 && row != con.cursor_row)
                continue;
            for (uint32_t col = from; col < con.cols; col++) {
                uint16_t attr = pack_attr(con.cur);
                cell_put(row, col, ' ', attr);
            }
            redraw_row(row);
        }
        break;
    }
    case 'K': {
        uint32_t mode = count > 0 ? params[0] : 0;
        clear_cursor();
        uint32_t from = 0, to = con.cols;
        if (mode == 0)
            from = con.cursor_col;
        else if (mode == 1)
            to = con.cursor_col + 1;
        for (uint32_t col = from; col < to && col < con.cols; col++) {
            uint16_t attr = pack_attr(con.cur);
            cell_put(con.cursor_row, col, ' ', attr);
        }
        redraw_row(con.cursor_row);
        break;
    }
    case 's':
        con.saved_row = con.cursor_row;
        con.saved_col = con.cursor_col;
        break;
    case 'u':
        clear_cursor();
        move_cursor(con.saved_row, con.saved_col);
        break;
    case 'r': {
        uint32_t top = (count > 0 && params[0] > 0) ? params[0] - 1 : 0;
        uint32_t bottom = (count > 1 && params[1] > 0) ? params[1] - 1 : con.rows - 1;
        if (top < con.rows && bottom < con.rows && top < bottom) {
            con.top = top;
            con.bottom = bottom;
        } else {
            con.top = 0;
            con.bottom = con.rows - 1;
        }
        clear_cursor();
        move_cursor(0, 0);
        break;
    }
    default:
        /* Unsupported control: ignore it rather than printing garbage. */
        break;
    }
}

/* --- output ------------------------------------------------------------- */

static void newline(void) {
    if (con.cursor_col != 0) {
        /* Overwrite the remainder of the line, like most terminals do. */
    }
    if (con.cursor_row >= con.bottom) {
        scroll_up(1);
    } else if (con.cursor_row + 1 < con.rows) {
        con.cursor_row++;
    }
    con.cursor_col = 0;
}

static void putc_raw(char c) {
    /* Before console_init() there is no cell buffer to render into.  The serial
     * mirror already happened in console_putc()/console_putc_raw(), so there is
     * nothing left to do here -- importantly, this must not touch the port I/O
     * again or every early character would be printed twice. */
    if (con.cells == NULL || con.cols == 0 || con.rows == 0)
        return;

    clear_cursor();
    switch (c) {
    case '\n':
        newline();
        break;
    case '\r':
        con.cursor_col = 0;
        break;
    case '\t': {
        uint32_t next = (con.cursor_col + 8) & ~7u;
        while (con.cursor_col < next && con.cursor_col < con.cols) {
            cell_put(con.cursor_row, con.cursor_col, ' ', pack_attr(con.cur));
            draw_cell(con.cursor_row, con.cursor_col);
            con.cursor_col++;
        }
        if (con.cursor_col >= con.cols) {
            con.cursor_col = 0;
            newline();
        }
        break;
    }
    case '\b':
        if (con.cursor_col > 0)
            con.cursor_col--;
        break;
    case 0x07: /* bell */
        break;
    default:
        if ((unsigned char)c < 0x20)
            break;
        cell_put(con.cursor_row, con.cursor_col, (uint8_t)c,
                 pack_attr(con.cur));
        /* Draw the cell just written.
         *
         * This is the character this function exists to print, and nothing else was ever
         * going to draw it: it used to appear as a side effect of `clear_cursor()`
         * repainting the cursor's entire row before every character.  That dependency was
         * invisible in the code and fatal the moment the repaint was narrowed to the one
         * cell the cursor was hiding -- the screen went blank, because with the repaint
         * gone nothing drew anything at all.  A character costs one glyph blit, which is
         * what a character costs. */
        draw_cell(con.cursor_row, con.cursor_col);
        con.cursor_col++;
        if (con.cursor_col >= con.cols) {
            con.cursor_col = 0;
            newline();
        }
        break;
    }
    draw_cursor();
}

static void putc_plain(char c) {
    putc_raw(c);
}

static void putc_escape(char c) {
    switch (esc_state) {
    case ESC_NONE:
        if (c == 0x1b) {
            esc_state = ESC_START;
            return;
        }
        putc_raw(c);
        return;

    case ESC_START:
        switch (c) {
        case '[':
            esc_state = ESC_CSI;
            csi_len = 0;
            return;
        case ']':
            esc_state = ESC_OSC;
            csi_len = 0;
            return;
        case 'c':
            console_clear();
            sgr_reset();
            esc_state = ESC_NONE;
            return;
        case '(':
        case ')':
        case '#':
            /* Character set selection: consume the following byte. */
            esc_state = ESC_OSC;
            csi_len = 0;
            return;
        default:
            esc_state = ESC_NONE;
            return;
        }

    case ESC_CSI:
        if ((c >= '0' && c <= '9') || c == ';' || c == ':' || c == '?' ||
            c == '>' || c == '<') {
            if (csi_len < sizeof(csi_buf) - 1)
                csi_buf[csi_len++] = c;
            return;
        }
        if (csi_len < sizeof(csi_buf))
            csi_buf[csi_len++] = c;
        parse_csi();
        esc_state = ESC_NONE;
        return;

    case ESC_OSC:
        /* OSC strings (window titles) end with BEL or ST (ESC \). */
        if (c == 0x07) {
            esc_state = ESC_NONE;
            return;
        }
        if (c == 0x1b) {
            esc_state = ESC_OSC_ESC;
            return;
        }
        return;

    case ESC_OSC_ESC:
        esc_state = ESC_NONE;
        return;

    default:
        esc_state = ESC_NONE;
        return;
    }
}

void console_init(void) {
    sgr_reset();

    const fb_info_t *info = fb_get_info();
    con.fb_ok = info->ready;

    if (!con.fb_ok) {
        /* Serial-only fallback: one very wide line, no rendering. */
        con.cols = 256;
        con.rows = 1;
        con.font_scale = 1;
        con.line_pixels = 1;
        con.top = 0;
        con.bottom = 0;
        con.enabled = true;
        con.cursor_visible = false;
        return;
    }

    /* The console always uses the font's native size.  Scaling it up wastes most
     * of a high-resolution framebuffer on a handful of very large cells, and
     * the whole point of a graphical console is room to work in. */
    con.font_scale = 1;

    con.line_pixels = FONT_H * con.font_scale;
    con.cols = info->width / (FONT_W * con.font_scale);
    con.rows = info->height / con.line_pixels;
    if (con.cols < 20)
        con.cols = 20;
    if (con.rows < 4)
        con.rows = 4;

    con.top = 0;
    con.bottom = con.rows - 1;

    size_t cells = (size_t)con.rows * con.cols;

    /* Statically sized for the biggest console this build can produce, and not
     * allocated.
     *
     * These were `kmalloc()`ed, in `console_init()`, which runs *before* the heap exists
     * -- the very next boot message is the heap announcing itself.  Whatever `kmalloc()`
     * handed back then sat in whatever that was, and the heap coming up underneath it is
     * not a thing that can be allowed to still be pointing at console text.  What it looked
     * like was a framebuffer that stopped updating part way through the boot log: the
     * characters written after that point went into a cell buffer the heap had taken over,
     * and every line drawn from it was blank, so the screen froze on the last line the
     * console still owned while the serial mirror -- which never touches this buffer --
     * carried on perfectly.
     *
     * Sixteen hundred pixels of cells and two bytes of attribute each, for the 256x64
     * console a 1920x1080 framebuffer at this font would give, is 40 KiB of `.bss`.  That
     * is the right trade for a buffer that has to outlive every allocator in the machine. */
    static uint8_t  cell_store[CONSOLE_MAX_CELLS];
    static uint16_t attr_store[CONSOLE_MAX_CELLS];

    if (cells > CONSOLE_MAX_CELLS) {
        panic("console: %zu cells exceeds the %d this build has room for",
              cells, (int)CONSOLE_MAX_CELLS);
    }

    con.cells = cell_store;
    con.attrs = attr_store;

    /* Start every cell on a space.  Leaving them at zero would render glyph 0
     * -- the CP437 smiley -- everywhere the text does not reach. */
    memset(con.cells, ' ', cells);
    memset(con.attrs, 0, cells * sizeof(uint16_t));

    con.cursor_row = 0;
    con.cursor_col = 0;
    con.cursor_visible = true;
    con.enabled = true;
    esc_state = ESC_NONE;

    fb_clear(CONSOLE_DEFAULT_BG);

    for (uint32_t row = 0; row < con.rows; row++)
        redraw_row(row);
    draw_cursor();

}

bool console_enabled(void) {
    return con.enabled;
}

void console_set_enabled(bool enabled) {
    con.enabled = enabled;
}

uint32_t console_rows(void) {
    return con.rows;
}

uint32_t console_cols(void) {
    return con.cols;
}

void console_get_cursor(uint32_t *row, uint32_t *col) {
    if (row)
        *row = con.cursor_row;
    if (col)
        *col = con.cursor_col;
}

void console_set_cursor(uint32_t row, uint32_t col) {
    clear_cursor();
    move_cursor(row, col);
}

void console_set_cursor_visible(bool visible) {
    clear_cursor();
    con.cursor_visible = visible;
    draw_cursor();
}

void console_set_color(console_color_t color) {
    con.cur = color;
}

console_color_t console_get_color(void) {
    return con.cur;
}

void console_clear(void) {
    clear_cursor();
    for (uint32_t row = 0; row < con.rows; row++) {
        for (uint32_t col = 0; col < con.cols; col++) {
            uint16_t attr = pack_attr(con.cur);
            cell_put(row, col, ' ', attr);
        }
        if (con.fb_ok)
            redraw_row(row);
    }
    con.cursor_row = 0;
    con.cursor_col = 0;
    draw_cursor();
}

void console_putc(char c) {
    serial_putc(c);
    putc_escape(c);
}

/* The console's write lock.
 *
 * The console is one buffer with four processors writing into it, and a write is a
 * *sequence* of characters rather than a character.  Without a lock, two writers interleave
 * inside each other and the boot log gets lines like "iinnitte:r rsutaprttsin egn,apbild
 * e" -- half a word from a user program's `write` and half a line from the kernel's own
 * output.  That is cosmetic in the sense that nothing is wrong, and it is not cosmetic in
 * the sense that it makes the serial log useless as evidence: a message split in half is
 * not findable by `grep`, and every phase of this kernel has been verified by reading that
 * log.  "The line is there somewhere" is not the same as "the log says it".
 *
 * The lock is *not* re-entrant, and making it so would be the wrong answer twice over.  A
 * re-entrant lock needs per-processor state to count depth, and that state is indexed by
 * the same `%gs:0` this function is reached through -- so a mistake in the nesting turns a
 * cosmetic bug into memory corruption, which is a bad trade for a boot log.  What nesting
 * would need it is also not there: `kprintf` holds the lock across format-and-write and then
 * calls `console_write_locked()`, not `console_write()`.
 *
 * Interrupts are off across the whole thing.  An interrupt that arrives while the lock is
 * held and tries to print would deadlock against the code it interrupted, and that is not a
 * rare event: the timer fires every millisecond. */
static ticket_lock_t out_lock;
static bool          out_lock_ready;
static uint64_t      out_lock_flags[MAX_CPU_COUNT];

/* Which slot to keep this processor's saved interrupt flags in.
 *
 * `percpu_self_index()` answers the boot processor's slot when GS has not been installed
 * yet, which is the case for the first lines the kernel prints.  That answer is in range,
 * so the clamp below is a backstop rather than the usual path -- but indexing an array with
 * an out-of-range processor index would write past the end of it into whatever static
 * happens to be next, which on this build is the lock itself, and a lock that corrupts
 * itself when it is contended is worse than no lock at all. */
static unsigned lock_flags_slot(void) {
    unsigned idx = percpu_self_index();

    return idx < MAX_CPU_COUNT ? idx : 0;
}

void console_lock(void) {
    /* Before the lock exists there is nothing to take and nothing to deadlock against
     * either, because nothing else is running: the first characters this kernel ever prints
     * are its own, on one processor, before the application processors are up.  Taking a
     * lock `ticket_lock_init()` has never seen is a different thing, and it is how the first
     * line of the boot log becomes the one line that is never printed. */
    if (!out_lock_ready)
        return;

    out_lock_flags[lock_flags_slot()] = spin_lock_irqsave(&out_lock);
}

void console_unlock(void) {
    if (!out_lock_ready)
        return;

    spin_unlock_irqrestore(&out_lock, out_lock_flags[lock_flags_slot()]);
}

/* Bring the write lock up.  Called once, from kmain(), before anything can print from more
 * than one processor. */
void console_lock_init(void) {
    ticket_lock_init(&out_lock);
    out_lock_ready = true;
}

void console_write(const char *data, size_t len) {
    console_lock();

    for (size_t i = 0; i < len; i++)
        console_putc(data[i]);

    console_unlock();
}

/* The same write with the lock already held, for a caller that is holding it -- `kprintf`
 * formats a whole line into a buffer and then writes it, and taking the lock around each
 * `console_write` separately would let another writer in between the format and the write,
 * which is the same interleaving in a different place. */
void console_write_locked(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++)
        console_putc(data[i]);
}

void console_puts(const char *s) {
    console_write(s, strlen(s));
}

void console_write_attr(const char *data, size_t len, console_color_t color) {
    console_color_t saved;
    size_t i;

    /* The current colour is shared state, so this has to hold the lock for the whole run,
     * not per character: two writers each saving and restoring `con.cur` around their own
     * characters would hand each other the wrong colour. */
    console_lock();

    saved = con.cur;
    con.cur = color;
    for (i = 0; i < len; i++)
        console_putc(data[i]);
    con.cur = saved;

    console_unlock();
}

/* Used by the panic path, which must never recurse into the escape parser. */
void console_putc_raw(char c) {
    serial_putc(c);
    putc_plain(c);
}

/* Deliberately *not* locked.
 *
 * This is the path a fault or a panic prints through, and it is the one place where being
 * unable to print is worse than printing untidily.  A lock here would deadlock against
 * whichever processor happens to hold it -- including the processor that faulted *inside* a
 * console write, which is not a hypothetical: `console_putc()` touches the framebuffer,
 * and a console write is exactly the kind of code that ends up taking a page fault.  So
 * this path may interleave with another writer's line.  That is the right way round: the
 * reader of a panic message needs the message, not the alignment. */
void console_raw_write(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++)
        console_putc_raw(data[i]);
}