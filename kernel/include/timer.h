/* minx - timekeeping.
 *
 * Three sources with different jobs:
 *   - the TSC          : the monotonic clock (never goes backwards)
 *   - the LAPIC timer   : the time-slice interrupt
 *   - the RTC (CMOS)   : the wall clock
 *
 * The PIT is used exactly once, to calibrate the LAPIC timer against a known
 * frequency, and is then left alone.
 */
#ifndef MINX_TIMER_H
#define MINX_TIMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_SEC  1000000000ull

int timer_init(void);

/* The 8253 as a periodic IRQ0 source.  Unlike the LAPIC timer this needs no
 * MMIO, so it works before the kernel owns its own page tables. */
int  pit_irq_init(void);
void pit_program_rate(uint32_t hz, uint32_t divisor);
uint64_t pit_uptime_ticks(void);
uint32_t pit_irq_hz(void);

/* Monotonic clock, in nanoseconds since boot.  Safe to call from any CPU. */
uint64_t monotonic_ns(void);
uint64_t monotonic_ms(void);

/* Ticks on the LAPIC timer, which is what drives preemption.  A separate
 * counter from the 8253's because they are different clocks at different
 * rates. */
int      lapic_timer_interrupt_init(void);
int      lapic_timer_arm_local(void);
uint64_t lapic_timer_ticks(void);

/* Milliseconds since the PIT started, which is a monotonic time base that does
 * not depend on the TSC frequency being known. */
uint64_t timer_uptime_ms(void);

/* The same interval as timer_uptime_ms(), measured with the TSC instead of the
 * 8253 and from the same starting point, so the two can be compared. */
uint64_t timer_tsc_since_pit_ms(void);

/* Busy-wait helpers that respect a TSC frequency already calibrated. */
void timer_sleep_ms(uint64_t ms);

/* Wall clock, read from the RTC. */
typedef struct {
    int      year;       /* full year, e.g. 2026      */
    unsigned month;      /* 1-12                       */
    unsigned day;        /* 1-31                       */
    unsigned hour;       /* 0-23                       */
    unsigned minute;     /* 0-59                       */
    unsigned second;     /* 0-59                       */
    unsigned nanosecond; /* 0-999999999                */
    bool     utc;        /* the RTC is running on UTC   */
} rtc_time_t;

int  rtc_init(void);
void rtc_read(rtc_time_t *out);

/* Seconds since the epoch, from the real-time clock.
 *
 * Separate from `timer_uptime_ms()` on purpose: uptime is measured from boot and moves at
 * whatever rate the timebase does, while this is the wall clock a file's timestamps and
 * `date` want.  They disagree by however long the machine has been up, which is the point
 * of having both. */
uint64_t wallclock_seconds(void);

/* Format an RTC timestamp as "YYYY-MM-DD HH:MM:SS" into buf. */
size_t rtc_format(const rtc_time_t *t, char *buf, size_t size);

#endif /* MINX_TIMER_H */