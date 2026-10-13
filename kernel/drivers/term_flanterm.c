/* minx - the graphical console, implemented with flanterm.
 *
 * An adapter, and nothing else.  Everything else in the kernel prints through `console.h`
 * and does not know or care what draws it; this file is the only thing that knows the
 * console is flanterm.  Swapping it back is `make TERMINAL=own`, and swapping it for
 * anything else means writing one more file against the same header.
 *
 * Why flanterm rather than the console in `console.c`: that one grew a terminal emulator by
 * accident, and it showed.  Three separate bugs lived in it -- a whole row of glyphs
 * repainted for every character typed, a whole screen of glyphs repainted for every line
 * scrolled, and a cell buffer allocated before the heap existed that the heap then took
 * over.  The last one is the one that cannot be fixed by being careful: `console_init()`
 * runs before there is a heap, so anything it allocates is at the mercy of one that did not
 * exist yet.  A terminal emulator that was written for this job, against this bootloader,
 * does not have that problem because it is not the thing that decides.
 *
 * It also knows things this console did not and the shell needs: a scrollback buffer, the
 * alternate screen, tabs, and the rest of the CSI set -- so `less`, `vi` and a line editor
 * have something to talk to.
 *
 * Licensing, version and why it was chosen are in docs/THIRD_PARTY.md.
 */
#include "console.h"

#include "fb.h"
#include "kprintf.h"
#include "panic.h"
#include "percpu.h"
#include "serial.h"
#include "spinlock.h"
#include "string.h"
#include "types.h"

/* The context struct -- rows, columns, where the cursor is -- lives in flanterm's private
 * header, which the public one includes only for flanterm's own sources.  Asking for it is
 * what this define does, and it is the documented way in rather than a copy of the layout
 * pasted here: a copy would be right until the next release changed a field, and would be
 * wrong silently. */
#define FLANTERM_IN_FLANTERM 1

#include <flanterm.h>
#include <flanterm_backends/fb.h>

/* The version, from the Makefile's pin, so there is one place that says which flanterm this
 * is and the console says the same thing. */
#ifndef FLANTERM_PINNED_VERSION
#define FLANTERM_PINNED_VERSION "unknown"
#endif

/* The console font, generated from lat9-16.psf by tools/psf2c.py.
 *
 * One byte per row, eight pixels wide, sixteen rows: the VGA font layout, which is exactly
 * what flanterm's framebuffer backend reads.  `g_font8x16` is generated, so there is no
 * third party's bytes in this file and no licence question about the font itself -- the
 * PSF it came from is the Linux console font, from kbd, and is recorded there. */
extern const uint8_t g_font8x16[];
#define FONT_WIDTH   8u
#define FONT_HEIGHT  16u

static struct flanterm_context *ft;

/* What `console_enabled()` reports and `console_set_enabled()` sets.
 *
 * Kept separately from flanterm's own notion because they are not the same question: this
 * one is "should anything be drawn at all", which is what a headless run wants, and it is
 * answered before the terminal exists as much as after. */
static bool enabled = true;

/* The colour `console_set_color()` last set, so `console_get_color()` can answer and so
 * `console_write_attr()` has something to apply.  flanterm owns the real SGR state. */
static console_color_t attr;

/* --- the lock ---------------------------------------------------------------------- */

/* The same lock `console.c` uses, and for the same reason: a write is a sequence of
 * characters, not a character, and two processors writing a line each must not end up
 * with their halves interleaved.  The serial mirror is inside the lock too, so the port
 * and the screen cannot disagree about what was said.
 *
 * Not re-entrant, deliberately, and for the reason in `console.h`: `kprintf` holds it
 * across format-and-write and then calls `console_write_locked()`, not `console_write()`. */
static ticket_lock_t out_lock;
static bool          out_lock_ready;
static uint64_t      out_lock_flags[MAX_CPU_COUNT];

static unsigned lock_flags_slot(void) {
    unsigned idx = percpu_self_index();

    return idx < MAX_CPU_COUNT ? idx : 0;
}

void console_lock(void) {
    if (!out_lock_ready)
        return;

    out_lock_flags[lock_flags_slot()] = spin_lock_irqsave(&out_lock);
}

void console_unlock(void) {
    if (!out_lock_ready)
        return;

    spin_unlock_irqrestore(&out_lock, out_lock_flags[lock_flags_slot()]);
}

void console_lock_init(void) {
    ticket_lock_init(&out_lock);
    out_lock_ready = true;
}

/* --- output ------------------------------------------------------------------------ */

/* To both, in that order and under one lock, which is the whole point of this function:
 * the framebuffer and COM1 are the same console, and a program that can read one and not
 * the other is debugging two different machines.
 *
 * The serial mirror goes out in one `serial_write()`, which is what that call is for.  It
 * used to be a `serial_putc()` per byte, and that is a port write per byte: for a screenful
 * of boot log that is several thousand port writes to mirror text that is already on the
 * framebuffer, and it was slow enough to be felt while scrolling.  The framebuffer is the
 * expensive half of this and always was -- but the mirror should not be adding to it. */
static void emit_locked(const char *data, size_t len) {
    serial_write(data, len);

    if (ft != NULL && enabled) {
        /* With the carriage return put back.
         *
         * A terminal's LF moves down a line and leaves the column alone; CR returns to
         * column zero and leaves the row alone.  Every kernel message and every program's
         * `printf` ends its lines with a bare "\n" -- which is right, because a newline is
         * a newline in a byte stream and it is the *terminal* that decides what it means.
         * A terminal that treats it as LF alone moves down one column further for every
         * line, so the text walks diagonally down the screen and each line starts where the
         * one above it ended.  That is what this looked like, and it is the same bug the
         * old console did not have because it did not implement LF as a separate motion at
         * all.
         *
         * So this is the `ONLCR` bit of a tty driver's output processing, which is exactly
         * what it is: the terminal emulator is doing the translating that a real tty line
         * discipline would otherwise have done for it.  Done here, on the way in, rather
         * than by asking every caller to write "\r\n", because the callers are a kernel
         * log and every program in the tree and neither of them should have to know.
         *
         * The serial port does *not* get the carriage return: a terminal emulator on the
         * other end of a serial line does the same translating, and sending it would make
         * the log stair-step on anything reading it as a dumb port. */
        size_t start = 0;

        for (size_t i = 0; i < len; i++) {
            if (data[i] != '\n')
                continue;

            if (i > start)
                flanterm_write(ft, data + start, i - start);

            flanterm_write(ft, "\r\n", 2);
            start = i + 1;
        }

        if (start < len)
            flanterm_write(ft, data + start, len - start);


        /* Render what was queued, now.
         *
         * flanterm does not draw on the write: it marks the cells that changed dirty and
         * leaves drawing them to `flanterm_flush()`, normally called from a timer.  This
         * kernel's console has no timer and no other event that would ever call it, so
         * nothing would be drawn after the first flush.
         *
         * Once per `console_write()`, not once per character: a boot log is thousands of
         * `kprintf()` calls, and a flush per call is a redraw per line. */
        flanterm_flush(ft);
    }
}

void console_write_locked(const char *data, size_t len) {
    emit_locked(data, len);
}

void console_write(const char *data, size_t len) {
    console_lock();
    emit_locked(data, len);
    console_unlock();
}

void console_putc(char c) {
    char one = c;

    console_lock();
    emit_locked(&one, 1);
    console_unlock();
}

void console_puts(const char *s) {
    console_write(s, strlen(s));
}

void console_putc_raw(char c) {
    char one = c;

    /* Deliberately unlocked: this is what the panic path prints with, and a panic that
     * cannot get past a lock held by the code it is reporting on is a panic that prints
     * nothing.  The raw path also skips flanterm, because the panic path is printing the
     * reason the machine is in trouble and re-entering the terminal that is in trouble is
     * how it stops being able to print at all. */
    serial_putc(c);

    if (ft != NULL && enabled) {
        /* Same ONLCR translating as `emit_locked()`; see the comment there. */
        flanterm_write(ft, c == '\n' ? "\r\n" : &one, c == '\n' ? 2u : 1u);
        flanterm_flush(ft);
    }
}

void console_raw_write(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++)
        serial_putc(data[i]);
}

/* --- colours ----------------------------------------------------------------------- */

void console_write_attr(const char *data, size_t len, console_color_t color) {
    console_set_color(color);
    console_write(data, len);
}

void console_set_color(console_color_t color) {
    /* 64, not 32.
     *
     * The sequence is `\033[0;38;2;r;g;b;48;2;r;g;b;m`: twenty-seven literal bytes and
     * eighteen digits at worst, which is forty-five characters and forty-six with the NUL.
     * In a 32 byte array the `m` landed on the NUL and the caller was handed the length the
     * formatter *would* have written -- so the terminal received the sequence with a NUL
     * where its terminator belonged, six bytes of the caller's stack, and a serial log that
     * stopped being text.  The size is computed rather than guessed, so a future edit to
     * the format cannot quietly make the buffer too small again. */
    char seq[64];
    int  n;

    attr = color;

    /* Push it into flanterm as the SGR that produces it.
     *
     * The colour this kernel uses is 0x00RRGGBB and flanterm's is the framebuffer's own
     * layout, so the two components are computed from the same numbers rather than assumed
     * to agree.  A caller that wants the *terminal's* idea of a colour -- which is what
     * `console_write_attr()` is for -- goes through here, so there is one place where the
     * two encodings meet. */
    n = ksnprintf(seq, sizeof(seq), "\033[0;38;2;%lu;%lu;%lu;48;2;%lu;%lu;%lu;m",
                 (unsigned long)((color.fg >> 16) & 0xff),
                 (unsigned long)((color.fg >> 8) & 0xff),
                 (unsigned long)(color.fg & 0xff),
                 (unsigned long)((color.bg >> 16) & 0xff),
                 (unsigned long)((color.bg >> 8) & 0xff),
                 (unsigned long)(color.bg & 0xff));

    if (n > 0)
        console_write(seq, (size_t)n);
}

/* One framebuffer-native pixel value as 0x00RRGGBB.
 *
 * flanterm's colours are whatever the framebuffer's channels are -- it writes them straight
 * into a pixel -- and this kernel's `console_color_t` is 0x00RRGGBB, so the two have to be
 * converted rather than compared.  Each channel is scaled by its own width, which is the
 * only part of this that is not mechanical: a 5-bit red and an 8-bit red are the same
 * colour and have to come out the same number. */
static uint32_t fb_native_to_rgb(uint32_t v) {
    const fb_info_t *info = fb_get_info();
    uint32_t r, g, b;

    if (info == NULL || info->r_size == 0)
        return v;

    r = (v >> info->r_shift) & ((1u << info->r_size) - 1u);
    g = (v >> info->g_shift) & ((1u << info->g_size) - 1u);
    b = (v >> info->b_shift) & ((1u << info->b_size) - 1u);

    r = (r * 255u) / ((1u << info->r_size) - 1u);
    g = (g * 255u) / ((1u << info->g_size) - 1u);
    b = (b * 255u) / ((1u << info->b_size) - 1u);

    return (r << 16) | (g << 8) | b;
}

console_color_t console_get_color(void) {
    console_color_t c = attr;

    /* Read the terminal's own state, not the copy kept here.
     *
     * The copy is only what `console_set_color()` last pushed, and an escape sequence
     * written straight to the console -- which is what every colour test in this tree
     * does, and what every program that colours its own output does -- changes the
     * terminal's state without going through this file at all.  A `console_get_color()`
     * that answered from the copy would report the colour from before the last SGR,
     * which is worse than not answering: it looks authoritative.
     *
     * Only once flanterm exists; before that the copy is the whole of what is known. */
    if (ft == NULL)
        return c;

    {
        /* The colours live in the *backend's* context, not the frontend's: the frontend
         * knows the current colour as an index into the palette, and it is the backend
         * that turns that index into pixels.  Asking the backend is therefore both the only
         * place the value exists and the place that holds it in this framebuffer's own
         * layout, which is what `fb_native_to_rgb()` takes. */
        struct flanterm_fb_context *fbctx = (void *)ft;

        c.fg = fb_native_to_rgb(fbctx->text_fg);
        c.bg = fb_native_to_rgb(fbctx->text_bg);
    }

    return c;
}

/* --- cursor and geometry ----------------------------------------------------------- */

void console_clear(void) {
    console_write("\033[2J\033[H", 7);
}

void console_set_cursor(uint32_t row, uint32_t col) {
    char seq[64];

    /* One-based on the wire, as every terminal has always been. */
    int n = ksnprintf(seq, sizeof(seq), "\033[%lu;%luH", (unsigned long)row + 1, (unsigned long)col + 1);

    if (n > 0)
        console_write(seq, (size_t)n);
}

void console_get_cursor(uint32_t *row, uint32_t *col) {
    if (row != NULL)
        *row = 0;
    if (col != NULL)
        *col = 0;

    if (ft == NULL || ft->get_cursor_pos == NULL)
        return;

    /* Asked of the terminal rather than kept here, because this is the only place that
     * genuinely knows: a program that moves the cursor with an escape sequence of its own
     * moves it for real, and a copy of the position kept alongside would be a copy that is
     * wrong the moment anything wrote one.  flanterm counts from zero, so there is nothing
     * to convert -- which is worth saying, because every other terminal counts rows from
     * one and this is where that would otherwise be got wrong. */
    size_t x = 0, y = 0;

    ft->get_cursor_pos(ft, &x, &y);
    *row = (uint32_t)y;
    *col = (uint32_t)x;
}

uint32_t console_rows(void) {
    return ft != NULL ? ft->rows : 0;
}

uint32_t console_cols(void) {
    return ft != NULL ? ft->cols : 0;
}

void console_set_cursor_visible(bool visible) {
    /* A plain field on the context, so this is an assignment and not a round trip through
     * the lock and an escape sequence -- and a null check, because the panic path calls
     * this before `console_init()` has run. */
    if (ft != NULL)
        ft->cursor_enabled = visible;
}

bool console_enabled(void) {
    return enabled;
}

void console_set_enabled(bool on) {
    enabled = on;
}

/* --- bring-up ---------------------------------------------------------------------- */

void console_init(void) {
    const fb_info_t *info = fb_get_info();

    if (info == NULL || !info->ready || info->base == NULL) {
        /* Serial-only.  Everything still prints to COM1; there is just nothing to draw,
         * and saying so once here is better than a terminal that silently eats it. */
        kprintf("console: no framebuffer; serial only\n");
        enabled = false;
        return;
    }

    /* The bump allocator is passed as NULL, NULL: flanterm carries one, and the console is
     * created once and never freed, so a heap for it would be a heap nobody can reuse. */
    ft = flanterm_fb_init(
        NULL, NULL,
        (uint32_t *)(uintptr_t)info->base, info->width, info->height, info->pitch,
        info->r_size, info->r_shift,
        info->g_size, info->g_shift,
        info->b_size, info->b_shift,
        NULL,
        NULL, NULL,                       /* the 16 ANSI colours: its defaults */
        NULL, NULL,
        NULL, NULL,
        (void *)(uintptr_t)g_font8x16, FONT_WIDTH, FONT_HEIGHT, 0,
        1, 1,                             /* this console always uses the font's own size */
        0,                                /* no margin */
        FLANTERM_FB_ROTATE_0);

    if (ft == NULL)
        panic("console: flanterm would not start on a %ux%u framebuffer\n",
              info->width, info->height);

    kprintf("console: flanterm %s on %lux%lu, text %lux%lu cells, font %lux%lu\n",
            FLANTERM_PINNED_VERSION, (unsigned long)info->width,
            (unsigned long)info->height, (unsigned long)ft->cols,
            (unsigned long)ft->rows, (unsigned long)FONT_WIDTH,
            (unsigned long)FONT_HEIGHT);
}