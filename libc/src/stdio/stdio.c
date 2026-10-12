/* minx - the streams.
 *
 * A `FILE` is a descriptor, a buffer and three pieces of state.  That is the whole
 * design, and it is the design because it is the one that makes `fgets()` on a terminal
 * work: the buffer is what lets a `fgetc()` be a memory read instead of a syscall, and it
 * is also what makes it *not* a memory read -- a program that reads a line at a time has to
 * see the whole line before it is complete, and that is the buffer's job, not `read()`'s.
 *
 * The decisions that matter, written where they are made:
 *
 *   - There is no locking.  One process, and its streams are its own.  Two threads writing
 *     to one `FILE` is not a thing this kernel can happen yet, and a lock that is always
 *     uncontended is a cost on the hottest path in the libc for a case that cannot occur.
 *
 *   - `stdout` is line-buffered when it is a terminal and fully buffered otherwise, which
 *     is what ISO C requires and what makes `ls | less` not do strange things.
 *
 *   - Reading is unbuffered by default.  This is the one place where a terminal is handled
 *     differently from a pipe on purpose: a line-buffered `stdin` makes a program that
 *     reads a character at a time wait for a newline nobody is going to type, which is the
 *     bug every interactive program has had to work around since 1970.
 */
#include <errno.h>
#include <fcntl.h>
#include <internal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* --- the stream ------------------------------------------------------------------ */

struct __FILE {
    int      fd;
    unsigned flags;          /* the fopen mode, kept for setvbuf() and for seeking */
    unsigned char *buf;      /* the buffer, or NULL when there is none */
    size_t   bufsize;
    int      bufmode;        /* _IOFBF, _IOLBF, _IONBF */
    int      ownbuf;         /* 1 when malloc() gave us the buffer and we must free it */

    /* Read side: where the next `read()` should put bytes, and how many of the buffer's
     * bytes are still unread.  `bufpos >= bufend` means "empty, refill". */
    size_t   bufpos;
    size_t   bufend;

    /* Write side: how much of the buffer is used.  Zero means "flush before writing". */
    size_t   bufused;

    /* One byte of pushback.  `ungetc()` is specified to work at least once, and a stream
     * that cannot push back cannot implement `scanf()`'s `%c` without one. */
    int      ungot;
    int      has_ungot;

    int      eof;
    int      error;
};

static FILE streams[FOPEN_MAX];
static int  streams_used;

/* Descriptors 0, 1 and 2 are already open in every process this kernel starts, and they
 * belong to the process table rather than to the program -- so a program that closes its
 * own stdout loses its output rather than taking the console away from everything else.
 * That is why they are `closable == false` in the kernel, and why these three exist here
 * at all rather than being opened by `fopen()`. */
/* The three standard streams, as one array with *external* linkage.
 *
 * Not `static`, and that is the whole point rather than a style choice.  `stdin`, `stdout`
 * and `stderr` are defined here and referenced by every program, so in a static link the
 * linker gives the program its own copies of the three pointers and fills them in from
 * these definitions.  With the array `static`, each initialiser was a relocation against a
 * symbol the linker cannot see -- a local one -- and it emitted **zeros**:
 *
 *     stdout = 0
 *
 * so `printf()` wrote through a NULL stream, `fileno()` answered -1, and `write(1, ...)`
 * from the same program worked perfectly.  A global array makes the initialiser a
 * relocation against a visible symbol and the copy is filled in correctly.
 *
 * Found by `libctest`, which is the first program here to link libc and the first to notice
 * that the libc's own globals were empty in a program that was otherwise fine.
 *
 * The three are initialised by hand rather than by a loop, so each one's descriptor is next
 * to its name in the source.  `-1` is the pushed-back byte's "nothing" value, because 0 is
 * NUL and is a perfectly good byte to push back. */
FILE _minx_stdio[3] = {
    { .fd = 0, .bufmode = _IOFBF, .ungot = -1 },
    { .fd = 1, .bufmode = _IOFBF, .ungot = -1 },
    { .fd = 2, .bufmode = _IOFBF, .ungot = -1 },
};

FILE *stdin  = &_minx_stdio[0];
FILE *stdout = &_minx_stdio[1];
FILE *stderr = &_minx_stdio[2];

/* Give a stream a buffer, once, at the first thing that needs one.
 *
 * The three standard streams get theirs here rather than being opened, so that the first
 * `printf()` in a program works without the program having called anything.  The size is
 * `BUFSIZ`, and nothing makes it bigger: a bigger buffer helps a program that writes a
 * megabyte in one call, and that program should be using `write()`. */
static void stream_buffer(FILE *f) {
    if (f->buf != NULL || f->bufmode == _IONBF)
        return;

    f->buf = malloc(BUFSIZ);

    if (f->buf == NULL) {
        /* No buffer is not a failure.  It turns every call into a syscall, which is slower
         * and still correct, and the alternative -- failing the write because the
         * allocator said no -- would make a memory shortage into a lost line of output. */
        f->bufmode = _IONBF;
        return;
    }

    f->bufsize = BUFSIZ;
    f->ownbuf  = 1;
}

/* --- writing ---------------------------------------------------------------------- */

/* Hand the buffer to the kernel, all of it. */
static int stream_flush(FILE *f) {
    if (f->bufused == 0)
        return 0;

    size_t done = 0;

    while (done < f->bufused) {
        ssize_t n = write(f->fd, f->buf + done, f->bufused - done);

        if (n < 0) {
            /* EINTR is not a failure: the write did not happen and the bytes are still
             * ours.  Retrying is what makes a program survive Ctrl-C in the middle of
             * printing its own prompt. */
            if (errno == EINTR)
                continue;

            f->error = 1;
            f->bufused = 0;
            return EOF;
        }

        if (n == 0)
            break;              /* no progress: a full pipe with nobody reading it */

        done += (size_t)n;
    }

    f->bufused = 0;
    return 0;
}

/* An unbuffered stream writes straight through.  That is the whole point of it: a prompt
 * the user has to wait to see until the buffer fills is not a prompt. */
static int stream_write_raw(FILE *f, const unsigned char *p, size_t n) {
    size_t done = 0;

    while (done < n) {
        ssize_t w = write(f->fd, p + done, n - done);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            f->error = 1;
            return EOF;
        }

        if (w == 0)
            break;

        done += (size_t)w;
    }

    return (int)done;
}

int fflush(FILE *f) {
    if (f == NULL) {
        /* NULL means "all of them", which is what a program calls before it does something
         * that will not return -- an `exec`, a crash handler -- and which has to reach the
         * streams a program never named. */
        int r = 0;

        for (int i = 0; i < streams_used; i++)
            if (fflush(&streams[i]) != 0)
                r = EOF;

        if (stream_flush(stdout) != 0)
            r = EOF;
        if (stream_flush(stderr) != 0)
            r = EOF;

        return r;
    }

    return stream_flush(f);
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    const unsigned char *p = ptr;
    size_t total = size * nmemb;

    if (size == 0 || nmemb == 0 || f == NULL)
        return 0;

    if (f->bufmode == _IONBF || f->buf == NULL) {
        int n = stream_write_raw(f, p, total);

        return n < 0 ? 0 : (size_t)n / size;
    }

    size_t done = 0;

    while (done < total) {
        size_t room = f->bufsize - f->bufused;

        if (room == 0) {
            /* A full buffer, and the buffer holds more than is left.  Writing the buffer
             * and starting again is right for anything bigger than the buffer; for
             * something smaller it would turn one `fwrite()` into two syscalls, which is
             * why the `room == 0` check is *before* the fill rather than after it. */
            if (stream_flush(f) != 0)
                break;

            room = f->bufsize - f->bufused;
        }

        size_t take = total - done;

        if (take > room)
            take = room;

        memcpy(f->buf + f->bufused, p + done, take);
        f->bufused += take;
        done += take;

        /* Line-buffered means *only* line-buffered: a newline anywhere in what was just
         * added is enough.  The usual implementation flushes at the end of a line, which
         * loses everything after the last newline in the buffer -- and that is the whole
         * line a program just asked to have appear. */
        if (f->bufmode == _IOLBF && memchr(f->buf + f->bufused - take, '\n', take) != NULL)
            stream_flush(f);
    }

    return done / size;
}

int fputc(int c, FILE *f) {
    unsigned char ch = (unsigned char)c;

    if (f->bufmode == _IONBF || f->buf == NULL)
        return stream_write_raw(f, &ch, 1) == 1 ? (int)ch : EOF;

    if (f->bufused == f->bufsize && stream_flush(f) != 0)
        return EOF;

    f->buf[f->bufused++] = ch;

    if (f->bufmode == _IOLBF && ch == '\n')
        stream_flush(f);

    return (int)ch;
}

int _putc(int c, FILE *f) {
    return fputc(c, f);
}

int putchar(int c) {
    return fputc(c, stdout);
}

int fputs(const char *s, FILE *f) {
    size_t n = strlen(s);

    if (fwrite(s, 1, n, f) != n)
        return EOF;

    return (int)n;
}

int puts(const char *s) {
    if (fputs(s, stdout) == EOF)
        return EOF;

    /* `puts()` adds the newline; `fputs()` does not.  Every other libc does this and it is
     * the single most common source of a doubled blank line in a program that was not sure
     * which one it called. */
    if (fputc('\n', stdout) == EOF)
        return EOF;

    return (int)strlen(s) + 1;
}

/* --- reading ---------------------------------------------------------------------- */

/* Fill the buffer, unless there is something in it or something pushed back. */
static int stream_refill(FILE *f) {
    if (f->bufpos < f->bufend)
        return 1;

    f->bufpos = 0;
    f->bufend = 0;

    if (f->buf == NULL || f->bufmode == _IONBF)
        return 0;               /* the caller reads the descriptor directly */

    for (;;) {
        ssize_t n = read(f->fd, f->buf, f->bufsize);

        if (n < 0) {
            /* EINTR is not EOF and not an error.  A program that reads a terminal and has
             * a SIGINT handler must get its next character after the handler, not a
             * permanent -1, and treating it as either of those is the bug. */
            if (errno == EINTR)
                continue;

            f->error = 1;
            return 0;
        }

        if (n == 0) {
            f->eof = 1;
            return 0;
        }

        f->bufend = (size_t)n;
        return 1;
    }
}

/* One character, from the pushed-back byte, the buffer, or the descriptor.
 *
 * The three sources are in that order and the order is the whole of `ungetc()`: a byte
 * that was pushed back is the next byte, whatever the buffer holds, because that is what
 * makes `ungetc()` able to say "no, that was not the next character". */
static int stream_getc(FILE *f, int *out) {
    if (f->has_ungot) {
        *out = f->ungot;
        f->has_ungot = 0;
        f->eof = 0;            /* a pushed-back byte clears EOF: there is more again */
        return 1;
    }

    if (f->buf == NULL || f->bufmode == _IONBF) {
        unsigned char c;

        /* One byte at a time from the descriptor.  This is the `stdin` path for a
         * terminal, and it is why an interactive program works: the character arrives when
         * the user types it, not when a buffer happens to fill. */
        for (;;) {
            ssize_t n = read(f->fd, &c, 1);

            if (n < 0) {
                if (errno == EINTR)
                    continue;
                f->error = 1;
                return 0;
            }

            if (n == 0) {
                f->eof = 1;
                return 0;
            }

            *out = c;
            return 1;
        }
    }

    if (!stream_refill(f))
        return 0;

    *out = f->buf[f->bufpos++];
    return 1;
}

int fgetc(FILE *f) {
    int c;

    if (f != NULL && stream_getc(f, &c))
        return c;

    return EOF;
}

int _getc(FILE *f) {
    return fgetc(f);
}

int getchar(void) {
    return fgetc(stdin);
}

char *fgets(char *buf, int size, FILE *f) {
    if (buf == NULL || size <= 0)
        return NULL;

    int i = 0;

    /* Until the newline or the size, whichever comes first.  `size` is the whole buffer
     * including the terminator, so a line of exactly `size - 1` characters plus a newline
     * is split across two calls -- which is what ISO C says and what every program that
     * handles long lines already knows. */
    while (i < size - 1) {
        int c = fgetc(f);

        if (c == EOF)
            break;

        buf[i++] = (char)c;

        if (c == '\n')
            break;
    }

    if (i == 0)
        return NULL;

    buf[i] = 0;

    return buf;
}

int ungetc(int c, FILE *f) {
    /* EOF cannot be pushed back -- there is no byte to push -- and neither can two.  Both
     * are specified to fail rather than to corrupt the stream, and returning EOF is how a
     * caller finds out. */
    if (c == EOF || f->has_ungot)
        return EOF;

    f->ungot = (unsigned char)c;
    f->has_ungot = 1;

    return (unsigned char)c;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
    unsigned char *p = ptr;
    size_t total = size * nmemb;
    size_t done = 0;

    if (size == 0 || nmemb == 0 || p == NULL)
        return 0;

    while (done < total) {
        int c = fgetc(f);

        if (c == EOF)
            break;

        p[done++] = (unsigned char)c;
    }

    /* Whole records, not bytes.  A `fread()` that returned a partial record would leave
     * the caller dividing a remainder it cannot use, and ISO C says the count is of
     * complete items. */
    return done / size;
}

/* --- opening and closing ------------------------------------------------------------- */

/* Turn an `fopen()` mode string into open flags and a buffer mode.
 *
 * Every mode is a sequence, so `"r+"` is read-write and `"w+b"` is write-binary and
 * nothing here has to enumerate the pairs.  `b` is accepted and ignored, because it means
 * nothing on a system with one kind of file and refusing it would break programs that pass
 * it. */
static int parse_mode(const char *mode, unsigned *flags_out, int *bufmode) {
    if (mode == NULL || mode[0] == 0) {
        errno = EINVAL;
        return -1;
    }

    unsigned flags;
    int      creating = 0;
    int      truncating = 0;

    switch (mode[0]) {
    case 'r':
        flags = O_RDONLY;
        break;
    case 'w':
        flags     = O_WRONLY | O_CREAT | O_TRUNC;
        creating  = 1;
        truncating = 1;
        break;
    case 'a':
        flags    = O_WRONLY | O_CREAT | O_APPEND;
        creating = 1;
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    for (const char *p = mode + 1; *p != 0; p++) {
        if (*p == '+') {
            /* `O_RDWR` replaces the access mode rather than being added to it: `O_RDONLY |
             * O_RDWR` is `O_RDWR` on every system, but relying on that is relying on an
             * accident of the encoding. */
            flags = (flags & ~(unsigned)O_ACCMODE) | O_RDWR;
        } else if (*p == 'b' || *p == 't') {
            /* Ignored: there is one kind of file, so "binary" and "text" are the same
             * thing, and a program that passes 'b' must not fail for it. */
        } else if (*p == 'x' && creating) {
            flags |= O_EXCL;    /* only for "wx", which is the mode that has a meaning */
        } else if (*p == 'e') {
            flags |= O_CLOEXEC;
        } else if (*p != 0) {
            errno = EINVAL;
            return -1;
        }
    }

    (void)truncating;
    *flags_out = flags;
    *bufmode   = _IOFBF;

    return 0;
}

FILE *fopen(const char *path, const char *mode) {
    unsigned flags;
    int      bufmode;

    if (parse_mode(mode, &flags, &bufmode) != 0)
        return NULL;

    /* The mode for a *new* file is 0666 masked by the umask, which is what every other
     * Unix does: a file created by a program that did not say should be readable by
     * everyone the umask allows, and a program that wants otherwise says so. */
    int fd = open(path, (int)flags, 0666);

    if (fd < 0)
        return NULL;

    /* One slot per open stream.  A program that runs out gets NULL with EMFILE set, which
     * is a real errno and tells the program what happened -- rather than a NULL with
     * nothing set, which is the same thing a failed `open()` looks like. */
    if (streams_used >= FOPEN_MAX) {
        close(fd);
        errno = EMFILE;
        return NULL;
    }

    FILE *f = &streams[streams_used++];

    memset(f, 0, sizeof(*f));
    f->fd      = fd;
    f->flags   = flags;
    f->bufmode = bufmode;
    f->ungot   = -1;

    stream_buffer(f);

    return f;
}

FILE *fdopen(int fd, const char *mode) {
    unsigned flags;
    int      bufmode;

    if (parse_mode(mode, &flags, &bufmode) != 0)
        return NULL;

    if (streams_used >= FOPEN_MAX) {
        errno = EMFILE;
        return NULL;
    }

    FILE *f = &streams[streams_used++];

    memset(f, 0, sizeof(*f));
    f->fd      = fd;
    f->flags   = flags;
    f->bufmode = bufmode;
    f->ungot   = -1;

    stream_buffer(f);

    return f;
}

int fclose(FILE *f) {
    if (f == NULL) {
        errno = EINVAL;
        return EOF;
    }

    /* Flushed before anything else, because closing first would discard the buffer and
     * the last line a program printed would be the one that disappeared. */
    if (stream_flush(f) != 0)
        return EOF;

    int r = 0;

    if (f >= &streams[0] && f < &streams[FOPEN_MAX]) {
        close(f->fd);

        /* Compacting the table: the array is the storage *and* the open list, so a closed
         * stream has to be removed rather than left as a hole that `fflush(NULL)` would
         * walk into. */
        size_t idx = (size_t)(f - &streams[0]);

        for (size_t i = idx; i + 1 < (size_t)streams_used; i++)
            streams[i] = streams[i + 1];

        streams_used--;
        r = 0;
    } else {
        /* One of the three standard streams.  They are not closed: descriptors 0, 1 and 2
         * belong to the process table, and a program that closes its stdout should lose
         * its output rather than take the console away from every other process. */
        if (f->ownbuf) {
            free(f->buf);
            f->buf = NULL;
            f->ownbuf = 0;
        }

        f->bufused = 0;
        f->bufpos = f->bufend = 0;
    }

    return r;
}

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    if (f == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF) {
        errno = EINVAL;
        return -1;
    }

    /* Anything already buffered is written out first: the caller is changing how output
     * is accumulated, and what is already accumulated has nowhere to go but the
     * descriptor. */
    if (stream_flush(f) != 0)
        return -1;

    if (f->ownbuf) {
        free(f->buf);
        f->buf = NULL;
        f->ownbuf = 0;
    }

    f->bufmode = mode;
    f->bufsize = 0;
    f->bufused = 0;
    f->bufpos = f->bufend = 0;

    if (mode == _IONBF) {
        f->buf = NULL;
        return 0;
    }

    if (buf != NULL && size > 0) {
        f->buf     = (unsigned char *)buf;
        f->bufsize = size;
        f->ownbuf  = 0;          /* the caller's buffer, and the caller's to free */
        return 0;
    }

    stream_buffer(f);

    /* A `setvbuf` that could not get a buffer is not a failure, for the same reason
     * `stream_buffer()` does not fail: an unbuffered stream is slower, not wrong. */
    return 0;
}

int fileno(FILE *f) {
    if (f == NULL) {
        errno = EINVAL;
        return -1;
    }

    return f->fd;
}

int fseek(FILE *f, long offset, int whence) {
    if (f == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (stream_flush(f) != 0)
        return -1;

    /* The buffer is about to be the wrong thing: its contents belong to an offset that is
     * no longer current, so it is dropped rather than kept. */
    f->bufpos = f->bufend = 0;
    f->eof = 0;

    return lseek(f->fd, offset, whence) < 0 ? -1 : 0;
}

long ftell(FILE *f) {
    if (f == NULL) {
        errno = EINVAL;
        return -1;
    }

    off_t here = lseek(f->fd, 0, SEEK_CUR);

    if (here < 0)
        return -1;

    /* The logical position is where the descriptor is, minus what is in the buffer but not
     * yet consumed.  Getting the sign wrong here is the classic bug: a `ftell()` that
     * *adds* the buffer size reports a position past the end of the file after a read. */
    return (long)here - (long)(f->bufend - f->bufpos);
}

void rewind(FILE *f) {
    if (f == NULL)
        return;

    fseek(f, 0, SEEK_SET);
    clearerr(f);
}

int feof(FILE *f) {
    return f != NULL && f->eof;
}

int ferror(FILE *f) {
    return f != NULL && f->error;
}

void clearerr(FILE *f) {
    if (f == NULL)
        return;

    f->eof = 0;
    f->error = 0;
}

int __stdio_fd_count(void) {
    return streams_used;
}

/* `fflush(NULL)` above needs to reach the standard streams, which live outside the
 * `streams` array -- so this is the one place that knows about all four kinds of stream. */
void __stdio_flush_all(void) {
    for (int i = 0; i < streams_used; i++)
        stream_flush(&streams[i]);

    stream_flush(&_minx_stdio[0]);
    stream_flush(&_minx_stdio[1]);
    stream_flush(&_minx_stdio[2]);
}

/* --- the mode the streams start in ----------------------------------------------- */

/* Called once, from `__libc_start_main()`, before `main()`.
 *
 * The decision made here is the one every libc makes and every interactive program has to
 * know about: `stdout` and `stderr` are line-buffered when they are a terminal and fully
 * buffered otherwise.  A pipeline gets full buffering because a program writing into a pipe
 * should do it in 4 KiB writes rather than one byte at a time; a terminal gets line
 * buffering because a prompt is a line and has to appear when it is written.
 *
 * `stdin` is *not* buffered at all.  That is the opposite decision and it is what makes a
 * character-at-a-time program work: with a buffer, `getchar()` would block until the buffer
 * filled, and on a terminal that means until the user typed a newline. */
void __stdio_init(void) {
    struct winsize ws;

    stream_buffer(&_minx_stdio[0]);
    stream_buffer(&_minx_stdio[1]);
    stream_buffer(&_minx_stdio[2]);

    if (ioctl(1, TIOCGWINSZ, &ws) == 0)
        _minx_stdio[1].bufmode = _IOLBF;

    if (ioctl(2, TIOCGWINSZ, &ws) == 0)
        _minx_stdio[2].bufmode = _IOLBF;

    _minx_stdio[0].bufmode = _IONBF;

    /* `stdout` and `stderr` share a stream when both are the same descriptor -- which they
     * are, for any program whose caller did not separate them.  The one that must not be
     * buffered across a fork is `stdout`, because a child that writes more after the fork
     * would otherwise emit what its parent had accumulated.  `stderr` is left alone: it is
     * unbuffered by convention and a duplicated buffer of it costs nothing. */
    if (_minx_stdio[1].fd == 2) {
        stream_flush(&_minx_stdio[1]);
    }
}