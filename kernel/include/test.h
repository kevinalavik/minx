/* minx - in-kernel self tests, run when booted with minx.test=1. */
#ifndef MINX_TEST_H
#define MINX_TEST_H

#include <stdarg.h>

void test_pass(const char *name);
void test_fail(const char *name, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Run every check, print the PASS/FAIL lines, then announce TEST-READY. */
void test_run_all(void);

/* Linger on screen so the harness can dump the framebuffer, then power off
 * with an exit status derived from the results. */
void test_linger_and_exit(void) __attribute__((noreturn));

#endif /* MINX_TEST_H */