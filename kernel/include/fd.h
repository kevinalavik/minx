/* minx - per-process descriptors, and pipes.
 *
 * A program names things by number.  Until this existed the kernel had only the three
 * numbers every process starts with, hard-coded into `sys_fd_write()`, and every call
 * naming any other came back EBADF -- which is correct, and also the end of the line: a
 * pipe is two more numbers and they have to come from somewhere.
 *
 * The table itself, and the types in it, are declared in task.h because task_t embeds
 * them.  This is the interface.
 */
#ifndef MINX_FD_H
#define MINX_FD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "task.h"

/* The calling task's table, or NULL if there is no calling task -- which is the case in a
 * kernel thread, and not an error in itself: a kernel thread that opens a descriptor is a
 * mistake in the caller, and `fd_alloc` says so by returning -1. */
fd_entry_t *fd_table(void);

/* Give the calling task a table holding only the three descriptors every process starts
 * with.  Called at creation. */
void fd_table_init_for_new_task(void);

/* Give `dst` a copy of `src`'s entries.  The objects are shared; the table is not -- and
 * sharing an object means the copy is another *user* of it, so this also counts the pipe
 * ends the child now holds. */
void fd_table_copy(fd_entry_t *dst, const fd_entry_t *src);

/* The lowest free descriptor naming `kind`, and its number.  -1 if the table is full or
 * there is no calling task.  `obj` is the object and `end` which end of it, for
 * FD_PIPE. */
int fd_alloc(fd_kind_t kind, void *obj, uint8_t end);

/* The entry for `fd`, or NULL if it is not open or the number is out of range.  This is
 * what every read and write starts with, so it is also the range check on the number. */
fd_entry_t *fd_get(int fd);

/* Close a descriptor.  False if it was not open, or is one of the three every process
 * starts with -- 0, 1 and 2 belong to the process table, not to the program, and a
 * program that closes its own stdout should not take the console with it. */
bool fd_close(int fd);

/* The file a descriptor names, or NULL if it does not name one.
 *
 * Separate from `fd_get()` because a caller wants to know whether a number is a *file* --
 * one that can be seeked and stat'ed -- and `fd_get()` answers the other question, which is
 * whether the number is open at all.  A pipe is open and is not a file, and a program that
 * lseeks one has to be told ENOSPC rather than have the kernel try. */
file_t *fd_file(int fd);

/* --- pipes ---------------------------------------------------------------- */

/* How many bytes a pipe holds.  One page: enough that a program writing a line and a
 * program writing in small pieces both make progress, small enough that a program can
 * fill it deliberately and see what blocking on a full pipe means. */
#define PIPE_CAPACITY 4096

typedef struct pipe pipe_t;

/* A pipe.  One ring buffer, one lock, one wait queue.
 *
 * The lock *is* the wait queue's lock, which is a requirement rather than a shortcut: a
 * reader that decides to wait has to enqueue itself while still holding the lock that
 * guards the buffer, or a writer that makes room in between wakes nobody and the reader
 * sleeps for ever with the condition already satisfied.  One lock for both is what makes
 * `wait_queue_enqueue_locked()` usable here.
 *
 * One wait queue for both ends, and every state change wakes all of them.  Two queues
 * would mean two locks and a reader woken by another reader's arrival, which is a thing to
 * get right for no gain: every waiter re-checks its own condition after waking, so a
 * spurious wake costs a loop iteration and only a missed one is a bug -- and waking all of
 * them cannot miss anybody.
 */
struct pipe {
    uint32_t     head;        /* read cursor into buf   */
    uint32_t     tail;        /* write cursor into buf  */
    uint32_t     count;       /* bytes in buf           */
    uint32_t     capacity;
    uint32_t     readers;     /* open read ends, for end-of-file */
    uint32_t     writers;     /* open write ends, for end-of-file */
    uint8_t     *buf;
    wait_queue_t waiters;
    uint64_t     bytes_read;
    uint64_t     bytes_written;
};

/* Create a pipe and put its two ends into the calling task's table, writing them to the
 * user pointer `fds`.  0, or -errno. */
long pipe_create(uint64_t fds_user);

/* Move bytes through one end.  Return the number moved, 0 at end of file, or -errno.
 *
 * `buf` is a *user* address, validated by the caller: the ring is copied to and from
 * straight into the program's memory, because a copy into a kernel bounce buffer and then
 * out again is a second chance to get the length wrong. */
long pipe_read(pipe_t *p, uint64_t buf, size_t bytes);
long pipe_write(pipe_t *p, uint64_t buf, size_t bytes);

/* Count one more open end.  A fork does this for every pipe it copies: the child holds a
 * descriptor for the same pipe, so the pipe has two users of that end and closing one
 * process's copy must not make the other process's writes fail. */
void pipe_retain(pipe_t *p, uint8_t end);

/* Drop one end.  `end` says which, and the object goes away with its last one -- a pipe
 * still named by a descriptor is reachable, and one that is not is a leak. */
void pipe_close_end(pipe_t *p, uint8_t end);

/* How many bytes are buffered, and how many ends are open.  The tests read these, and
 * they are the only way to tell a pipe that works from one that silently discarded. */
size_t   pipe_buffered(const pipe_t *p);
unsigned pipe_open_ends(const pipe_t *p);

#endif /* MINX_FD_H */