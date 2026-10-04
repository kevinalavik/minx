#include "task.h"
#include "timer.h"

#include "apic.h"
#include "delay.h"
#include "io.h"
#include "irq.h"
#include "kprintf.h"
#include "panic.h"
#include "percpu.h"
#include "string.h"
#include "types.h"
#include "vectors.h"

/* --- monotonic clock ----------------------------------------------------- */

/* The TSC is what the clock is read from.  Invariant on every CPU QEMU and any
 * real x86-64 exposes, so no resynchronisation is needed.  The frequency comes
 * from delay.c; when it could not be detected we fall back to 1 GHz, which makes
 * derived intervals short rather than long, so the value is also scaled against
 * the LAPIC timer below. */
static uint64_t tsc_hz;

/* Calibrated LAPIC rate, kept for the scheduler's load-balancing maths. */
static uint32_t lapic_ticks_per_ms;

/* 8253 state, declared here because the uptime conversion needs it before the
 * PIT section further down. */
/* Stop the LAPIC timer (defined at the bottom of this file, used by both
 * calibration methods above and by init). */
static void lapic_timer_disable(void);

static volatile uint64_t pit_ticks;
#define PIT_IRQ_HZ 100

/* The TSC reading taken when the 8253's counter was zeroed, so the TSC and the
 * 8253 can be compared over the same interval. */
static uint64_t pit_start_tsc;

uint64_t monotonic_ns(void) {
    /* Keep the sub-second remainder instead of dividing to whole seconds first:
     * rdtsc/tsc_hz is a second count, so rounding it away first throws away
     * every microsecond and leaves a clock that only ticks once a second and
     * always reads as a whole number.  Multiplying the whole TSC by 1e9 would
     * overflow after a few minutes at gigahertz rates, so the quotient and the
     * remainder are converted separately. */
    uint64_t tsc = rdtsc_ordered();
    uint64_t seconds = tsc / tsc_hz;
    uint64_t remainder = tsc % tsc_hz;

    return seconds * NSEC_PER_SEC +
           remainder * NSEC_PER_SEC / (tsc_hz ? tsc_hz : 1);
}

uint64_t monotonic_ms(void) {
    return monotonic_ns() / NSEC_PER_MSEC;
}

void timer_sleep_ms(uint64_t ms) {
    delay_ms(ms);
}

uint64_t timer_uptime_ms(void) {
    uint64_t ticks = pit_ticks;
    if (ticks == 0)
        return monotonic_ms();
    return ticks * 1000ull / PIT_IRQ_HZ;
}

/* The same interval measured with the TSC instead of the 8253.
 *
 * The two only mean anything side by side if they start at the same instant, so
 * this is a delta from the point where the 8253's counter was zeroed, not from
 * boot: comparing a TSC reading taken at boot against a tick count started later
 * measures the boot time as a clock error. */
uint64_t timer_tsc_since_pit_ms(void) {
    uint64_t elapsed = rdtsc_ordered() - pit_start_tsc;
    return elapsed / tsc_hz * 1000ull + (elapsed % tsc_hz) * 1000ull /
                                       (tsc_hz ? tsc_hz : 1);
}

/* --- PIT: the calibration time base -------------------------------------- */

#define PIT_CH0    0x40
#define PIT_CMD    0x43
#define PIT_FREQ   1193182u    /* Hz */
#define PIT_MAX    0xffffu

#define PIT_CH2    0x42
#define PIT_GATE   0x61       /* 8255: bit 5 mirrors the PIT's OUT2 pin */
#define PIT_GATE_BIT 0x20

/* Program channel 0 as a one-shot counting down from `ticks` at PIT_FREQ. */
static void pit_oneshot(uint32_t ticks) {
    outb(PIT_CMD, 0x34);          /* channel 0, lobyte/hibyte, mode 0, binary */
    outb(PIT_CH0, (uint8_t)(ticks & 0xff));
    outb(PIT_CH0, (uint8_t)((ticks >> 8) & 0xff));
}

/* Number of PIT ticks that make up `us` microseconds, clamped to the counter. */
static uint32_t pit_ticks_for_us(uint32_t us) {
    uint64_t ticks = (uint64_t)(PIT_FREQ / 1000000u) * us;
    if (ticks > PIT_MAX)
        ticks = PIT_MAX;
    return (uint32_t)ticks;
}

/* Run one exact 65536-tick window on the 8253's channel 2 and report what
 * happened inside it.
 *
 * Channel 2 is wired to the 8255's speaker path and its OUT2 pin is readable on
 * bit 5 of port 0x61, so counting 65536 ticks gives a window of exactly
 * 65536 / 1193182 s -- a real-time reference that depends on no frequency
 * guess, which is the whole point: anything calibrated against the TSC needs a
 * clock other than the TSC to calibrate against.  The window is opened by
 * raising the gate with the speaker path enabled and closed by OUT2 going low.
 *
 * Both measurements are taken from the same window because taking two windows
 * doubles the cost and lets the two readings drift apart. */
#define PIT_WINDOW_TICKS 0x10000u   /* 65536 */

static bool pit_exact_window(uint64_t *tsc_delta, uint32_t *lapic_delta) {
    /* Speaker path off, gate low. */
    uint8_t saved = (uint8_t)(inb(PIT_GATE) & ~0x0c);
    outb(PIT_GATE, (uint8_t)(saved | 0x01));

    /* Channel 2, lobyte/hibyte, mode 0, binary: start counting when the gate
     * rises with the speaker path enabled. */
    outb(PIT_CMD, 0xb0);
    outb(PIT_CH2, 0x00);
    outb(PIT_CH2, 0x00);
    outb(PIT_GATE, (uint8_t)(saved | 0x03));

    /* LAPIC timer in one-shot mode, counting down from 0xffffffff with no
     * divider, so the current-count register reads how far it has come.  The
     * LVT Timer entry (0x320) selects the mode and vector; the counter is a
     * different register (0x380), and writing the count to the LVT would arm
     * nothing at all. */
    lapic_timer_disable();
    lapic_write_reg(LAPIC_COUNT, 0xffffffffu);

    uint64_t tsc_start = rdtsc_ordered();

    unsigned spins = 0;
    while ((inb(PIT_GATE) & PIT_GATE_BIT) == 0) {
        if (++spins > 200000000u) {
            outb(PIT_GATE, saved);
            lapic_timer_disable();
            return false;
        }
        pause_cpu();
    }

    /* Leave the gate low and the speaker off. */
    outb(PIT_GATE, saved);

    uint32_t lapic_raw = lapic_read_reg(LAPIC_CURRENT_COUNT);
    uint64_t tsc_elapsed = rdtsc_ordered() - tsc_start;

    lapic_timer_disable();

    if (tsc_delta)
        *tsc_delta = tsc_elapsed;
    if (lapic_delta) {
        /* The counter counts down, so the elapsed ticks are the difference
         * between where it started and where it ended. */
        *lapic_delta = (uint32_t)(0xffffffffu - lapic_raw) + 1u;
    }
    return true;
}

/* Measure the LAPIC timer's rate against the 8253.
 *
 * This is the reference every other clock is measured against, so it must not
 * be measured against the TSC: a TSC of unknown frequency cannot check itself. */
static bool pit_calibrate(uint32_t *ticks_per_ms) {
    uint64_t window_ticks = PIT_WINDOW_TICKS;

    uint32_t elapsed;
    if (!pit_exact_window(NULL, &elapsed))
        return false;

    /* ticks_per_ms = elapsed / window_ms, where
     * window_ms = window_ticks / PIT_FREQ * 1000.  Rearranged to keep the
     * multiply in 64 bits: elapsed * PIT_FREQ stays well inside uint64. */
    uint64_t per_ms = (uint64_t)elapsed * PIT_FREQ / ((uint64_t)window_ticks * 1000ull);
    if (per_ms < 1000ull || per_ms > 100000000ull)
        return false;

    *ticks_per_ms = (uint32_t)per_ms;
    return true;
}

static uint32_t pit_reference_hz(void) {
    return PIT_FREQ;
}

/* Measure the LAPIC timer's tick rate against the TSC.
 *
 * The TSC provides the time base and the LAPIC counter provides the count, so
 * lapic_hz = elapsed_ticks / elapsed_seconds.  All of it is integer arithmetic:
 * soft-float helpers are not available in a freestanding kernel.
 */
static bool lapic_calibrate(uint32_t *ticks_per_ms) {
    /* Stop counting, then run one-shot from 0xffffffff with no divider. */
    lapic_timer_disable();
    lapic_write_reg(LAPIC_COUNT, 0xffffffffu);

    uint64_t tsc_start = rdtsc_ordered();
    uint64_t expected = (tsc_hz / 1000u) * 20u;   /* a 20 ms window */

    /* Burn the window without relying on delay_init(), which would recurse. */
    while (rdtsc_ordered() - tsc_start < expected)
        pause_cpu();

    uint32_t raw = lapic_read_reg(LAPIC_CURRENT_COUNT);
    uint64_t tsc_elapsed = rdtsc_ordered() - tsc_start;

    lapic_timer_disable();

    if (tsc_elapsed == 0)
        return false;

    /* The counter counts up from 0xffffffff, so the elapsed count is the
     * two's complement difference plus one. */
    uint64_t lapic_elapsed = (uint64_t)(uint32_t)(0xffffffffu - raw) + 1ull;
    if (lapic_elapsed == 0)
        return false;

    /* ticks_per_ms = lapic_elapsed * 1000 / tsc_elapsed, with the multiply done
     * in 64 bits so a fast timer cannot overflow. */
    uint64_t per_ms = lapic_elapsed * 1000ull / tsc_elapsed;
    if (per_ms < 1000ull || per_ms > 100000000ull)
        return false;

    *ticks_per_ms = (uint32_t)per_ms;
    return true;
}

/* --- RTC / CMOS ---------------------------------------------------------- */

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define RTC_SECONDS      0x00
#define RTC_MINUTES      0x02
#define RTC_HOURS        0x04
#define RTC_DAY          0x07
#define RTC_MONTH        0x08
#define RTC_YEAR         0x09
#define RTC_STATUS_A     0x0a
#define RTC_STATUS_B     0x0b
#define RTC_CENTURY      0x32
#define RTC_STATUS       0x0c

#define RTC_STATUS_B_24H   0x02
#define RTC_STATUS_B_BINARY 0x04
#define RTC_STATUS_B_UPD   0x80

static bool rtc_24h;
static bool rtc_binary;
static bool rtc_utc;

static inline uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static inline void cmos_write(uint8_t reg, uint8_t value) {
    outb(CMOS_ADDR, reg);
    outb(CMOS_DATA, value);
}

static bool cmos_update_in_progress(void) {
    return (cmos_read(RTC_STATUS_A) & 0x80) != 0;
}

int rtc_init(void) {
    uint8_t status_b = cmos_read(RTC_STATUS_B);
    rtc_24h = (status_b & RTC_STATUS_B_24H) != 0;
    rtc_binary = (status_b & RTC_STATUS_B_BINARY) != 0;

    /* Guess once whether the firmware keeps the RTC on UTC; a real UTC system
     * and QEMU both default to it.  Phase 12 re-reads this from the FADT. */
    rtc_utc = true;

    /* Update-ended interrupts are not used, so make sure they are off. */
    uint8_t b = cmos_read(RTC_STATUS_B);
    cmos_write(RTC_STATUS_B, (uint8_t)(b & ~RTC_STATUS_B_UPD));

    rtc_time_t now;
    rtc_read(&now);
    kprintf("RTC: %04u-%02u-%02u %02u:%02u:%02u (%s)\n", now.year, now.month,
            now.day, now.hour, now.minute, now.second, rtc_utc ? "UTC" : "local");
    return 0;
}

static uint8_t from_bcd(uint8_t v) {
    return rtc_binary ? v : (uint8_t)((v & 0x0f) + ((v >> 4) * 10));
}

static uint8_t read_rtc_reg(uint8_t reg) {
    /* Values are only stable when no update is in progress. */
    for (unsigned spins = 0; spins < 1000000u; spins++) {
        if (!cmos_update_in_progress())
            return from_bcd(cmos_read(reg));
        pause_cpu();
    }
    return from_bcd(cmos_read(reg));
}

void rtc_read(rtc_time_t *out) {
    memset(out, 0, sizeof(*out));

    uint8_t seconds = read_rtc_reg(RTC_SECONDS);
    uint8_t minutes = read_rtc_reg(RTC_MINUTES);
    uint8_t hours   = read_rtc_reg(RTC_HOURS);
    uint8_t day     = read_rtc_reg(RTC_DAY);
    uint8_t month   = read_rtc_reg(RTC_MONTH);
    uint8_t year    = read_rtc_reg(RTC_YEAR);
    uint8_t century = from_bcd(cmos_read(RTC_CENTURY));

    /* The hour register's bit 7 marks 12-hour mode, in which 0 is 12. */
    bool pm = false;
    if (!rtc_24h) {
        uint8_t raw = cmos_read(RTC_HOURS);
        pm = (raw & 0x80) != 0;
        hours = (uint8_t)(raw & 0x1f);
    }
    if (!rtc_binary && hours > 12)
        hours = (uint8_t)(hours - 12);
    if (pm && hours < 12)
        hours = (uint8_t)(hours + 12);

    unsigned full_year;
    if (century >= 19 && century <= 21) {
        full_year = (unsigned)century * 100u + year;
    } else {
        /* No usable century: assume the firmware stores 20xx. */
        full_year = 2000u + year;
    }

    out->year = (int)full_year;
    out->month = month;
    out->day = day;
    out->hour = hours;
    out->minute = minutes;
    out->second = seconds;
    out->nanosecond = 0;
    out->utc = rtc_utc;
}

size_t rtc_format(const rtc_time_t *t, char *buf, size_t size) {
    int n = ksnprintf(buf, size, "%04d-%02u-%02u %02u:%02u:%02u", t->year,
                      t->month, t->day, t->hour, t->minute, t->second);
    return n < 0 ? 0 : (size_t)n;
}

/* --- 8253 on IRQ0: the time base that needs no MMIO --------------------- */

#define PIT_IRQ_VECTOR VECTOR_IRQ0   /* IRQ0 lands on the first device line */

/* Program channel 0 for a periodic rate and count. */
void pit_program_rate(uint32_t hz, uint32_t divisor) {
    outb(PIT_CMD, 0x34);          /* channel 0, lobyte/hibyte, mode 2, binary */
    outb(PIT_CH0, (uint8_t)(divisor & 0xff));
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xff));
    (void)hz;
}

static void pit_irq(struct regs *regs) {
    (void)regs;
    pit_ticks++;
    percpu_count_interrupt();
}

/* Find out which IOAPIC pin actually carries the 8253's interrupt.
 *
 * The kernel can raise this line whenever it likes, which makes it the one
 * legacy interrupt that can be located rather than assumed, and locating it
 * matters because "pin n is ISA IRQ n" is an expectation and not a guarantee:
 * QEMU's IOAPIC model routes the 8253's IRQ0 to pin 2, so a kernel that
 * programs the identity map gets a 100 Hz clock arriving on IRQ2's vector,
 * where nothing is listening.  Nothing about the delivery tells the guest that
 * it came from the wrong pin -- the chip hands over a vector and nothing else --
 * so the only way to get it right is to go and look.
 *
 * Each pin is given the PIT's own vector with every other pin masked, and the
 * pin whose vector turns up is the one the clock is on.  Masking the others is
 * what makes the attribution sound: with several pins live, an interrupt cannot
 * be traced back to the pin that raised it.
 *
 * The rate is raised for the probe so that a window short enough to keep the
 * whole scan down to a fraction of a second still contains several ticks. */
#define PIT_PROBE_HZ     2000u
#define PIT_PROBE_WINDOW 6u

static bool pit_probe_pin(unsigned vector) {
    uint8_t cpu = ioapic_primary_cpu();

    ioapic_mask_all_legacy_pins();

    for (unsigned pin = 0; pin < 16; pin++) {
        if (ioapic_route_pin_to_vector(pin, vector, cpu) != 0)
            continue;

        uint64_t before = pit_uptime_ticks();

        sti();
        delay_ms(PIT_PROBE_WINDOW);
        cli();

        if (pit_uptime_ticks() != before) {
            kprintf("timer: the 8253's interrupt is on IOAPIC pin %u, "
                    "not pin 0\n", pin);
            ioapic_set_legacy_pin(0, pin);
            return true;
        }
    }

    kprintf("[warn] timer: no IOAPIC pin answered for the 8253; assuming "
            "pin 0\n");
    return false;
}

/* Start the 8253 as a periodic interrupt on IRQ0. */
int pit_irq_init(void) {
    /* The nearest divisor to PIT_FREQ/PIT_IRQ_HZ that fits in 16 bits. */
    uint32_t divisor = PIT_FREQ / PIT_IRQ_HZ;
    if (divisor == 0)
        divisor = 1;
    if (divisor > 0xffff)
        divisor = 0xffff;

    if (irq_register(0, pit_irq, "pit") != 0)
        return -1;

    /* Find the real pin before programming it, then route the whole legacy set
     * once the mapping is known so the table matches what the machine does. */
    pit_program_rate(PIT_PROBE_HZ, PIT_FREQ / PIT_PROBE_HZ);
    if (ioapic_count() > 0)
        pit_probe_pin(PIT_IRQ_VECTOR);
    if (ioapic_route_legacy_lines(VECTOR_IRQ0, ioapic_primary_cpu(), false) != 0)
        return -1;

    pit_program_rate(PIT_IRQ_HZ, divisor);
    pit_ticks = 0;
    pit_start_tsc = rdtsc_ordered();

    if (irq_enable(0) != 0)
        return -1;

    kprintf("timer: 8253 channel 0 at %u Hz (divisor %u) on IRQ 0, "
            "vector %u\n", PIT_IRQ_HZ, divisor, PIT_IRQ_VECTOR);
    return 0;
}

/* Ticks since the PIT was started.  Monotonic and independent of the TSC. */
uint64_t pit_uptime_ticks(void) {
    return pit_ticks;
}

uint32_t pit_irq_hz(void) {
    return PIT_IRQ_HZ;
}

/* --- init ---------------------------------------------------------------- */

/* The preemption tick gets its own vector rather than one of the sixteen
 * device lines, so it cannot be confused with the 8253 on IRQ0 -- both are
 * timers, and both were previously routed to the same vector, where the
 * dispatcher could not tell which had fired. */
#ifndef TIMER_IRQ_VECTOR
#define TIMER_IRQ_VECTOR VECTOR_LAPIC_TIMER
#endif

uint32_t lapic_timer_ticks_per_ms(void);

/* Stop the LAPIC timer.  Writing zero to the count register halts it; the LVT
 * Timer entry is left alone because it also carries the vector, which later
 * phases still need. */
static void lapic_timer_disable(void) {
    lapic_write_reg(LAPIC_COUNT, 0);
    /* Wait for the counter to actually stop: the write is posted, and reading
     * the current count back immediately can still return the old value. */
    for (unsigned spins = 0; spins < 1000u; spins++) {
        if (lapic_read_reg(LAPIC_CURRENT_COUNT) == 0)
            return;
        pause_cpu();
    }
}

/* Measure the TSC's real frequency against the 8253.
 *
 * The CPUID leaves that could report a TSC frequency are optional and frequently
 * report nothing at all, and the fallback of assuming one is wrong in whichever
 * direction the assumption was not checked: on a machine whose TSC runs faster
 * than the guess, every calibrated delay comes out short -- a "10 ms" wait that
 * lasts 2 ms, a timeout that fires early, a heartbeat that beats five times too
 * fast.  There is no way to notice that from inside the kernel unless something
 * independent is measured against it, and the 8253 is the only such thing here.
 *
 * Returns false and leaves the detected value alone if the measurement does not
 * look like a clock: a TSC that advanced by an implausible amount over a 55 ms
 * window is more likely a mis-read than a very fast processor. */
static bool tsc_calibrate(void) {
    uint64_t detected = tsc_frequency();

    uint64_t elapsed;
    if (!pit_exact_window(&elapsed, NULL))
        return false;
    if (elapsed == 0)
        return false;

    /* hz = tsc_ticks * PIT_FREQ / window_ticks, entirely in 64 bits. */
    uint64_t hz = elapsed * PIT_FREQ / PIT_WINDOW_TICKS;

    /* Between 1 MHz and 100 GHz.  Below that it is not a TSC; above it, either
     * the window was not the window it claims to be or this is not a machine
     * worth guessing about. */
    if (hz < 1000000ull || hz > 100000000000ull)
        return false;

    /* Within 1 per cent of the detected value there is nothing to correct, and
     * leaving the CPUID number alone keeps its provenance in the log. */
    if (detected != 0) {
        uint64_t diff = detected > hz ? detected - hz : hz - detected;
        if (diff * 100ull <= detected)
            return false;
    }

    delay_set_tsc_hz(hz);
    kprintf("timer: TSC measured at %lu.%03lu MHz against the 8253 "
            "(detected %lu Hz was wrong)\n",
            (unsigned long)(hz / 1000000ull),
            (unsigned long)((hz / 1000ull) % 1000ull),
            (unsigned long)detected);
    return true;
}

int timer_init(void) {
    /* The TSC has to be right before anything measures anything, because every
     * delay in the kernel is a TSC delta and the 8253 is the only clock here
     * that can be trusted to check it. */
    delay_init();
    tsc_calibrate();
    tsc_hz = tsc_frequency();

    kprintf("timer: TSC at %lu MHz (%s)\n",
            (unsigned long)(tsc_hz / 1000000),
            tsc_is_invariant() ? "invariant" : "not invariant");

    if (!lapic_available()) {
        kprintf("[warn] no LAPIC timer; the kernel has no preemption yet\n");
        return -1;
    }

    uint32_t ticks_per_ms = 0;
    if (!pit_calibrate(&ticks_per_ms)) {
        kwarn("PIT-gate calibration failed; falling back to the TSC");
        if (!lapic_calibrate(&ticks_per_ms)) {
            kwarn("TSC calibration failed too; assuming 10000000 ticks/ms");
            ticks_per_ms = 10000000;
        }
    }
    kprintf("timer: LAPIC timer calibrated at %u ticks/ms (PIT %u Hz)\n",
            ticks_per_ms, pit_reference_hz());
    lapic_ticks_per_ms = ticks_per_ms;

    /* Cross-check against the TSC: two independent measurements that disagree
     * mean one of the two clocks is not what we think it is, which is worth
     * knowing before the scheduler depends on it. */
    uint32_t tsc_based = 0;
    if (lapic_calibrate(&tsc_based) && tsc_based != 0) {
        int32_t delta = (int32_t)(tsc_based - ticks_per_ms);
        if (delta < 0)
            delta = -delta;
        uint32_t permille = (ticks_per_ms != 0)
                                ? (delta * 1000u) / ticks_per_ms : 1000u;
        kprintf("timer: TSC cross-check says %u ticks/ms (%u per mille %s)\n",
                tsc_based, permille,
                permille <= 50 ? "agreement" : "DISAGREEMENT");
    }

    /* A one-shot on channel 0 is left programmed so later phases have a
     * progress indicator available; it is not running now. */
    pit_oneshot(pit_ticks_for_us(1000));

    /* Program a periodic 1 ms tick.
     *
     * Two registers are involved and confusing them is silent: the LVT Timer
     * entry (0x320) holds the vector and the periodic bit, and the count
     * register (0x380) holds the period.  Writing the period into the LVT arms
     * no timer at all, which is exactly what used to happen here.  The counter
     * is a full 32 bits wide and the LVT only carries 8 bits of vector. */
    uint32_t period = ticks_per_ms;
    if (period < 1)
        period = 1;
    if (period > 0xffffffffu)
        period = 0xffffffffu;

    lapic_timer_disable();

    /* Unmasked, periodic, with the vector in the low byte.  Clearing the mask bit
     * is the part that matters: the firmware leaves it set, and a masked LVT
     * arms a timer that counts down and never interrupts -- which looks exactly
     * like a working timer whose interrupts are being dropped somewhere else. */
    lapic_write_reg(LAPIC_LVT_TIMER,
                    (lapic_read_reg(LAPIC_LVT_TIMER) &
                     ~(LAPIC_LVT_MASKED | 0xffu)) |
                        LAPIC_LVT_PERIODIC |
                        (uint32_t)(TIMER_IRQ_VECTOR & 0xffu));
    lapic_write_reg(LAPIC_COUNT, period);

    kprintf("timer: LAPIC timer armed periodic, vector %u, count %u "
            "(%u ticks/ms)\n", (unsigned)TIMER_IRQ_VECTOR, period,
            ticks_per_ms);
    return 0;
}

/* --- the tick ------------------------------------------------------------ */

/* Ticks on the LAPIC timer.  Kept separate from the 8253's counter: the two are
 * different clocks at different rates, and phase 5's scheduler needs the one
 * that actually drives preemption. */
static volatile uint64_t lapic_ticks;

/* The preemption tick.
 *
 * This is the only place a task is taken off a processor without asking.  The EOI
 * has to be written *before* the switch, not after: the switch leaves this frame
 * on the task's stack and does not come back until the task is resumed, and an
 * unacknowledged in-service bit at priority zero blocks every further interrupt of
 * that priority -- including the timer that would bring the task back.  So the
 * order is acknowledge, then switch. */
static void lapic_timer_interrupt(struct regs *regs) {
    (void)regs;
    lapic_ticks++;
    percpu_count_interrupt();

    lapic_eoi();

    sched_tick();
}

int lapic_timer_interrupt_init(void) {
    isr_install_handler(VECTOR_LAPIC_TIMER, lapic_timer_interrupt);
    return 0;
}

/* Arm *this* processor's own LAPIC timer at the rate the boot processor uses.
 *
 * A local APIC is per-CPU state: an application processor comes out of its
 * trampoline with an LVT timer entry the firmware left masked and a count
 * register of zero, and inherits none of the boot processor's programming.  The
 * 8253's line is routed to the boot processor's local APIC, so a processor that
 * does not arm its own counter never ticks at all -- which is not only a lost
 * time slice, it is a processor that can never notice a halt request, so an idle
 * loop that waits for one would wait forever.
 *
 * The rate is reused rather than recalibrated: it was measured against the 8253
 * by the boot processor, and every processor's local APIC is driven by the same
 * bus. */
int lapic_timer_arm_local(void) {
    if (!lapic_available() || lapic_ticks_per_ms == 0)
        return -1;

    lapic_write_reg(LAPIC_LVT_TIMER,
                    (lapic_read_reg(LAPIC_LVT_TIMER) &
                     ~(LAPIC_LVT_MASKED | 0xffu)) |
                        LAPIC_LVT_PERIODIC |
                        (uint32_t)(TIMER_IRQ_VECTOR & 0xffu));
    lapic_write_reg(LAPIC_COUNT, lapic_ticks_per_ms);
    return 0;
}

uint64_t lapic_timer_ticks(void) {
    return lapic_ticks;
}

uint32_t lapic_timer_ticks_per_ms(void);
uint32_t lapic_timer_ticks_per_ms(void) {
    return lapic_ticks_per_ms;
}