/* minx - raw framebuffer access.
 *
 * Thin wrapper over whatever pixel layout the bootloader handed us.  Nothing
 * above this layer knows about bpp or channel shifts; it asks for pixels.
 */
#ifndef MINX_FB_H
#define MINX_FB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A framebuffer in the 0x00RRGGBB form used throughout the console. */
typedef uint32_t fb_color_t;

typedef struct {
    uint8_t  *base;      /* virtual address of the pixel array      */
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;     /* bytes per row, >= width * bpp / 8      */
    uint8_t   bpp;
    bool      rgb;
    uint8_t   r_size, r_shift;
    uint8_t   g_size, g_shift;
    uint8_t   b_size, b_shift;
    bool      ready;
} fb_info_t;

int  fb_init(uint8_t *base, uint32_t width, uint32_t height, uint32_t pitch,
             uint16_t bpp, bool rgb, uint8_t r_size, uint8_t r_shift,
             uint8_t g_size, uint8_t g_shift, uint8_t b_size, uint8_t b_shift);
void fb_fill(uint32_t y0, uint32_t x0, uint32_t y1, uint32_t x1, fb_color_t c);
void fb_clear(fb_color_t c);
void fb_scroll_up(uint32_t rows, uint32_t line_pixels);
void fb_draw_glyph(uint32_t px, uint32_t py, const uint8_t *bitmap,
                   uint32_t glyph_w, uint32_t glyph_h, uint32_t bytes_per_row,
                   fb_color_t fg, fb_color_t bg, bool transparent_bg,
                   uint32_t scale_x, uint32_t scale_y);
void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  fb_color_t c);
const fb_info_t *fb_get_info(void);
uint8_t *fb_framebuffer_address(void);

#endif /* MINX_FB_H */