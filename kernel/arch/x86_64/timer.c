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

uint64_t monotonic_ns(void) {
    return (rdtsc_ordered() / tsc_hz) * NSEC_PER_SEC;
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

/* Measure the LAPIC timer's rate against the PIT.
 *
 * Channel 2 is wired to the 8255's speaker path and its OUT2 pin is readable on
 * bit 5 of port 0x61, so counting 65536 ticks gives an exact window of
 * 65536 / 1193182 s without depending on the TSC frequency at all.  The window
 * is opened by raising the gate with the speaker enabled and closed by OUT2
 * going low.
 */
static bool pit_calibrate(uint32_t *ticks_per_ms) {
    const uint32_t window_ticks = 0x10000;   /* 65536 */

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

    uint32_t raw = lapic_read_reg(LAPIC_CURRENT_COUNT);

    lapic_timer_disable();

    /* The counter counts down, so the elapsed ticks are the difference between
     * where it started and where it ended. */
    uint64_t elapsed = (uint64_t)(uint32_t)(0xffffffffu - raw) + 1ull;
    if (elapsed == 0)
        return false;

    /* ticks_per_ms = elapsed / window_ms, where
     * window_ms = window_ticks / PIT_FREQ * 1000.  Rearranged to keep the
     * multiply in 64 bits: elapsed * PIT_FREQ stays well inside uint64. */
    uint64_t per_ms = elapsed * PIT_FREQ / ((uint64_t)window_ticks * 1000ull);
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

int timer_init(void) {
    delay_init();
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

static void lapic_timer_interrupt(struct regs *regs) {
    (void)regs;
    lapic_ticks++;
    percpu_count_interrupt();

    /* Phase 5 turns this into the scheduler tick.  Until then the counter is
     * still the proof that the timer interrupt path works at all. */
    if (lapic_ticks == 1)
        kprintf("timer: LAPIC timer interrupt arrived (vector %u)\n",
                (unsigned)VECTOR_LAPIC_TIMER);

    lapic_eoi();
}

int lapic_timer_interrupt_init(void) {
    isr_install_handler(VECTOR_LAPIC_TIMER, lapic_timer_interrupt);
    return 0;
}

uint64_t lapic_timer_ticks(void) {
    return lapic_ticks;
}

uint32_t lapic_timer_ticks_per_ms(void);
uint32_t lapic_timer_ticks_per_ms(void) {
    return lapic_ticks_per_ms;
}