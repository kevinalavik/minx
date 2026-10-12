/* minx - the memory system calls.  See memsys.c for what the heap and the mmap area are. */
#ifndef MINX_MEMSYS_H
#define MINX_MEMSYS_H

#include "cpu.h"

long sys_mmap(regs_t *regs);
long sys_munmap(regs_t *regs);
long sys_mprotect(regs_t *regs);

/* `brk`, which lives in syscall.c for historical reasons and is implemented beside the
 * other memory calls because it is the same question they all ask. */
long sys_brk_grow(regs_t *regs);

#endif /* MINX_MEMSYS_H */
