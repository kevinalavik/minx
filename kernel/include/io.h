/* minx - port I/O and small CPU helpers. */
#ifndef MINX_IO_H
#define MINX_IO_H

#include <stdbool.h>
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline void cpuid_raw(uint32_t leaf, uint32_t subleaf,
                             uint32_t *a, uint32_t *b, uint32_t *c,
                             uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(subleaf));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)val),
                     "d"((uint32_t)(val >> 32)));
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdtsc_ordered(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence\n\trdtsc"
                     : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpuid_serialise(void) {
    __asm__ volatile("mfence\n\tlfence" ::: "memory");
}

static inline void pause_cpu(void) {
    __asm__ volatile("pause" ::: "memory");
}

static inline void hlt(void) {
    __asm__ volatile("hlt");
}

static inline void cli(void) {
    __asm__ volatile("cli" ::: "memory");
}

static inline void sti(void) {
    __asm__ volatile("sti" ::: "memory");
}

static inline bool interrupts_enabled(void) {
    uint64_t flags;
    __asm__ volatile("pushfq\n\tpopq %0" : "=r"(flags));
    return (flags & 0x200) != 0;
}

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq\n\tpopq %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ volatile("pushq %0\n\tpopfq" :: "r"(flags) : "memory",
                     "cc");
}

#define MSR_EFER   0xc0000080
#define MSR_STAR   0xc0000081
#define MSR_LSTAR  0xc0000082
#define MSR_SFMASK 0xc0000084
#define MSR_FS_BASE 0xc0000100
#define MSR_GS_BASE 0xc0000101
#define MSR_KERNEL_GS_BASE 0xc0000102

/* Control registers only accept 64-bit moves in long mode, so these go through
 * a fixed register rather than a general output constraint. */
static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile("movq %%cr2, %%rax" : "=a"(v));
    return v;
}

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("movq %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint64_t v) {
    __asm__ volatile("movq %0, %%cr3" :: "r"(v) : "memory");
}

/* Access the per-CPU area.  Until the GS MSRs are programmed this reads
 * whatever the bootloader left there, so callers must not rely on it. */
static inline void *gs_base(void) {
    return (void *)(uint64_t)rdmsr(MSR_GS_BASE);
}

static inline void set_gs_base(void *base) {
    wrmsr(MSR_GS_BASE, (uint64_t)base);
}

#endif /* MINX_IO_H */