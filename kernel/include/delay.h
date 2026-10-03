/* minx - calibrated busy-wait delays based on the TSC. */
#ifndef MINX_DELAY_H
#define MINX_DELAY_H

#include <stdbool.h>
#include <stdint.h>

void delay_init(void);
void delay_us(uint64_t us);
void delay_ms(uint64_t ms);
void delay_seconds(uint64_t seconds);

/* TSC frequency in Hz, as detected from CPUID (see delay.c). */
uint64_t tsc_frequency(void);

/* Replace the detected TSC frequency with a measured one.  timer_init() calls
 * this with the TSC's rate measured against the 8253; the detected value is
 * still reported by delay_report() so the disagreement stays visible. */
void delay_set_tsc_hz(uint64_t hz);
bool tsc_is_invariant(void);

/* Raw CPUID values behind that detection, reported at boot so a wrong
 * calibration is visible rather than mysterious. */
void delay_report(void);

#endif /* MINX_DELAY_H */