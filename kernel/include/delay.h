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
bool tsc_is_invariant(void);

#endif /* MINX_DELAY_H */