/* minx - the higher-half direct map offset, as reported by the bootloader. */
#ifndef MINX_HHDM_H
#define MINX_HHDM_H

#include <stdint.h>

#include <limine.h>

/* The request block entry.  Limine fills in the response before kmain() runs. */
extern volatile struct limine_hhdm_request limine_hhdm_request;

/* Ask the bootloader where it put the direct map.  Must run before anything
 * calls phys_to_virt(). */
void hhdm_init(void);

/* Set the offset directly; used by hhdm_init() and by the self test to prove
 * that a wrong offset breaks phys_to_virt(). */
void hhdm_set(uint64_t offset);

#endif /* MINX_HHDM_H */