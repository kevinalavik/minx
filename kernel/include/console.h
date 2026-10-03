/* minx - the kernel console.
 *
 * A text-mode terminal implemented on top of the framebuffer, mirroring every
 * byte to COM1.  It understands enough of ANSI/VT100 to handle colours, cursor
 * movement, erase and scrolling regions, which is what `less`, `vi` and the
 * shell's line editor need.
 *
 * Everything the kernel prints goes through here, so the graphical console and
 * the serial port can never disagree.
 */
#ifndef MINX_CONSOLE_H
#define MINX_CONSOLE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CONSOLE_ATTR_NORMAL = 0,
    CONSOLE_ATTR_BOLD   = 1u << 0,
    CONSOLE_ATTR_DIM    = 1u << 1,
    CONSOLE_ATTR_ITALIC = 1u << 2,
    CONSOLE_ATTR_UNDER  = 1u << 3,
    CONSOLE_ATTR_BLINK  = 1u << 4,
    CONSOLE_ATTR_INVERSE = 1u << 5,
} console_attr_t;

typedef struct {
    uint32_t fg;
    uint32_t bg;
    uint8_t  attr;
} console_color_t;

void console_init(void);
void console_putc(char c);
void console_write(const char *data, size_t len);
void console_puts(const char *s);

/* Escape-sequence-free output for the panic path, which must not re-enter the
 * ANSI parser while it is printing the reason for the panic. */
void console_putc_raw(char c);
void console_raw_write(const char *data, size_t len);

/* Attribute-aware output: honour SGR sequences instead of printing them. */
void console_write_attr(const char *data, size_t len, console_color_t color);

void console_clear(void);
void console_set_cursor(uint32_t row, uint32_t col);
void console_get_cursor(uint32_t *row, uint32_t *col);
uint32_t console_rows(void);
uint32_t console_cols(void);

void console_set_color(console_color_t color);
console_color_t console_get_color(void);
void console_set_cursor_visible(bool visible);

bool console_enabled(void);
void console_set_enabled(bool enabled);

#endif /* MINX_CONSOLE_H */