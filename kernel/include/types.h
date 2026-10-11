/* minx - kernel wide definitions shared by every subsystem. */
#ifndef MINX_TYPES_H
#define MINX_TYPES_H

#include <stddef.h>
#include <stdint.h>

#define MINX_NAME_MAX 255

/* Physical/virtual address helpers.  The kernel is linked into the higher
 * half; the direct map window lands 4 GiB above it. */
#define KERNEL_VIRT_BASE 0xffffffff80000000ULL

/* The higher-half direct map's base offset.
 *
 * This is *not* a constant.  Limine 12 randomises the offset (for KASLR), and
 * reports it through the protocol's HHDM response; hard-coding
 * 0xffff800000000000 makes every phys_to_virt() read land somewhere else, which
 * shows up as the allocator handing out frames whose contents are whatever the
 * machine happens to have at a random physical address.  mm/hhdm.c asks the
 * bootloader for the real value at boot and this variable holds it. */
extern uint64_t hhdm_offset;

#define HHDM_OFFSET hhdm_offset

#define phys_to_virt(p) ((uint64_t)(p) + hhdm_offset)
#define virt_to_phys(v) ((uint64_t)(v) - hhdm_offset)

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

/* A file's mode bits: the permission bits plus the type, exactly as they appear in a cpio
 * header, so the initrd's are used without translating them. */
typedef uint32_t mode_t;

#define S_IFMT   0170000u
#define S_IFDIR  0040000u
#define S_IFREG  0100000u
#define S_IFLNK  0120000u
#define S_IFCHR  0020000u
#define S_IFBLK  0060000u
#define S_IFIFO  0010000u
#define S_IFSOCK 0140000u
