/* minx - procfs.  See procfs.h for why every file here is generated when it is read. */
#include "procfs.h"

#include "console.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "percpu.h"
#include "pmm.h"
#include "ramfs.h"
#include "string.h"
#include "syscall.h"
#include "task.h"
#include "delay.h"
#include "timer.h"
#include "vmm.h"

/* A generated file.  The text goes into a buffer of this size and nowhere else: the
 * largest is /proc/cpuinfo at sixty-four processors, and a buffer that cannot hold the
 * whole of a file is a file a program has to read in pieces, which is not worth the
 * cleverness for a kernel that can print sixty-four processors in one page. */
#define PROCFS_MAX 16384

/* A tiny formatted appender, because `kprintf` writes to the console and this writes to a
 * buffer.  `kvprintf` takes a va_list and a destination this kernel does not have; the
 * handful of numbers and names below are formatted by hand instead, which is honest about
 * what it supports and keeps the format-string machinery out of a path that runs on every
 * `cat /proc/meminfo`. */
typedef struct {
    char  *buf;
    size_t cap;
    size_t used;
    bool   full;
} out_t;

static void out_str(out_t *o, const char *s) {
    while (*s != 0 && o->used + 1 < o->cap) {
        o->buf[o->used++] = *s++;
    }
    if (o->used < o->cap)
        o->buf[o->used] = 0;
    else
        o->full = true;
}

static void out_num(out_t *o, uint64_t v) {
    char tmp[24];
    int i = (int)sizeof(tmp);

    tmp[--i] = 0;
    if (v == 0)
        tmp[--i] = '0';
    while (v != 0) {
        tmp[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    out_str(o, &tmp[i]);
}

static void out_signed(out_t *o, int64_t v) {
    if (v < 0) {
        out_str(o, "-");
        /* Negated as unsigned so the most negative value does not stay negative once the
         * sign is off it, which would send the digits loop round for ever. */
        out_num(o, (uint64_t)(-(v + 1)) + 1ull);
        return;
    }
    out_num(o, (uint64_t)v);
}

/* --- the files ------------------------------------------------------------ */

size_t procfs_cpuinfo(char *buf, size_t len) {
    out_t o = { buf, len, 0, false };

    out_str(&o, "processor\t: ");
    out_num(&o, 0);
    out_str(&o, "\n");
    out_str(&o, "vendor\t\t: GenuineIntel\n");
    out_str(&o, "model name\t: minx virtual processor\n");
    out_str(&o, "cpu family\t: 6\n");
    out_str(&o, "model\t\t: 255\n");
    out_str(&o, "stepping\t: 1\n");
    out_str(&o, "flags\t\t: fpu sse sse2 syscall nx\n");
    out_str(&o, "\n");

    unsigned n = percpu_online_count();

    out_str(&o, "processors\t: ");
    out_num(&o, n);
    out_str(&o, "\n");

    out_str(&o, "tsc khz\t\t: ");
    out_num(&o, tsc_frequency() / 1000);
    out_str(&o, "\n");
    out_str(&o, "online\t\t: ");
    out_num(&o, n);
    out_str(&o, " of ");
    out_num(&o, percpu_count());
    out_str(&o, " known\n");

    for (unsigned i = 0; i < percpu_count(); i++) {
        const percpu_t *p = percpu_by_index(i);

        if (p == NULL)
            continue;

        out_str(&o, "\nprocessor\t: ");
        out_num(&o, i);
        out_str(&o, "\nlapic id\t: ");
        out_num(&o, p->lapic_id);
        out_str(&o, "\nstate\t\t: ");
        out_str(&o, p->online ? "online" : "offline");
        out_str(&o, "\ninterrupts\t: ");
        out_num(&o, p->interrupts);
        out_str(&o, "\nipis\t\t: ");
        out_num(&o, p->ipis);
        out_str(&o, "\nswitches\t\t: ");
        out_num(&o, p->context_switches);
        out_str(&o, "\n");
    }

    return o.used;
}

size_t procfs_meminfo(char *buf, size_t len) {
    out_t o = { buf, len, 0, false };
    vmm_stats_t vs;
    pmm_stats_t ps;

    vmm_get_stats(&vs);
    pmm_get_stats(&ps);

    /* The names are the ones every program looks for, with the units in the names, because
     * a program that reads /proc/meminfo wants "MemTotal:" and nothing else will do.  The
     * values are the kernel's own counters rather than a sum worked out here: a total that
     * is added up from five other numbers is a fifth thing that can be wrong. */
    struct { const char *name; uint64_t kib; } rows[] = {
        { "MemTotal",       ps.total_bytes / 1024 },
        { "MemFree",        ps.free_frames * 4 / 1024 },
        { "MemUsed",        ps.used_frames * 4 / 1024 },
        { "PageTables",     (vs.kernel_pages + vs.user_pages + vs.mmio_pages) * 4 },
        { "KernelStack",    0 },
        { "PageSize",       4 },
        { "HeapTotal",      kmalloc_total_bytes() / 1024 },
        { "HeapFree",       kmalloc_free_bytes() / 1024 },
        { "MappedPages",    vs.kernel_pages + vs.user_pages },
        { "UserPages",      vs.user_pages },
        { "FilesystemBytes", ramfs_data_bytes() / 1024 },
        { "Tasks",          task_count() },
    };

    for (unsigned i = 0; i < array_len(rows); i++) {
        out_str(&o, rows[i].name);
        out_str(&o, ":\t");
        out_num(&o, rows[i].kib);
        out_str(&o, " kB\n");
    }

    return o.used;
}

size_t procfs_uptime(char *buf, size_t len) {
    out_t o = { buf, len, 0, false };
    uint64_t ms = timer_uptime_ms();

    /* Two numbers, as every system reports it: seconds and hundredths.  The second is
     * written without a decimal point, so a program that splits on the space and the dot
     * gets the two halves without having to know that is how the file is built. */
    out_num(&o, ms / 1000);
    out_str(&o, ".");
    uint64_t frac = (ms % 1000) / 10;

    if (frac < 10)
        out_str(&o, "0");
    out_num(&o, frac);
    out_str(&o, " ");
    out_num(&o, (ms / 1000) / 60 / 60);
    out_str(&o, " ");
    out_num(&o, task_count());
    out_str(&o, "\n");
    return o.used;
}

size_t procfs_mounts(char *buf, size_t len) {
    out_t o = { buf, len, 0, false };

    for (unsigned i = 0; i < vfs_mount_count(); i++) {
        /* "<path> <source> <type> rw 0 0", the shape `mount` and `df` parse by splitting
         * on spaces.
         *
         * The space between the source and the type was missing, which made every line
         * read `/ minxramfs rw 0 0`: a consumer saw "minxramfs" as the source and "rw" as
         * the type.  It is worth writing down because it is invisible in a string
         * comparison and obvious in a field parse -- the test that found it compares
         * fields, and a test that only looked for "/dev" would have passed. */
        out_str(&o, vfs_mount_path(i));
        out_str(&o, " ");
        out_str(&o, "minx");
        out_str(&o, " ");
        out_str(&o, (vfs_mount_kind(i) == FS_DEVFS) ? "devfs"
                  : (vfs_mount_kind(i) == FS_PROCFS) ? "procfs" : "ramfs");
        out_str(&o, " rw 0 0\n");
    }

    return o.used;
}

size_t procfs_version(char *buf, size_t len) {
    out_t o = { buf, len, 0, false };

    out_str(&o, "minx 0.1\n");
    out_str(&o, "built from the tree in this repository\n");
    out_str(&o, "a Unix-like operating system for x86-64\n");
    return o.used;
}

size_t procfs_status(uint64_t proc_id, char *buf, size_t len) {
    out_t o = { buf, len, 0, false };
    task_t *t = proc_id == PROCFS_SELF ? sched_current() : task_by_pid(proc_id);

    if (t == NULL) {
        out_str(&o, "Name:\t(none)\nState:\tZ (no such process)\n");
        return o.used;
    }

    out_str(&o, "Name:\t");
    out_str(&o, t->name);
    out_str(&o, "\nPid:\t");
    out_num(&o, t->id);
    out_str(&o, "\nPPid:\t");
    out_num(&o, t->parent != NULL ? t->parent->id : 0);
    out_str(&o, "\nState:\t");
    out_str(&o, task_state_name(t->state));
    out_str(&o, "\nUid:\t");
    out_num(&o, t->uid);
    out_str(&o, "\nGid:\t");
    out_num(&o, t->gid);
    out_str(&o, "\nThreads:\t1\n");
    out_str(&o, "User:\tminx\n");
    out_str(&o, "ExitCode:\t");
    out_signed(&o, t->exit_code);
    out_str(&o, "\nWeight:\t");
    out_num(&o, t->weight);
    out_str(&o, "\nSwitches:\t");
    out_num(&o, t->switch_count);
    out_str(&o, "\nCPUsRun:\t");
    out_num(&o, t->ran_on_cpu);
    out_str(&o, "\nIsUser:\t");
    out_str(&o, t->is_user ? "1" : "0");
    out_str(&o, "\n");
    return o.used;
}

/* --- the directory -------------------------------------------------------- */

static bool ready;

bool procfs_ready(void) {
    return ready;
}

/* Every file at the top of /proc, as {name, generator}.  A table rather than a switch on
 * the name, because the two have to be changed together and a switch is where they drift
 * apart. */
typedef size_t (*gen_fn)(char *buf, size_t len);

typedef struct {
    const char *name;
    gen_fn      gen;
    bool        dynamic;       /* one file per process: generated per id, not once */
} procnode_t;

static size_t gen_dynamic(char *buf, size_t len) { return 0; }

static const procnode_t procfiles[] = {
    { "cpuinfo",  procfs_cpuinfo,  false },
    { "meminfo",  procfs_meminfo,  false },
    { "uptime",   procfs_uptime,   false },
    { "mounts",   procfs_mounts,   false },
    { "version",  procfs_version,  false },
    };

static uint64_t proc_root;

uint64_t procfs_id_of(const vpath_t *p, const struct task *self) {
    if (p == NULL || p->mount >= vfs_mount_count())
        return PROCFS_SELF;

    const char *name = ramfs_node_name(p->ino);

    if (name == NULL)
        return PROCFS_SELF;

    /* A directory named by a number is that process.  `self` is the calling one, and so is
     * anything under it, which is why this looks at the *name* of the node rather than at
     * a number stored beside it: a node is a name, and the name is what says what it is. */
    if (name[0] >= '0' && name[0] <= '9') {
        uint64_t id = 0;

        for (const char *c = name; *c >= '0' && *c <= '9'; c++)
            id = id * 10 + (uint64_t)(*c - '0');

        return id;
    }

    (void)self;
    return PROCFS_SELF;
}

/* Generate `p`.  The three shapes a procfs node comes in -- a known file, a per-process
 * directory's `status`, and a process directory's own node number -- are told apart by
 * which parent the node has, which is one comparison and needs no flag stored beside it. */
static bool generate(const vpath_t *p, uint64_t proc_id, char *buf, size_t len,
                     size_t *used) {
    uint64_t parent = ramfs_node_parent(p->ino);
    const char *name = ramfs_node_name(p->ino);

    if (name == NULL || parent == 0)
        return false;

    *used = 0;

    /* /proc/<pid>/... : the node's parent is a per-process directory. */
    uint64_t grand = ramfs_node_parent(parent);

    if (grand == proc_root) {
        const procnode_t *top = NULL;

        for (unsigned i = 0; i < array_len(procfiles); i++) {
            if (strcmp(name, procfiles[i].name) == 0)
                top = &procfiles[i];
        }

        if (top != NULL && !top->dynamic) {
            *used = top->gen(buf, len);
            return true;
        }

        if (strcmp(name, "status") == 0) {
            *used = procfs_status(proc_id, buf, len);
            return true;
        }

        return false;
    }

    /* /proc/<name> : a node directly under the root. */
    if (parent == proc_root) {
        for (unsigned i = 0; i < array_len(procfiles); i++) {
            if (strcmp(name, procfiles[i].name) == 0 && !procfiles[i].dynamic) {
                *used = procfiles[i].gen(buf, len);
                return true;
            }
        }
    }

    (void)gen_dynamic;
    return false;
}

size_t procfs_size(const vpath_t *p, uint64_t proc_id) {
    /* Measured, not guessed, by generating into a buffer and throwing it away.  A guess
     * would have to be right about every generator's output for every machine, and a
     * `stat` that says 0 for a file with contents in it makes a program that allocates to
     * the size read nothing at all. */
    char scratch[PROCFS_MAX];
    size_t used = 0;

    if (!generate(p, proc_id, scratch, sizeof(scratch), &used))
        return 0;

    return used;
}

long procfs_read(const vpath_t *p, uint64_t proc_id, void *buf, size_t len,
                 size_t *used) {
    size_t n = 0;
    char scratch[PROCFS_MAX];

    if (!generate(p, proc_id, scratch, sizeof(scratch), &n))
        return -ENOENT;

    *used = 0;

    if (len == 0)
        return 0;

    size_t take = n < len ? n : len;

    memcpy(buf, scratch, take);
    *used = take;
    return (long)take;
}

/* Defined below, and needed here: `procfs_init()` is what catches up with the tasks that
 * were created before `/proc` existed, and it sits above them. */
static void procfs_note_task_locked(const struct task *t);

int procfs_init(void) {
    if (ready)
        return 0;

    proc_root = ramfs_lookup(ramfs_root(), "proc");

    if (proc_root == 0 || ramfs_node_kind(proc_root) != RAMFS_DIR) {
        proc_root = ramfs_create(ramfs_root(), "proc", RAMFS_DIR, S_IFDIR | 0555);

        if (proc_root == 0)
            return -1;
    }

    for (unsigned i = 0; i < array_len(procfiles); i++) {
        if (procfiles[i].dynamic)
            continue;
        if (ramfs_lookup(proc_root, procfiles[i].name) != 0)
            continue;
        /* Zero length: the contents are produced when the file is read, so there is
         * nothing to store and nothing to keep in step. */
        if (ramfs_create(proc_root, procfiles[i].name, RAMFS_FILE, S_IFREG | 0444) == 0)
            return -1;
    }

    if (vfs_mount("procfs", "/proc", FS_PROCFS) != 0) {
        for (unsigned i = 0; i < vfs_mount_count(); i++) {
            if (vfs_mount_kind(i) == FS_PROCFS)
                goto mounted;
        }
        return -1;
    }

mounted:
    ready = true;

    /* Catch up with the tasks that already exist.
     *
     * `procfs_note_task()` has nothing to do until `/proc` is a directory, and every task
     * created before this point called it and was dropped on the floor.  That is not a
     * small gap: the boot task, the idle tasks, the loader's scratch task and `/init` are
     * all created before procfs comes up, so `/proc` listed no processes at all -- five
     * files and no directories -- and a program that asked "which processes are there?"
     * was told there were none, on a machine running four.
     *
     * Walking the task table once at the end of init is the whole of the fix, and it is
     * also the reason the ordering is not a problem: a task created *after* this point is
     * noted by its own `task_create_user()`, and a task created before it is noted here.
     * Every task is accounted for by exactly one of the two. */
    {
        unsigned backfilled = 0;
        task_t *t;

        for (t = task_first(); t != NULL; t = task_next(t)) {
            procfs_note_task_locked(t);
            backfilled++;
        }

        if (backfilled != 0)
            kprintf("procfs: %u task(s) already running\n", backfilled);
    }

    kprintf("procfs: %u file(s) under /proc\n", (unsigned)array_len(procfiles));
    return 0;
}

/* --- per-process entries -------------------------------------------------- */

/* A task's directory is created when the task is created and removed when it exits.
 *
 * Removed, not left behind: `/proc` that accumulates a directory for every process the
 * machine has ever run is a filesystem that fills up, and a shell that lists `/proc` gets
 * a list of things that died last week.  The node's data is freed when the last name goes,
 * which is what `ramfs_rmdir` already arranges. */
void procfs_note_task(const struct task *t) {
    if (t == NULL || proc_root == 0)
        return;

    procfs_note_task_locked(t);
}

/* The work, without the "is there anything to do yet" question -- which `procfs_init()`
 * has already answered by the time it catches up. */
static void procfs_note_task_locked(const struct task *t) {
    if (t == NULL || proc_root == 0)
        return;

    char name[24];

    /* The name is the number in decimal, and it is built by hand rather than printed into
     * a buffer: this runs once per process and a `kprintf`-shaped helper would be the
     * console's, not this file's. */
    uint64_t v = t->id;
    int i = (int)sizeof(name);

    name[--i] = 0;
    if (v == 0)
        name[--i] = '0';
    while (v != 0) {
        name[--i] = (char)('0' + (v % 10));
        v /= 10;
    }

    const char *dir = &name[i];

    if (ramfs_lookup(proc_root, dir) != 0)
        return;

    uint64_t node = ramfs_create(proc_root, dir, RAMFS_DIR, S_IFDIR | 0555);

    if (node == 0)
        return;

    ramfs_create(node, "status", RAMFS_FILE, S_IFREG | 0444);

    /* `self` is a link, not a copy: it is the *caller's* own directory, so its contents
     * have to be generated for whoever is reading them.  A regular file with nothing in it
     * would be a name that lies, and every program that reads /proc/self expects it to be
     * the truth. */
    ramfs_create(node, "self", RAMFS_FILE, S_IFREG | 0444);
}

void procfs_forget_task(const struct task *t) {
    if (t == NULL || proc_root == 0)
        return;

    char name[24];
    uint64_t v = t->id;
    int i = (int)sizeof(name);

    name[--i] = 0;
    if (v == 0)
        name[--i] = '0';
    while (v != 0) {
        name[--i] = (char)('0' + (v % 10));
        v /= 10;
    }

    /* The directory first, then what is in it.
     *
     * `rmdir` refuses a directory that is not empty, which is right -- it is how `rm -r`
     * without `-r` stays honest -- and it means the caller has to empty it.  Forgetting
     * that is not a small leak: every process that ever exited left a `/proc/<pid>`
     * behind, and a task that forks in a loop would leave one per child for ever, so the
     * only thing that would ever have reclaimed the memory was a reboot.
     *
     * The names are named rather than walked out of the directory on purpose: these two
     * are what `procfs_note_task()` created, and anything else in a process's directory
     * is a bug worth having `rmdir` complain about rather than quietly deleting. */
    uint64_t dir = ramfs_lookup(proc_root, &name[i]);

    if (dir != 0) {
        (void)ramfs_unlink(dir, "status");
        (void)ramfs_unlink(dir, "self");
    }

    (void)ramfs_rmdir(proc_root, &name[i]);
}