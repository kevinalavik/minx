/* minx - bootloader handover and kernel start-up.
 *
 * Limine fills in the `.response` pointers of the request structures below and
 * then calls kmain().  Everything the kernel needs from the firmware (memory
 * map, framebuffer, RSDP, SMP info, the initrd module) is read straight out of
 * those responses.
 */
#include "boot.h"

#include <limine.h>

#include "console.h"
#include "cpu.h"
#include "delay.h"
#include "fb.h"
#include "io.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "panic.h"
#include "serial.h"
#include "string.h"
#include "test.h"
#include "types.h"

/* Size of the boot CPU's kernel stack.  Generous, because deep call chains in
 * the filesystem and the scheduler need the headroom. */
#define BOOT_STACK_SIZE (64u * 1024u)

/* --- Limine requests ---------------------------------------------------- */

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests_start_marker")))
static volatile uint64_t limine_requests_start_marker[] =
    LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile struct limine_entry_point_request entry_point_request = {
    .id = LIMINE_ENTRY_POINT_REQUEST_ID,
    .revision = 0,
    .response = NULL,
    .entry = kmain,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0,
    .response = NULL,
    .flags = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests_end_marker")))
static volatile uint64_t limine_requests_end_marker[] =
    LIMINE_REQUESTS_END_MARKER;

/* --- Handover record --------------------------------------------------- */

/* Filled in from the Limine responses and read by later phases; see boot.h. */
boot_info_t boot_info;




/* The kernel command line, e.g. "minx.quiet=1 minx.test=1". */
static char kernel_cmdline[512];
boot_cmdline_t boot_cmdline = { kernel_cmdline, 0, false, false, { 0 }, 0 };

/* --- helpers ------------------------------------------------------------ */

int atoi_simple(const char *s) {
    char *end = NULL;
    long v = (long)strtoull(s, &end, 10);
    return (int)v;
}

static void framebuffer_start(void) {
    struct limine_framebuffer_response *resp = framebuffer_request.response;
    if (resp == NULL || resp->framebuffer_count < 1) {
        kprintf("[warn] no framebuffer: the graphical console is unavailable\n");
        return;
    }

    struct limine_framebuffer *fb = resp->framebuffers[0];
    if (fb->memory_model != LIMINE_FRAMEBUFFER_RGB) {
        kprintf("[warn] framebuffer is not RGB (model %u); "
                "using the serial console only\n", fb->memory_model);
        return;
    }

    if (fb_init((uint8_t *)fb->address, (uint32_t)fb->width,
                (uint32_t)fb->height, (uint32_t)fb->pitch, fb->bpp, true,
                fb->red_mask_size, fb->red_mask_shift, fb->green_mask_size,
                fb->green_mask_shift, fb->blue_mask_size,
                fb->blue_mask_shift) != 0) {
        kprintf("[warn] framebuffer %ux%u @ %u bpp is not supported\n",
                (uint32_t)fb->width, (uint32_t)fb->height, fb->bpp);
        return;
    }

    boot_info.framebuffer_addr = (uint64_t)fb->address;
    boot_info.framebuffer_width = (uint32_t)fb->width;
    boot_info.framebuffer_height = (uint32_t)fb->height;
    boot_info.framebuffer_pitch = (uint32_t)fb->pitch;
    boot_info.framebuffer_bpp = fb->bpp;
}

static void parse_cmdline(void) {
    struct limine_executable_cmdline_response *resp = cmdline_request.response;
    const char *src = resp != NULL && resp->cmdline != NULL ? resp->cmdline : "";

    strlcpy(kernel_cmdline, src, sizeof(kernel_cmdline));
    boot_cmdline.len = (uint32_t)strlen(kernel_cmdline);

    /* Very small option lookup: "key" or "key=value", whitespace separated. */
    const char *p = kernel_cmdline;
    while (*p) {
        while (*p == ' ')
            p++;
        const char *start = p;
        while (*p && *p != ' ')
            p++;
        size_t len = (size_t)(p - start);

        static const char *const known[] = {
            "minx.quiet", "minx.test", "minx.init", "minx.loglevel",
        };
        for (size_t i = 0; i < array_len(known); i++) {
            size_t klen = strlen(known[i]);
            if (len >= klen && strncmp(start, known[i], klen) == 0 &&
                (len == klen || start[klen] == '=')) {
                switch (i) {
                case 0: boot_cmdline.quiet = (len == klen || start[klen + 1] == '1'); break;
                case 1: boot_cmdline.test = (len == klen || start[klen + 1] == '1'); break;
                case 2:
                    strlcpy(boot_cmdline.init, start + klen + 1,
                            sizeof(boot_cmdline.init));
                    break;
                case 3:
                    boot_cmdline.loglevel = atoi_simple(start + klen + 1);
                    break;
                default: break;
                }
            }
        }
    }
}

static void print_banner(const struct limine_bootloader_info_response *info) {
    kprintf("\n");
    kprintf("minx - a Unix-like OS for x86-64\n");
    kprintf("booted by %s %s\n",
            info != NULL && info->name ? info->name : "unknown",
            info != NULL && info->version ? info->version : "");
    kprintf("kernel image %016lx-%016lx\n", (uint64_t)__kernel_start,
            (uint64_t)__kernel_end);
}

static void report_memmap(const struct limine_memmap_response *resp) {
    static const char *const names[] = {
        "usable",     "reserved",        "ACPI reclaimable", "ACPI NVS",
        "bad memory", "bootloader",      "kernel + modules", "framebuffer",
        "reserved(mapped)",
    };

    boot_info.memmap_entries = (uint32_t)resp->entry_count;

    kprintf("memory map (%lu entries):\n", resp->entry_count);
    for (uint64_t i = 0; i < resp->entry_count; i++) {
        struct limine_memmap_entry *e = resp->entries[i];
        const char *name = e->type < array_len(names) ? names[e->type] : "?";
        kprintf("  %016lx-%016lx  %s\n", e->base, e->base + e->length, name);
    }
}

static void report_initrd(const struct limine_module_response *resp) {
    if (resp == NULL || resp->module_count < 1) {
        kprintf("[warn] no initrd module was loaded\n");
        return;
    }
    struct limine_file *m = resp->modules[0];
    boot_info.initrd_base = (uint64_t)m->address;
    boot_info.initrd_size = m->size;
    kprintf("initrd: %s, %lu bytes at %p\n", m->path ? m->path : "?",
            m->size, m->address);
}

/* Runs on the kernel's own stack; kmain() switches to it and never returns. */
static void kmain_stage2(void) {
    print_banner(bootloader_info_request.response);

    /* The bootloader zeroes limine_base_revision[2] when it understands the
     * revision the kernel asked for, so "supported" means it reads zero. */
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        panic("the bootloader speaks a newer Limine base revision (%lu) "
              "than this kernel supports",
              limine_base_revision[2]);
    }

    if (entry_point_request.response == NULL)
        panic("the bootloader did not report entry point support");

    if (memmap_request.response != NULL)
        report_memmap(memmap_request.response);
    else
        kprintf("[warn] no memory map response\n");

    if (rsdp_request.response != NULL) {
        boot_info.rsdp = rsdp_request.response->address;
        boot_info.rsdp_phys = virt_to_phys(boot_info.rsdp);
        kprintf("RSDP at %p (physical %#lx)\n", boot_info.rsdp, boot_info.rsdp_phys);
    } else {
        kprintf("[warn] no RSDP: ACPI will not be available\n");
    }

    if (mp_request.response != NULL) {
        struct limine_mp_response *mp = mp_request.response;
        boot_info.cpu_count = (uint32_t)mp->cpu_count;
        boot_info.bsp_lapic_id = mp->bsp_lapic_id;
        kprintf("SMP: %lu CPUs, BSP LAPIC id %u\n", mp->cpu_count,
                mp->bsp_lapic_id);
    } else {
        boot_info.cpu_count = 1;
        kprintf("SMP: single CPU (no MP response)\n");
    }

    report_initrd(module_request.response);

    const fb_info_t *fbi = fb_get_info();
    if (fbi->ready) {
        kprintf("console: framebuffer %ux%u, %u bpp; text %ux%u cells\n",
                fbi->width, fbi->height, fbi->bpp, console_cols(),
                console_rows());
    } else {
        kprintf("console: serial only (no framebuffer)\n");
    }

    kprintf("kernel command line: \"%s\"\n", boot_cmdline.str);
    kprintf("heap: %lu KiB in %u region(s), %lu KiB free\n",
            (unsigned long)(kmalloc_total_bytes() / 1024),
            kmalloc_region_count(),
            (unsigned long)(kmalloc_free_bytes() / 1024));
    kprintf("tsc: %lu MHz, %s\n",
            (unsigned long)(tsc_frequency() / 1000000),
            tsc_is_invariant() ? "invariant" : "not invariant");

    kprintf("boot complete: graphical console live, serial mirrored\n");

    if (boot_cmdline.test) {
        test_run_all();
        test_linger_and_exit();
    }

    /* Phase 2 onwards continues here.  Until then, park the CPU so the
     * graphical console and the serial mirror stay on screen. */
    khang();
}

/* --- entry -------------------------------------------------------------- */

void kmain(void) {
    /* Descriptor tables first: from here on a fault is reportable with a full
     * register dump instead of silently triple-faulting the machine. */
    gdt_init();
    idt_init();

    serial_init();
    kmalloc_init();
    parse_cmdline();

    /* The framebuffer console has to come up before anything is printed so
     * that the graphical console and the serial mirror never disagree. */
    framebuffer_start();
    console_init();

    /* Hand the boot CPU a stack that lives in the kernel's own heap. */
    void *stack = kmalloc(BOOT_STACK_SIZE);
    if (stack == NULL)
        panic("out of memory allocating the boot stack");

    kstack_enter((uint8_t *)stack + BOOT_STACK_SIZE, kmain_stage2);
}