/* minx - procfs.
 *
 * The kernel's own state, as files.  Every one of them is generated when it is *read*, so
 * there is exactly one copy of the numbers in the machine and it is the kernel's -- a
 * `/proc/cpuinfo` cached at boot would be a file that lies the moment a processor comes up
 * or a task exits, and nothing about the file would say so.
 *
 * That is also what makes this cheap: there is no storage, no buffer to keep in step, and
 * no way for what is written here to disagree with what the code does.
 *
 * The cost is that a file's size changes between two `stat`s, which is why `stat` on a
 * procfs node reports a size and `read` returns whatever it has today.  Every program that
 * reads `/proc/meminfo` reads until end of file rather than to a length it got from
 * `stat`, and that is the rule to write down for anything that has to cope with a process
 * filesystem at all.
 */
#ifndef MINX_PROCFS_H
#define MINX_PROCFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vfs.h"

/* Build /proc.  Every file in it is created as a zero-length regular file whose contents
 * are produced on read; the name is all that is stored. */
int procfs_init(void);

/* Whether /proc has been built. */
bool procfs_ready(void);

/* The files at the top of /proc, and the numbers behind them. */
size_t procfs_cpuinfo(char *buf, size_t len);
size_t procfs_meminfo(char *buf, size_t len);
size_t procfs_uptime(char *buf, size_t len);
size_t procfs_mounts(char *buf, size_t len);
size_t procfs_version(char *buf, size_t len);

/* One task's status.  `proc_id` is PROCFS_SELF for `self` and `self/..`, which is the
 * same task and is written because every program looks for it. */
size_t procfs_status(uint64_t proc_id, char *buf, size_t len);

/* How many bytes the file `p` names would produce right now, for `stat`.  A procfs file's
 * size is the size of what is generated today, which is the only honest answer. */
size_t procfs_size(const vpath_t *p, uint64_t proc_id);

/* Generate `p` into `buf`; `used` says how much was written.  0, or -errno. */
long procfs_read(const vpath_t *p, uint64_t proc_id, void *buf, size_t len,
                 size_t *used);

/* The process id a procfs node refers to, or PROCFS_SELF.  A node named by a number is
 * that process; `self` and the node for the calling task are PROCFS_SELF. */
uint64_t procfs_id_of(const vpath_t *p, const struct task *self);

/* Register a task so `/proc/<pid>` exists for it.  Called from task registration; without
 * it a running process has no entry, and `/proc` is then a directory of numbers with
 * nothing behind them. */
void procfs_note_task(const struct task *t);
void procfs_forget_task(const struct task *t);

#endif /* MINX_PROCFS_H */