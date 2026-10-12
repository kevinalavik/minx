/* minx - <setjmp.h>
 *
 * `setjmp()` and `longjmp()`, and the signal-aware pair beside them.
 *
 * The buffer is visible and its layout is fixed, because it cannot be anything else: a
 * `longjmp()` in another translation unit has to restore exactly the callee-saved registers
 * the ABI says a call may clobber, plus the stack pointer, plus where to return to.  That
 * is seven registers and two words on this ABI, and the structure below names all nine so
 * that a program which corks a `jmp_buf` in a structure knows how big it is.
 *
 * A `jmp_buf` is not a pointer and not an array parameter: `setjmp(env)` fills in the
 * caller's storage, so `jmp_buf` is an array type and every function taking one takes the
 * address of the caller's array.  That is the standard's arrangement, and getting it wrong
 * is a buffer of the wrong size rather than a compile error.
 */
#ifndef MINX_SETJMP_H
#define MINX_SETJMP_H

/* rbx rbp r12 r13 r14 r15 rsp rip -- the callee-saved registers, then the stack pointer,
 * then where `setjmp()` will return to.  Nine words, in that order, written by
 * libc/src/setjmp/setjmp.S. */
typedef unsigned long jmp_buf[9];

/* `sigjmp_buf` is a `jmp_buf` plus the blocked-signal set, so that `siglongjmp()` can put
 * the mask back the way it was.  Without that a program leaving a handler by `siglongjmp()`
 * would arrive with every signal unblocked that the handler had blocked -- and the signal
 * that interrupted it would be delivered again, for ever. */
typedef unsigned long sigjmp_buf[10];

/* Returns 0 when `setjmp()` is called and the value passed to `longjmp()` when it returns
 * again.  The two cases are not distinguishable by the return value alone: a `longjmp(v)`
 * with v == 0 arrives as 1, because a `longjmp` that arrived as 0 would be
 * indistinguishable from the call that saved the environment.
 *
 * `returns_twice` is not decoration and omitting it is a silent, total failure.  Without it
 * the compiler believes the function returns once, so it keeps a `setjmp()` result in a
 * register across the call -- and `longjmp()` restores the registers the ABI saves, which
 * are not that one.  The program's test
 *
 *     int v = setjmp(env);
 *     if (v == 0) longjmp(env, 42);
 *     use(v);                 <-- v is still 0 here, and nothing says why
 *
 * compiles, links, runs, and is wrong.  With the attribute the compiler keeps `v` in memory
 * and reloads it, which is what the source says. */
#if defined(__GNUC__) || defined(__clang__)
#define MINX_RETURNS_TWICE __attribute__((returns_twice))
#define MINX_NORETURN      __attribute__((noreturn))
#else
#define MINX_RETURNS_TWICE
#define MINX_NORETURN
#endif

MINX_RETURNS_TWICE int  setjmp(jmp_buf env);
MINX_NORETURN      void longjmp(jmp_buf env, int value);

MINX_RETURNS_TWICE int  sigsetjmp(sigjmp_buf env, int save_mask);
MINX_NORETURN      void siglongjmp(sigjmp_buf env, int value);

#endif /* MINX_SETJMP_H */
