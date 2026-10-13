/* minx - descriptors and pipes.  See fd.h for what this is for and why the pieces are
 * shaped the way they are. */
#include "fd.h"

#include "kmalloc.h"
#include "vfs.h"
#include "serial.h"
#include "spinlock.h"
#include "string.h"
#include "syscall.h"
#include "task.h"

/* --- the table ----------------------------------------------------------- */


fd_entry_t *fd_table(void) {
    task_t *self = sched_current();

    return self != NULL ? self->fds : NULL;
}

void fd_table_init_for_new_task(void) {
    fd_entry_t *t = fd_table();

    if (t == NULL)
        return;

    memset(t, 0, FD_TABLE_ENTRIES * sizeof(*t));

    /* The three every process starts with, all naming the console.
     *
     * Not `closable`: they are not the program's to close, and a program that closes its
     * own stdout should lose its output rather than the console for every other process
     * on the machine. */
    for (int i = 0; i < 3; i++) {
        t[i].kind = FD_CONSOLE;
        t[i].closable = false;
    }
}

void fd_table_copy(fd_entry_t *dst, const fd_entry_t *src) {
    memcpy(dst, src, FD_TABLE_ENTRIES * sizeof(*dst));

    /* Every copied pipe descriptor is another open end of that pipe.
     *
     * Without this the pipe's idea of who is reading it is the parent's alone, and the
     * first thing the child does -- close its inherited copy of the read end, which every
     * program does, because otherwise it could never see end of file -- takes the pipe's
     * reader count to zero while the parent is still holding it open.  The parent's next
     * write then comes back EPIPE, against a reader that is right there holding the
     * descriptor.  The count is of ends, and a fork made one more. */
    for (int i = 0; i < FD_TABLE_ENTRIES; i++) {
        if (dst[i].kind == FD_PIPE)
            pipe_retain((pipe_t *)dst[i].obj, dst[i].pipe_end);
        else if (dst[i].kind == FD_FILE)
            vfs_file_retain((file_t *)dst[i].obj);
    }
}

int fd_alloc(fd_kind_t kind, void *obj, uint8_t end) {
    fd_entry_t *t = fd_table();

    if (t == NULL)
        return -1;

    /* Lowest free, the same rule every system uses, so a program's idea of which numbers
     * are taken does not depend on this kernel. */
    for (int i = 0; i < FD_TABLE_ENTRIES; i++) {
        if (t[i].kind != FD_NONE)
            continue;

        t[i].kind = kind;
        t[i].obj = obj;
        t[i].pipe_end = end;
        t[i].closable = true;
        return i;
    }

    return -1;
}

fd_entry_t *fd_get(int fd) {
    fd_entry_t *t = fd_table();

    if (t == NULL || fd < 0 || fd >= FD_TABLE_ENTRIES)
        return NULL;

    return t[fd].kind == FD_NONE ? NULL : &t[fd];
}

/* The file a descriptor names, or NULL.  See fd.h for why this is not fd_get(). */
file_t *fd_file(int fd) {
    fd_entry_t *e = fd_get(fd);

    if (e == NULL || e->kind != FD_FILE)
        return NULL;

    return (file_t *)e->obj;
}

bool fd_close(int fd) {
    fd_entry_t *t = fd_table();

    if (t == NULL || fd < 0 || fd >= FD_TABLE_ENTRIES)
        return false;

    fd_entry_t *e = &t[fd];

    if (e->kind == FD_NONE || !e->closable)
        return false;

    /* The object first, then the number.  A pipe's readers are waiting to be told the
     * write end is gone, and a reader that is woken before the count is right sees a
     * pipe that still has writers and goes back to sleep.  A file is closed here too --
     * its buffer is freed, because a file that has been unlinked and closed has nothing
     * left that could reach it. */
    if (e->kind == FD_PIPE)
        pipe_close_end((pipe_t *)e->obj, e->pipe_end);
    else if (e->kind == FD_FILE)
        vfs_close((file_t *)e->obj);

    e->kind = FD_NONE;
    e->obj = NULL;
    return true;
}

/* --- pipes --------------------------------------------------------------- */

/* Bring a cursor back inside the buffer.
 *
 * A ring has two representations of "the end": `capacity`, where the arithmetic naturally
 * lands after the last byte, and 0, which is the same place.  A cursor allowed to sit at
 * `capacity` makes the room before it compute as zero, and a copy that sees zero room and
 * zero to do spins for ever holding the pipe's lock -- so the writer never returns, and
 * every reader that comes afterwards blocks on that same lock.  The whole pipe appears to
 * stop working: nothing faults, nothing panics, and the data already in the buffer is
 * still readable by a task that got there first.
 *
 * Both sides normalise on the way in rather than after every byte, because the cursor is
 * only ever *left* at `capacity` by a copy that finished exactly on the boundary. */
static inline uint32_t pipe_cursor(uint32_t at) {
    return at >= PIPE_CAPACITY ? 0 : at;
}

static void pipe_ring_copy_in(pipe_t *p, uint64_t user, size_t n) {
    /* In pieces, because the user buffer does not have to line up with the buffer's wrap.
     * A whole-buffer `memcpy` would be faster and wrong at the seam, which is exactly where
     * a pipe is used. */
    size_t done = 0;

    while (done < n) {
        /* Normalised on *every* pass, not just the first.
         *
         * The room left before the end of the buffer is what says how much can be copied
         * in one piece, and a cursor sitting at `capacity` reports none.  That happens
         * part-way through a copy whose data wraps: the run reaches the end of the buffer,
         * the cursor lands on `capacity`, and the next pass sees zero room.  With the
         * normalisation only on entry the loop stops there, having copied part of what it
         * was asked for -- and the caller, which has already decided how many bytes to move,
         * books the whole number against the buffer's contents.  Bytes are lost, the count
         * is wrong, and both show up much later as a checksum that does not add up. */
        p->tail = pipe_cursor(p->tail);

        size_t room = p->capacity - p->tail;
        size_t chunk = n - done;

        if (chunk > room)
            chunk = room;

        if (chunk == 0)
            break;              /* unreachable with both cursors normalised */

        memcpy(p->buf + p->tail, (const void *)(uintptr_t)(user + done), chunk);
        p->tail = (uint32_t)((p->tail + chunk) % p->capacity);
        done += chunk;
    }
}

static void pipe_ring_copy_out(pipe_t *p, uint64_t user, size_t n) {
    size_t done = 0;

    while (done < n) {
        /* Normalised every pass, for the reason pipe_ring_copy_in() gives. */
        p->head = pipe_cursor(p->head);

        /* How far the copy may run before it reaches the end of the buffer.
         *
         * The bytes in a pipe are the run `[head, head + count)`, so the part of it before
         * the buffer's end is `capacity - head`, capped by what is actually there.  Both
         * halves matter, and the second one especially: with a full ring `head` equals
         * `tail`, and a run length computed as a distance *between* the cursors is then
         * zero.  A reader asking for one byte out of a full pipe would spin for ever
         * copying nothing -- which looks exactly like a pipe that has stopped working,
         * because the writer is blocked on a full pipe and nothing is draining it. */
        size_t until = p->capacity - p->head;
        size_t chunk = n - done;

        if (until > p->count)
            until = p->count;
        if (chunk > until)
            chunk = until;
        if (chunk == 0)
            break;
        if (chunk > until)
            chunk = until;

        memcpy((void *)(uintptr_t)(user + done), p->buf + p->head, chunk);
        p->head = (uint32_t)((p->head + chunk) % p->capacity);
        done += chunk;
    }
}


long pipe_read(pipe_t *p, uint64_t buf, size_t bytes) {
    if (bytes == 0)
        return 0;

    for (;;) {
        ticket_lock(&p->waiters.lock);

        if (p->count > 0) {
            /* What there is, which may be less than asked for.  A pipe that made a
             * reader wait for all of a count would deadlock against its own buffer: the
             * writer has no way to know how much the reader wants. */
            size_t n = p->count < bytes ? p->count : bytes;

            pipe_ring_copy_out(p, buf, n);
            p->count -= (uint32_t)n;
            p->bytes_read += n;

            ticket_unlock(&p->waiters.lock);

            /* Room now exists, so anything waiting to write may go.  Waking needs the
             * lock this call just dropped -- the queue's lock is the pipe's lock. */
            wait_queue_wake_all(&p->waiters);
            return (long)n;
        }

        /* Nothing buffered and nobody left to fill it: end of file.
         *
         * Not an error and not a block.  A reader on a pipe whose write ends are all
         * closed would otherwise wait for ever on a condition that can never hold, which
         * is what makes `cat a | nothing` terminate rather than hang. */
        if (p->writers == 0) {
            ticket_unlock(&p->waiters.lock);
            return 0;
        }

        /* Empty but still open, so there is something to wait for.  Enqueued under the
         * lock that guards the buffer: a writer cannot make room before the slot exists,
         * so there is no window in which this wake is lost. */
        wait_queue_block_locked(&p->waiters,
                                wait_queue_enqueue_locked(&p->waiters, 1));
            }
}

long pipe_write(pipe_t *p, uint64_t buf, size_t bytes) {
    if (bytes == 0)
        return 0;

    for (;;) {
        ticket_lock(&p->waiters.lock);

        if (p->count < p->capacity) {
            size_t room = p->capacity - p->count;
            size_t n = room < bytes ? room : bytes;

            pipe_ring_copy_in(p, buf, n);
            p->count += (uint32_t)n;
            p->bytes_written += n;

            ticket_unlock(&p->waiters.lock);

            /* Something to read now. */
            wait_queue_wake_all(&p->waiters);
            return (long)n;
        }

        /* Full.  Unlike a read end, a full write end has no end-of-file to fall back on:
         * there is no way to write, and the only honest answer is to wait for room.  A
         * `write` to a pipe whose reader has gone away returns EPIPE instead, which is
         * what tells a program its output is going nowhere. */
        if (p->readers == 0) {
            ticket_unlock(&p->waiters.lock);
            errno_return(EPIPE);
        }

        wait_queue_block_locked(&p->waiters,
                                wait_queue_enqueue_locked(&p->waiters, 1));
            }
}

void pipe_retain(pipe_t *p, uint8_t end) {
    ticket_lock(&p->waiters.lock);

    if (end == PIPE_READ)
        p->readers++;
    else
        p->writers++;

    ticket_unlock(&p->waiters.lock);
}

void pipe_close_end(pipe_t *p, uint8_t end) {
    bool last;

    ticket_lock(&p->waiters.lock);

    if (end == PIPE_READ) {
        if (p->readers > 0)
            p->readers--;
    } else {
        if (p->writers > 0)
            p->writers--;
    }

    last = (p->readers == 0 && p->writers == 0);
    ticket_unlock(&p->waiters.lock);

    /* Woken with the counts already right, so a reader that sees "no writers" takes the
     * end-of-file path rather than going back to sleep on a pipe nobody can fill. */
    wait_queue_wake_all(&p->waiters);

    /* The last end goes, and the object with it.
     *
     * Freed here rather than left to the last descriptor's task, because a pipe is not
     * owned by a task: it is named by descriptors in several tasks at once, and there is
     * no single one whose death should end it.  The count is what says. */
    if (last) {
        kfree(p->buf);
        kfree(p);
    }
}

size_t pipe_buffered(const pipe_t *p) {
    uint64_t flags = spin_lock_irqsave((ticket_lock_t *)&p->waiters.lock);

    size_t n = p->count;

    spin_unlock_irqrestore((ticket_lock_t *)&p->waiters.lock, flags);
    return n;
}

unsigned pipe_open_ends(const pipe_t *p) {
    uint64_t flags = spin_lock_irqsave((ticket_lock_t *)&p->waiters.lock);

    unsigned n = p->readers + p->writers;

    spin_unlock_irqrestore((ticket_lock_t *)&p->waiters.lock, flags);
    return n;
}

long pipe_create(uint64_t fds_user) {
    int rfds[2];

    if (fd_table() == NULL)
        errno_return(EBADF);

    pipe_t *p = kmalloc(sizeof(*p));

    if (p == NULL)
        errno_return(ENOMEM);

    p->buf = kmalloc(PIPE_CAPACITY);

    if (p->buf == NULL) {
        kfree(p);
        errno_return(ENOMEM);
    }

    p->head = 0;
    p->tail = 0;
    p->count = 0;
    p->capacity = PIPE_CAPACITY;
    p->readers = 1;
    p->writers = 1;
    p->bytes_read = 0;
    p->bytes_written = 0;
    wait_queue_init(&p->waiters);

    /* Both ends allocated before either is stored: if the second one does not fit, the
     * first has to be given back and the pipe has to be torn down, and doing that from
     * one allocation path is easier to get right than doing it from two. */
    int r = fd_alloc(FD_PIPE, p, PIPE_READ);

    if (r < 0)
        goto fail;

    int w = fd_alloc(FD_PIPE, p, PIPE_WRITE);

    if (w < 0) {
        fd_close(r);
        goto fail;
    }

    rfds[0] = r;
    rfds[1] = w;

    if (!syscall_copy_to_user(fds_user, rfds, sizeof(rfds))) {
        fd_close(w);
        fd_close(r);
        errno_return(EFAULT);
    }

    return 0;

fail:
    pipe_close_end(p, PIPE_READ);
    pipe_close_end(p, PIPE_WRITE);
    errno_return(ENOMEM);
}