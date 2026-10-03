/* minx - kernel wide definitions shared by every subsystem. */
#ifndef MINX_TYPES_H
#define MINX_TYPES_H

#include <stddef.h>
#include <stdint.h>

#define MINX_NAME_MAX 255

/* Physical/virtual address helpers.  The kernel is linked into the higher
 * half; the direct map window lands 4 GiB above it. */
#define KERNEL_VIRT_BASE 0xffffffff80000000ULL
#define HHDM_OFFSET      0xffff800000000000ULL

#define phys_to_virt(p) ((uint64_t)(p) + HHDM_OFFSET)
#define virt_to_phys(v) ((uint64_t)(v) - HHDM_OFFSET)

#define KERNEL_STACK_TOP 0xffffffff80000000ULL

#define page_size       0x1000UL
#define page_shift      12

#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((uint64_t)(a) - 1)) == 0)

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

#define array_len(a) (sizeof(a) / sizeof((a)[0]))

#define UNUSED(x) ((void)(x))

/* Every subsystem exposes an init function called from kmain() in phase
 * order; returning a negative value means "no hardware, carry on". */
typedef int (*subsystem_init_fn)(void);

#endif /* MINX_TYPES_H */