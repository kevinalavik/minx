#include <stdint.h>

#define COM1 0x3f8u
#define DEBUG_EXIT 0xf4u
#define LIMINE_COMMON_MAGIC 0xc7b1dd30df4c8b88ull, 0x0a82e883a194f07bull
#define LIMINE_BASE_REVISION { 0xf9562b2d5c95a6c8ull, 0x6a7b384944536bdcull, 3 }
#define LIMINE_CMDLINE_REQUEST_ID { LIMINE_COMMON_MAGIC, 0x4b161536e598651eull, 0xb390ad4a2f1f303aull }

struct limine_cmdline_response {
    uint64_t revision;
    char *cmdline;
};

struct limine_cmdline_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_cmdline_response *response;
};

static volatile uint64_t base_revision[3]
    __attribute__((used, section(".limine_requests"))) = LIMINE_BASE_REVISION;
static volatile struct limine_cmdline_request cmdline_request
    __attribute__((used, section(".limine_requests"))) = {
        .id = LIMINE_CMDLINE_REQUEST_ID,
        .revision = 0,
        .response = 0
    };

static volatile uint64_t requests_start[4]
    __attribute__((used, section(".limine_requests_start"))) = {
        0xf6b8f4b39de7d1aeull, 0xfab91a6940fcb9cfull,
        0x785c6ed015d3e316ull, 0x181e920a7852b9d9ull
    };
static volatile uint64_t requests_end[2]
    __attribute__((used, section(".limine_requests_end"))) = {
        0xadc0e0531bb10d03ull, 0x9572709f31764c62ull
    };

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xc7);
    outb(COM1 + 4, 0x0b);
}

static void serial_putc(char c) {
    while ((inb(COM1 + 5) & 0x20u) == 0) {
        __asm__ volatile("pause");
    }
    outb(COM1, (uint8_t)c);
}

static void serial_write(const char *text) {
    while (*text != '\0') {
        if (*text == '\n') {
            serial_putc('\r');
        }
        serial_putc(*text++);
    }
}

static void halt(void) __attribute__((noreturn));
static void halt(void) {
    __asm__ volatile("cli");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static void panic(const char *reason) __attribute__((noreturn));
static void panic(const char *reason) {
    serial_write("\nMINX: PANIC: ");
    serial_write(reason);
    serial_write("\n");
    halt();
}

void _start(void) __attribute__((noreturn));
void _start(void) {
    __asm__ volatile("cli");
    if (((volatile uint64_t *)base_revision)[2] != 0) {
        panic("unsupported Limine base revision");
    }
    serial_init();
    serial_write("MINX: phase 1 boot OK\n");

    if (cmdline_request.response != 0 && cmdline_request.response->cmdline != 0 &&
        cmdline_request.response->cmdline[0] != '\0') {
        serial_write("MINX: self-test PASS\n");
        outb(DEBUG_EXIT, 0x10);
        halt();
    }

    serial_write("MINX: kernel halted; next phase initializes CPU core\n");
    halt();
}
