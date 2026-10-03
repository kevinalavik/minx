/* minx - calibrated time delays.
 *
 * Busy-waiting on a raw rdtsc() delta needs the TSC frequency, which is not
 * fixed across the machines this kernel is expected to run on.  CPUID leaf
 * 0x15 gives the crystal ratio, leaf 0x16 gives the frequency directly, and KVM
 * exposes leaf 0x40000010.  When none of those are available we assume 1 GHz,
 * which only ever makes a delay longer than requested -- never shorter, so
 * callers can rely on it as a minimum.
 */
#include "delay.h"

#include "cpu.h"
#include "io.h"
#include "types.h"

static uint64_t tsc_hz;
static bool calibrated;

static uint64_t detect_tsc_hz(void) {
    uint32_t a, b, c, d;

    /* CPUID leaf 0x16: EAX = crystal clock in Hz, EBX = 0. */
    cpuid_raw(0x16, 0, &a, &b, &c, &d);
    if (a != 0)
        return a;

    /* KVM's TSC frequency leaf. */
    cpuid_raw(0x40000010, 0, &a, &b, &c, &d);
    if (a != 0)
        return a;

    /* CPUID leaf 0x15: ECX = numerator, EBX:EAX = denominator. */
    cpuid_raw(0x15, 0, &a, &b, &c, &d);
    if (c != 0 && b != 0)
        return (uint64_t)c * 1000000000ull / (uint64_t)b;

    return 0;
}

void delay_init(void) {
    if (calibrated)
        return;

    uint64_t hz = detect_tsc_hz();
    if (hz == 0) {
        /* No frequency information: assume 1 GHz, so a requested delay always
         * comes out at least as long as asked for. */
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

bool tsc_is_invariant(void) {
    uint32_t a, b, c, d;
    cpuid_raw(0x80000007, 0, &a, &b, &c, &d);
    return (d & (1u << 8)) != 0;
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