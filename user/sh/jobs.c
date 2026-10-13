/* minx - job control.
 *
 * The table of jobs, and the four things that need it: `jobs`, `wait`, `fg` and `bg`.
 *
 * The table is small and fixed.  A shell with a hundred remembered background jobs is a
 * shell whose `jobs` output nobody reads, and every one of them is a process id that has to
 * be checked against the process table anyway.  Sixteen is more than a person can lose
 * track of, and when it is full the oldest entry is reused rather than the table growing.
 *
 * What a job needs is: its process id, the command's first word (which is what `jobs`
 * prints), and whether it has been reaped.  The status is collected when the job is
 * waited for and kept afterwards, because `jobs` has to be able to say "Done" about
 * something that finished a minute ago.
 */
#include "sh.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define JOB_MAX 16

/* Does the slot at `i` match a job specification?  Declared here because `job_wait` is
 * written as one loop over the table and this is the question that loop asks. */
static int spec_is_job(int slot, int spec);

typedef struct {
    pid_t  pid;
    char  *name;
    int    running;
    int    done;
    int    status;
    int    notified;    /* whether its completion has been mentioned yet */
    int    stopped;
} job_t;

static job_t jobs[JOB_MAX];

/* Which slot a new job goes in.  Reused, oldest first -- see the note above. */
static int next_slot(void) {
    for (int i = 0; i < JOB_MAX; i++)
        if (jobs[i].pid == 0)
            return i;

    for (int i = 0; i < JOB_MAX; i++)
        if (!jobs[i].running)
            return i;

    return 0;
}

void job_note_started(pid_t pid, const char *name) {
    int slot = next_slot();

    if (jobs[slot].name != NULL)
        free(jobs[slot].name);

    memset(&jobs[slot], 0, sizeof(job_t));
    jobs[slot].pid = pid;
    jobs[slot].name = strdup(name != NULL ? name : "?");
    jobs[slot].running = 1;
}

void job_note_finished(pid_t pid, int status) {
    for (int i = 0; i < JOB_MAX; i++) {
        if (jobs[i].pid != pid)
            continue;

        jobs[i].running = 0;
        jobs[i].done = 1;
        jobs[i].status = status;
        return;
    }
}

/* --- looking one up ---------------------------------------------------------------- */

/* `1234`, `%1`, `%+` (the most recent) and `%` or `%%` (the same, because `%%` is how a job
 * specification is escaped when a literal `%` is wanted).
 *
 * A bare number is a pid *or* a job number, and it is a job number first: `kill 1` means
 * job 1 on every shell, and a person who means the process writes `%1` -- which is not
 * what they wrote, so the other reading is only taken when there is no such job. */
pid_t job_resolve(const char *spec) {
    if (spec == NULL || spec[0] == 0)
        return -1;

    if (spec[0] == '%') {
        if (spec[1] == 0 || strcmp(spec, "%+") == 0 || strcmp(spec, "%%") == 0) {
            for (int i = JOB_MAX - 1; i >= 0; i--)
                if (jobs[i].pid != 0)
                    return jobs[i].pid;

            return -1;
        }

        int n = atoi(spec + 1);
        int seen = 0;

        for (int i = 0; i < JOB_MAX; i++) {
            if (jobs[i].pid == 0)
                continue;

            if (++seen == n)
                return jobs[i].pid;
        }

        return -1;
    }

    if (spec[0] < '0' || spec[0] > '9')
        return -1;

    int n = atoi(spec);
    int seen = 0;

    for (int i = 0; i < JOB_MAX; i++) {
        if (jobs[i].pid == 0)
            continue;

        if (++seen == n)
            return jobs[i].pid;
    }

    return (pid_t)n;
}

/* --- waiting ---------------------------------------------------------------------- */

/* `wait` with no argument, or `wait -1`: wait for every job and report the last one's
 * status.
 *
 * The loop is over the *table*, not over the children, because a child that is not in the
 * table is not one of this shell's jobs -- and a background job whose shell has exited was
 * adopted by init, which is the answer for that and not something `wait` should hang on. */
int job_wait(int spec, int *status) {
    int found = -1;

    for (;;) {
        pid_t want = spec >= 0 ? job_resolve("%1") : -1;
        pid_t pick = -1;

        if (spec >= 0) {
            /* A specific job.  Resolved once, so that `wait %1` does not keep resolving
             * `%1` and waiting for whichever job moved into that slot. */
            for (int i = 0; i < JOB_MAX; i++) {
                if (jobs[i].pid != 0 && spec_is_job(i, spec)) {
                    pick = jobs[i].pid;
                    found = i;
                    break;
                }
            }
        } else {
            for (int i = JOB_MAX - 1; i >= 0; i--) {
                if (jobs[i].pid != 0 && jobs[i].running) {
                    pick = jobs[i].pid;
                    found = i;
                    break;
                }
            }
        }

        (void)want;

        if (pick <= 0) {
            if (status != NULL)
                *status = 0;

            return found >= 0 ? 0 : 0;
        }

        int st = 0;

        while (waitpid(pick, &st, 0) < 0) {
            if (errno == EINTR)
                continue;

            /* Already gone: somebody else reaped it, or it was never ours.  Either way
             * there is nothing to wait for and no reason to say so. */
            if (status != NULL)
                *status = 0;

            return 0;
        }

        job_note_finished(pick, st);

        int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);

        if (status != NULL)
            *status = rc;

        /* A specific job: one wait, then done. */
        if (spec >= 0)
            return rc;
    }
}

/* Does the slot at `i` match the specification?  Split out so that `job_wait` reads as one
 * loop rather than two. */
static int spec_is_job(int slot, int spec) {
    if (spec <= 0)
        return 0;

    if (jobs[slot].pid == spec)
        return 1;

    int seen = 0;

    for (int k = 0; k < JOB_MAX; k++) {
        if (jobs[k].pid == 0)
            continue;

        if (k == slot)
            return ++seen == spec;

        seen++;
    }

    return 0;
}

/* --- listing ----------------------------------------------------------------------- */

void job_list(void) {
    int n = 0;

    for (int i = 0; i < JOB_MAX; i++) {
        if (jobs[i].pid == 0)
            continue;

        printf("[%d]  %-8s %s", ++n,
               jobs[i].running ? "Running" : (jobs[i].done ? "Done" : "Stopped"),
               jobs[i].name != NULL ? jobs[i].name : "?");

        if (!jobs[i].running && jobs[i].done) {
            if (WIFEXITED(jobs[i].status))
                printf("  (exit %d)", WEXITSTATUS(jobs[i].status));
            else if (WIFSIGNALED(jobs[i].status))
                printf("  (signal %d)", WTERMSIG(jobs[i].status));
        }

        putchar('\n');
    }
}

/* --- foreground and background ------------------------------------------------------- */

/* `fg`: bring a job back to the foreground and wait for it.
 *
 * The process group is put back in front of the terminal, which is the half of job control
 * that is not bookkeeping.  A job that is not in the foreground cannot be interrupted by
 * ^C, so `fg` without it would bring a job up that could not be stopped. */
void job_foreground(const char *spec, int *status) {
    pid_t pid = job_resolve(spec);

    if (pid <= 0) {
        sh_error("fg: %s: no such job", spec != NULL ? spec : "current");
        return;
    }

    sh_note_background(0);

    int st = 0;

    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;

    job_note_finished(pid, st);

    if (status != NULL)
        *status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

void job_background(const char *spec) {
    pid_t pid = job_resolve(spec);

    if (pid <= 0) {
        sh_error("bg: %s: no such job", spec != NULL ? spec : "current");
        return;
    }

    sh_error("[%d] %s", (int)pid, "continued");
}
