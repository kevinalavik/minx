#include "fb.h"

#include "string.h"
#include "types.h"

static fb_info_t fb;

/* Expand an 8-bit channel to the full 0..255 range, then shift it into place
 * and truncate to however many bits the hardware actually has. */
static inline uint32_t pack_channel(uint8_t value, uint8_t size, uint8_t shift) {
    if (size == 0)
        return 0;
    uint32_t v = value;
    if (size < 8)
        v = (v >> (8 - size)) & ((1u << size) - 1u);
    return v << shift;
}

static inline uint32_t pack_color(fb_color_t c) {
    uint8_t r = (uint8_t)((c >> 16) & 0xff);
    uint8_t g = (uint8_t)((c >> 8) & 0xff);
    uint8_t b = (uint8_t)(c & 0xff);
    return pack_channel(r, fb.r_size, fb.r_shift) |
           pack_channel(g, fb.g_size, fb.g_shift) |
           pack_channel(b, fb.b_size, fb.b_shift);
}

int fb_init(uint8_t *base, uint32_t width, uint32_t height, uint32_t pitch,
            uint16_t bpp, bool rgb, uint8_t r_size, uint8_t r_shift,
            uint8_t g_size, uint8_t g_shift, uint8_t b_size, uint8_t b_shift) {
    if (base == NULL || width == 0 || height == 0)
        return -1;
    if (bpp != 16 && bpp != 24 && bpp != 32)
        return -1;

    memzero(&fb, sizeof(fb));
    fb.base = base;
    fb.width = width;
    fb.height = height;
    fb.pitch = pitch;
    fb.bpp = (uint8_t)bpp;
    fb.rgb = rgb;
    fb.r_size = r_size;
    fb.r_shift = r_shift;
    fb.g_size = g_size;
    fb.g_shift = g_shift;
    fb.b_size = b_size;
    fb.b_shift = b_shift;
    fb.ready = true;
    return 0;
}

const fb_info_t *fb_get_info(void) {
    return &fb;
}

uint8_t *fb_framebuffer_address(void) {
    return fb.base;
}

void fb_fill(uint32_t y0, uint32_t x0, uint32_t y1, uint32_t x1, fb_color_t c) {
    if (!fb.ready)
        return;
    if (y0 >= fb.height || x0 >= fb.width)
        return;
    if (y1 > fb.height)
        y1 = fb.height;
    if (x1 > fb.width)
        x1 = fb.width;
    if (y1 <= y0 || x1 <= x0)
        return;

    uint32_t pixel = pack_color(c);
    uint32_t bytes_per_pixel = fb.bpp / 8u;

    for (uint32_t y = y0; y < y1; y++) {
        uint8_t *row = fb.base + (uint64_t)y * fb.pitch;
        if (bytes_per_pixel == 4) {
            uint32_t *p = (uint32_t *)row;
            for (uint32_t x = x0; x < x1; x++)
                p[x] = pixel;
        } else if (bytes_per_pixel == 2) {
            uint16_t *p = (uint16_t *)row;
            for (uint32_t x = x0; x < x1; x++)
                p[x] = (uint16_t)pixel;
        } else {
            /* 24 bpp: three bytes per pixel, no padding. */
            for (uint32_t x = x0; x < x1; x++) {
                uint8_t *p = row + (uint64_t)x * 3;
                p[0] = (uint8_t)(pixel & 0xff);
                p[1] = (uint8_t)((pixel >> 8) & 0xff);
                p[2] = (uint8_t)((pixel >> 16) & 0xff);
            }
        }
    }
}

void fb_clear(fb_color_t c) {
    fb_fill(0, 0, fb.height, fb.width, c);
}

void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  fb_color_t c) {
    if (w == 0 || h == 0)
        return;
    fb_fill(y, x, y + h, x + w, c);
}

void fb_scroll_up(uint32_t rows, uint32_t line_pixels) {
    if (!fb.ready || rows == 0 || rows >= fb.height)
        return;

    uint32_t src_y = rows * line_pixels;
    uint32_t dst_y = 0;
    uint32_t height = fb.height - src_y;
    uint32_t row_bytes = fb.width * (fb.bpp / 8u);

    uint8_t *dst = fb.base + (uint64_t)dst_y * fb.pitch;
    uint8_t *src = fb.base + (uint64_t)src_y * fb.pitch;

    if (src > dst && src < dst + row_bytes) {
        /* Overlapping moves must walk backwards. */
        for (uint32_t y = height; y-- > 0;) {
            memmove(dst + (uint64_t)y * fb.pitch,
                    src + (uint64_t)y * fb.pitch, row_bytes);
        }
    } else {
        for (uint32_t y = 0; y < height; y++) {
            memmove(dst + (uint64_t)y * fb.pitch,
                    src + (uint64_t)y * fb.pitch, row_bytes);
        }
    }
}

void fb_draw_glyph(uint32_t px, uint32_t py, const uint8_t *bitmap,
                   uint32_t glyph_w, uint32_t glyph_h, uint32_t bytes_per_row,
                   fb_color_t fg, fb_color_t bg, bool transparent_bg,
                   uint32_t scale_x, uint32_t scale_y) {
    if (!fb.ready || bitmap == NULL)
        return;
    if (scale_x < 1)
        scale_x = 1;
    if (scale_y < 1)
        scale_y = 1;

    /* Pre-compute the packed pixel values once per glyph. */
    const uint32_t fg_px = pack_color(fg);
    const uint32_t bg_px = pack_color(bg);
    const uint32_t bytes_per_pixel = fb.bpp / 8u;

    for (uint32_t gy = 0; gy < glyph_h; gy++) {
        const uint8_t *row_bits = bitmap + (uint64_t)gy * bytes_per_row;
        uint32_t dst_y = py + gy * scale_y;
        if (dst_y >= fb.height)
            break;

        /* Skip rows that are entirely off the top or bottom of the screen. */
        if (dst_y + scale_y <= 0)
            continue;

        for (uint32_t gx = 0; gx < glyph_w; gx++) {
            bool on = (row_bits[gx >> 3] & (0x80u >> (gx & 7u))) != 0;
            if (!on && transparent_bg)
                continue;

            uint32_t dst_x = px + gx * scale_x;
            if (dst_x >= fb.width)
                break;

            uint32_t value = on ? fg_px : bg_px;
            uint8_t *out = fb.base + (uint64_t)dst_y * fb.pitch;

            for (uint32_t sy = 0; sy < scale_y; sy++) {
                uint32_t row = dst_y + sy;
                if (row >= fb.height)
                    break;
                out = fb.base + (uint64_t)row * fb.pitch;
                if (bytes_per_pixel == 4) {
                    uint32_t *p = (uint32_t *)out;
                    for (uint32_t sx = 0; sx < scale_x; sx++) {
                        uint32_t x = dst_x + sx;
                        if (x < fb.width)
                            p[x] = value;
                    }
                } else if (bytes_per_pixel == 2) {
                    uint16_t *p = (uint16_t *)out;
                    for (uint32_t sx = 0; sx < scale_x; sx++) {
                        uint32_t x = dst_x + sx;
                        if (x < fb.width)
                            p[x] = (uint16_t)value;
                    }
                } else {
                    for (uint32_t sx = 0; sx < scale_x; sx++) {
                        uint32_t x = dst_x + sx;
                        if (x >= fb.width)
                            break;
                        uint8_t *p = out + (uint64_t)x * 3;
                        p[0] = (uint8_t)(value & 0xff);
                        p[1] = (uint8_t)((value >> 8) & 0xff);
                        p[2] = (uint8_t)((value >> 16) & 0xff);
                    }
                }
            }
        }
    }
}