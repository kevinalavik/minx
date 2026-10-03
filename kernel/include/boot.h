/* minx - what the bootloader hands us, and where it lives afterwards. */
#ifndef MINX_BOOT_H
#define MINX_BOOT_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_CPU_COUNT 64

/* Parsed kernel command line.  minx accepts a handful of its own options on
 * top of whatever the bootloader passed through. */
typedef struct {
    const char *str;
    uint32_t    len;
    bool        quiet;     /* minx.quiet=1     suppress banner chatter     */
    bool        test;      /* minx.test=1      run the automated suite      */
    char        init[128]; /* minx.init=PATH   program to run as PID 1      */
    uint32_t    loglevel;  /* minx.loglevel=N  0 = everything               */
} boot_cmdline_t;

typedef struct {
    uint64_t framebuffer_addr;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_pitch;
    uint16_t framebuffer_bpp;

    uint64_t initrd_base;
    uint64_t initrd_size;

    void    *rsdp;
    uint64_t rsdp_phys;

    uint32_t cpu_count;
    uint32_t bsp_lapic_id;

    uint32_t memmap_entries;

    uint32_t acpi_version;   /* 0 = not parsed yet */
} boot_info_t;

extern boot_info_t boot_info;
extern boot_cmdline_t boot_cmdline;

extern char __kernel_start[];
extern char __kernel_end[];
extern char __bss_start[];
extern char __bss_end[];

/* Defined in kernel/boot/crt.S.  Switches to *top and jumps to *entry,
 * abandoning the current stack.  Never returns. */
void kstack_enter(void *top, void (*entry)(void)) __attribute__((noreturn));

/* Called by the bootloader once the request block has been answered. */
void kmain(void);

/* Small helpers shared by the boot code. */
int atoi_simple(const char *s);

#endif /* MINX_BOOT_H */