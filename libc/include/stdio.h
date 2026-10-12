/* minx - <stdio.h>
 *
 * ISO C's streams, and the formatted output the shell and the coreutils live on.
 *
 * The streams are three buffers over three descriptors, and the standard says a program may
 * assume `stdin`, `stdout` and `stderr` exist and are open.  They are: descriptors 0, 1 and 2
 * belong to the process table in this kernel, so they exist before `main()` and a program
 * cannot lose them by closing one.  What is *not* assumed is that they are a terminal --
 * `isatty()` asks, and everything here behaves the same either way, because a stream that
 * behaves differently when it is a terminal is a stream a test harness cannot use.
 */
#ifndef MINX_STDIO_H
#define MINX_STDIO_H

#include <stdarg.h>
#include <stddef.h>
#include <sys/sysdep.h>

/* --- the stream --------------------------------------------------------------- */

#define EOF (-1)

/* `FILE` is a complete type, not an opaque one, and deliberately so.
 *
 * A program that only passes a `FILE *` around never sees inside it.  A program that needs
 * to -- `setvbuf()`, `fileno()`, or its own `fmemopen()` -- needs the layout to be the same
 * as the one it was compiled against, and an opaque struct in a header is exactly as much
 * of a promise as a concrete one.  The cost is that these names cannot move. */
struct __FILE;
typedef struct __FILE FILE;

#define BUFSIZ      4096
#define EOF_VALUE   (-1)

/* Seek origins.  Defined here as well as in <unistd.h> because <stdio.h> is what ISO C
 * says defines them, and a program that includes only <stdio.h> and calls `fseek()` must
 * not fail. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Buffering modes for `setvbuf()`. */
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

/* Everything `fopen()` can be asked for, and nothing more: this filesystem has exactly one
 * kind of thing that is not a file, and it is a terminal. */
#define O_RDONLY_ 0

/* `stdin`, `stdout` and `stderr`, as addresses of real objects rather than as macros with
 * their own storage.  They are the three streams every program starts with, they are open,
 * and they are connected to descriptors 0, 1 and 2. */
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* `stdin`'s buffer is not line-buffered by default even when it is a terminal, which is
 * what ISO C says and what every system does: a program that reads a character at a time
 * from a terminal would otherwise wait for a newline it is never going to type.  An
 * interactive shell sets `setvbuf(stdin, NULL, _IONBF, 0)` for exactly this reason. */

/* --- opening and closing ------------------------------------------------------- */

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *stream);
int   fclose(FILE *stream);
int   fflush(FILE *stream);

/* The four standard streams and the twenty-slot array behind them.  A fixed table rather
 * than an allocated one: `stdin` must exist before any allocation can be relied on, and a
 * program that opens more than twenty files at once is a program that can be told so. */
#define FOPEN_MAX 20

/* --- reading and writing -------------------------------------------------------- */

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

int    fgetc(FILE *stream);
int    getc(FILE *stream);
int    getchar(void);
char  *fgets(char *buf, int size, FILE *stream);
int    ungetc(int c, FILE *stream);
int    fputc(int c, FILE *stream);
int    putc(int c, FILE *stream);
int    putchar(int c);
int    fputs(const char *s, FILE *stream);
int    puts(const char *s);

/* `getc` and `putc` are macros in every other libc, for the same reason they are here:
 * they are the two a program calls inside its innermost loop, and a macro avoids a call
 * that does nothing but dispatch.  The functions exist too, so a program that takes their
 * address still links.  `_getc`/`_putc` are the names behind the macros. */
int _getc(FILE *stream);
int _putc(int c, FILE *stream);
#define getc(s)  _getc(s)
#define putc(c, s) _putc((c), (s))

/* --- positioning ----------------------------------------------------------------- */

int    fseek(FILE *stream, long offset, int whence);
long   ftell(FILE *stream);
void   rewind(FILE *stream);

int    feof(FILE *stream);
int    ferror(FILE *stream);
void   clearerr(FILE *stream);
void   perror(const char *prefix);

/* --- formatting ------------------------------------------------------------------ */

int printf(const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *stream, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

int scanf(const char *fmt, ...);
int fscanf(FILE *stream, const char *fmt, ...);
int sscanf(const char *s, const char *fmt, ...);
int vscanf(const char *fmt, va_list ap);
int vfscanf(FILE *stream, const char *fmt, va_list ap);
int vsscanf(const char *s, const char *fmt, va_list ap);

/* `asprintf()` and `vasprintf()` are the two GNU extensions every program that builds a
 * string from a format eventually reaches for.  The allocation is the caller's to free,
 * and a program that forgets is the same program that forgets with `strdup()`. */
int asprintf(char **out, const char *fmt, ...);
int vasprintf(char **out, const char *fmt, va_list ap);

/* --- the buffer and the descriptor ----------------------------------------------- */

/* `setvbuf()` is here and works, and the third case is the one worth knowing about: a NULL
 * buffer with `_IONBF` turns the stream into one `write()` per call, which is what an
 * interactive program wants for output that has to appear before the next thing the user
 * types. */
int   setvbuf(FILE *stream, char *buf, int mode, size_t size);
int   fileno(FILE *stream);
FILE *popen(const char *command, const char *mode);
int   pclose(FILE *stream);
/* `setmode()` is an MSVCRT name for a question this machine cannot answer: on every other
 * system it chooses between text and binary translation, and here there is one kind of
 * file and the terminal translates nothing.  It returns 0, which is `O_TEXT`, because that
 * is the answer to the question it is being asked and a program that checks deserves one. */
int   setmode(int fd, int mode);

#endif /* MINX_STDIO_H */