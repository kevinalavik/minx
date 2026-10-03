/* minx - calibrated time delays.
 *
 * Busy-waiting on a raw rdtsc() delta needs the TSC frequency, which is not
 * fixed across the machines this kernel is expected to run on.  CPUID leaf
 * 0x15 gives the crystal ratio, leaf 0x16 gives the frequency directly, and KVM
 * exposes leaf 0x40000010.  Those leaves are optional and often report nothing
 * at all, so they are only ever a first guess: timer_init() measures the TSC
 * against the 8253 at boot and calls delay_set_tsc_hz() with the result.  Every
 * delay in the kernel is a TSC delta, so a guess that is wrong is a kernel whose
 * timeouts fire early rather than late, which is the harder direction to notice.
 */
#include "delay.h"

#include "cpu.h"
#include "io.h"
#include "kprintf.h"
#include "types.h"

static uint64_t tsc_hz;
static bool calibrated;

/* Raw CPUID values behind the detection, reported at boot so a wrong
 * calibration is visible rather than mysterious. */
static uint32_t reported_leaf15;
static uint32_t reported_kvm;
static uint32_t reported_cpuid16;

/* KVM's hypervisor leaf is only meaningful when the vendor string matches. */
#define KVM_SIGNATURE 0x4b564d45u   /* "KVMK" */

static uint64_t detect_tsc_hz(uint32_t *leaf15_ratio, uint32_t *kvm_hz,
                              uint32_t *cpuid16_hz) {
    uint32_t a, b, c, d;

    /* CPUID leaf 0x16: EAX = crystal clock in Hz, EBX = 0 when available. */
    cpuid_raw(0x16, 0, &a, &b, &c, &d);
    *cpuid16_hz = a;
    if (a != 0)
        return a;

    /* KVM reports the TSC frequency in its hypervisor leaf. */
    cpuid_raw(0x40000000, 0, &a, &b, &c, &d);
    if (b == KVM_SIGNATURE) {
        cpuid_raw(0x40000010, 0, &a, &b, &c, &d);
        *kvm_hz = a;
        if (a != 0)
            return a;
    } else {
        *kvm_hz = 0;
    }

    /* CPUID leaf 0x15: ECX = numerator, EBX:EAX = denominator, so the crystal
     * frequency is ECX * 1e9 / (EBX:EAX). */
    cpuid_raw(0x15, 0, &a, &b, &c, &d);
    *leaf15_ratio = c != 0 && b != 0 ? (c << 16) | a : 0;
    if (c != 0 && b != 0)
        return (uint64_t)c * 1000000000ull / (uint64_t)b;

    return 0;
}

void delay_init(void) {
    if (calibrated)
        return;

    uint32_t leaf15 = 0, kvm = 0, cpuid16 = 0;
    uint64_t hz = detect_tsc_hz(&leaf15, &kvm, &cpuid16);
    reported_leaf15 = leaf15;
    reported_kvm = kvm;
    reported_cpuid16 = cpuid16;
    if (hz == 0) {
        /* No frequency information from the firmware.  The guess is only a
         * placeholder: timer_init() replaces it with a measurement against the
         * 8253 before any delay is taken. */
        hz = 1000000000ull;
    }
    tsc_hz = hz;
    calibrated = true;
}

uint64_t tsc_frequency(void) {
    if (!calibrated)
        delay_init();
    return tsc_hz;
}

/* Replace the detected TSC frequency with a measured one.
 *
 * timer.c measures the TSC against the 8253 at boot and hands the result here.
 * The detected value stays available through delay_report(), because "the
 * firmware said 1 GHz and the hardware said otherwise" is worth being able to
 * see in the log. */
void delay_set_tsc_hz(uint64_t hz) {
    if (hz == 0)
        return;
    tsc_hz = hz;
    calibrated = true;
}

bool tsc_is_invariant(void) {
    uint32_t a, b, c, d;
    cpuid_raw(0x80000007, 0, &a, &b, &c, &d);
    return (d & (1u << 8)) != 0;
}

/* Reported at boot: a wrong TSC frequency silently skews every calibrated
 * interval, so the raw CPUID values are worth having in the log. */
void delay_report(void) {
    kprintf("tsc: %lu Hz detected, %s\n", (unsigned long)tsc_frequency(),
            tsc_is_invariant() ? "invariant TSC" : "TSC may vary");
    kprintf("tsc: cpuid.16=%u Hz kvm.0x40000010=%u Hz cpuid.15=%#x "
            "(all zero means the assumed fallback was used)\n",
            reported_cpuid16, reported_kvm, reported_leaf15);
}

/* rdtsc is not serialising by itself; without LFENCE the compiler and the CPU
 * are both free to move the read relative to the work being timed. */
static inline uint64_t tsc_serialised(void) {
    return rdtsc_ordered();
}

void delay_us(uint64_t us) {
    if (!calibrated)
        delay_init();

    uint64_t start = tsc_serialised();
    uint64_t ticks = (tsc_hz / 1000000ull) * us;
    if (ticks == 0)
        ticks = us;   /* sub-microsecond request on a very fast TSC */
    while (tsc_serialised() - start < ticks)
        pause_cpu();
}

void delay_ms(uint64_t ms) {
    if (!calibrated)
        delay_init();

    /* Chunk the wait so a long delay stays interruptible and does not lose too
     * much time to the two rdtsc reads around each chunk. */
    uint64_t chunk_ms = 10;
    while (ms > 0) {
        uint64_t n = ms < chunk_ms ? ms : chunk_ms;
        delay_us(n * 1000ull);
        ms -= n;
    }
}

void delay_seconds(uint64_t seconds) {
    while (seconds-- > 0)
        delay_ms(1000);
}