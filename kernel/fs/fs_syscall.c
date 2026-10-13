/* minx - the file-related system calls.  See fscalls.h. */
#include "fscalls.h"

#include "console.h"
#include "devfs.h"
#include "fd.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "procfs.h"
#include "ramfs.h"
#include "string.h"
#include "syscall.h"
#include "task.h"
#include "timer.h"
#include "tty.h"
#include "vfs.h"

/* --- reading names out of the boundary ----------------------------------- */

long syscall_copy_string(uint64_t src, char *dst, size_t max) {
    if (max == 0)
        return -EFAULT;

    /* Page by page, and the length of a page is whatever is left of the buffer, so a path
     * that is longer than the buffer is refused rather than truncated: a name cut short is
     * a different name, and opening one of those is how a program ends up writing to a file
     * it never mentioned. */
    size_t got = 0;

    while (got + 1 < max) {
        size_t chunk = max - 1 - got;

        if (chunk > 256)
            chunk = 256;

        if (!syscall_copy_from_user(dst + got, src + got, chunk))
            return -EFAULT;

        for (size_t i = 0; i < chunk; i++) {
            if (dst[got + i] == 0)
                return (long)(got + i);
        }

        got += chunk;
    }

    return -ENAMETOOLONG;
}

/* The path a call names, into `dst`.  0, or -errno. */
static long path_arg(regs_t *regs, unsigned n, char *dst, size_t max) {
    return syscall_copy_string(syscall_arg(regs, n), dst, max);
}

/* --- open and close -------------------------------------------------------- */

long fs_sys_open(regs_t *regs) {
    char path[VFS_NAME_MAX * 2];
    long r = path_arg(regs, 0, path, sizeof(path));

    if (r < 0)
        return r;

    uint32_t flags = (uint32_t)syscall_arg(regs, 1);
    mode_t mode = (mode_t)(syscall_arg(regs, 2) & 07777);
    task_t *t = sched_current();

    vpath_t p;
    r = vfs_resolve(t, path, true, &p);

    if (r == 0) {
        if (flags & O_CREAT && (flags & O_EXCL))
            return -EEXIST;
    } else if (r == -ENOENT && (flags & O_CREAT)) {
        char name[VFS_NAME_MAX];

        /* Split the path by hand rather than through vfs_mkdir(), because that one makes a
         * *directory* and a file is not one.  The parent's resolution is the part that has
         * to be right, and it is the same call `mkdir` makes. */
        const char *slash = strrchr(path, '/');
        const char *base = slash != NULL ? slash + 1 : path;
        char dir[VFS_NAME_MAX * 2];
        size_t dlen = (size_t)(base - path);

        if (base[0] == 0)
            return -EISDIR;

        strlcpy(name, base, sizeof(name));

        if (dlen == 0) {
            strlcpy(dir, "/", sizeof(dir));
        } else {
            if (dlen >= sizeof(dir))
                return -ENAMETOOLONG;
            memcpy(dir, path, dlen - 1);
            dir[dlen - 1] = 0;
        }

        vpath_t dp;

        if (vfs_resolve(t, dir[0] == 0 ? "/" : dir, true, &dp) != 0)
            return -ENOENT;
        if (ramfs_node_kind(dp.ino) != RAMFS_DIR)
            return -ENOTDIR;
        if (vfs_mount_kind(dp.mount) != FS_RAMFS)
            return -EROFS;

        if (ramfs_create(dp.ino, name, RAMFS_FILE, mode | S_IFREG) == 0)
            return -EIO;

        r = vfs_resolve(t, path, true, &p);

        if (r != 0)
            return r;
    } else {
        return r;
    }

    file_t *f = NULL;
    r = vfs_open(&p, flags, &f);

    if (r != 0)
        return r;

    int fd = fd_alloc(FD_FILE, f, 0);

    if (fd < 0) {
        vfs_close(f);
        return -EMFILE;
    }

    /* Opening the terminal gives it to the program.
     *
     * Not because the terminal belongs to whoever opened it first -- two programs can have
     * it open at once -- but because the *keystrokes* have to go to exactly one place, and
     * the program that most recently opened the terminal is the one whose prompt is on
     * screen.  A `sh` that starts a foreground program must therefore be able to take the
     * terminal back when it stops, which is what closing does. */
    if (ramfs_node_kind(p.ino) == RAMFS_DEVICE &&
        ramfs_node_device(p.ino) == RAMFS_DEV_CONSOLE && (flags & O_RDWR) == O_RDWR)
        tty_attach(true);

    return fd;
}

long fs_sys_close(regs_t *regs) {
    int fd = (int)(int64_t)syscall_arg(regs, 0);
    file_t *f = fd_file(fd);

    /* Was this the terminal?  Asked before the close, because after it the file is gone and
     * the question has no answer. */
    bool was_terminal = f != NULL &&
                        ramfs_node_kind(f->ino) == RAMFS_DEVICE &&
                        ramfs_node_device(f->ino) == RAMFS_DEV_CONSOLE;

    /* EBADF for a number that is not open, and for one of 0, 1 and 2: the program did
     * not have it to begin with, and the console belongs to the process table.  Linux
     * returns success for the second case and that is a question of taste, not one this
     * kernel gets to be quietly different about on the way to a shell that will use it. */
    if (!fd_close(fd))
        return -EBADF;

    /* Closing the terminal gives the keystrokes back, so the kernel's own console commands
     * work again.  A `sh` that has just run a foreground program relies on this to get its
     * prompt's keystrokes back without a timeout. */
    if (was_terminal)
        tty_attach(false);

    return 0;
}

long fs_sys_lseek(regs_t *regs) {
    int fd = (int)(int64_t)syscall_arg(regs, 0);
    file_t *f = fd_file(fd);

    if (f == NULL)
        return -EBADF;

    return vfs_seek(f, (int64_t)syscall_arg(regs, 1), (int)(int64_t)syscall_arg(regs, 2));
}

long fs_sys_fsync(regs_t *regs) {
    int fd = (int)(int64_t)syscall_arg(regs, 0);

    if (fd_get(fd) == NULL)
        return -EBADF;

    /* Everything this kernel writes is already in memory -- there is no page cache behind
     * a file and no disk under it -- so a sync has nothing to do and succeeds.  Saying
     * ENOSYS would make every editor's save path report a failure that is not one. */
    return 0;
}

/* `sync()`, with no descriptor: flush everything.
 *
 * Its own syscall rather than `fsync()` of some magic descriptor, for one reason that is
 * not tidiness.  The obvious spelling is `fsync(-1)`, and that makes "flush everything"
 * indistinguishable from "flush the descriptor -1", which is not a descriptor -- so a
 * program that meant to sync one file and passed an `fd` it never set gets a flush of
 * everything, which is a success reported for a mistake.
 *
 * `init` calls this immediately before stopping the machine, and the answer is zero for the
 * same reason `fsync`'s is: there is nothing to flush. */
long fs_sys_sync(regs_t *regs) {
    (void)regs;
    return 0;
}

long fs_sys_fstat(regs_t *regs) {
    int fd = (int)(int64_t)syscall_arg(regs, 0);
    uint64_t out = syscall_arg(regs, 1);
    file_t *f = fd_file(fd);

    if (f == NULL) {
        /* 1 and 2 are descriptors and have attributes too: `ls -l /proc/self/fd` and every
         * program that stats its own output depend on that, and EBADF for the console
         * would be wrong for a descriptor the program certainly has. */
        if (fd < 0 || fd > 2 || fd_get(fd) == NULL)
            return -EBADF;

        stat_t st;

        memset(&st, 0, sizeof(st));
        st.st_mode = S_IFCHR | 0620;
        st.st_nlink = 1;
        st.st_blksize = 4096;
        st.st_ino = 1000 + (uint64_t)fd;

        if (!syscall_copy_to_user(out, &st, sizeof(st)))
            return -EFAULT;

        return 0;
    }

    stat_t st;
    long r = vfs_fstat(f, &st);

    if (r != 0)
        return r;

    if (!syscall_copy_to_user(out, &st, sizeof(st)))
        return -EFAULT;

    return 0;
}

long fs_sys_stat(regs_t *regs) {
    char path[VFS_NAME_MAX * 2];
    long r = path_arg(regs, 0, path, sizeof(path));

    if (r < 0)
        return r;

    stat_t st;
    r = vfs_stat_path(sched_current(), path, true, &st);

    if (r != 0)
        return r;

    if (!syscall_copy_to_user(syscall_arg(regs, 1), &st, sizeof(st)))
        return -EFAULT;

    return 0;
}

long fs_sys_lstat(regs_t *regs) {
    char path[VFS_NAME_MAX * 2];
    long r = path_arg(regs, 0, path, sizeof(path));

    if (r < 0)
        return r;

    stat_t st;
    r = vfs_stat_path(sched_current(), path, false, &st);

    if (r != 0)
        return r;

    if (!syscall_copy_to_user(syscall_arg(regs, 1), &st, sizeof(st)))
        return -EFAULT;

    return 0;
}

long fs_sys_readlink(regs_t *regs) {
    char path[VFS_NAME_MAX * 2];
    long r = path_arg(regs, 0, path, sizeof(path));

    if (r < 0)
        return r;

    /* Read straight into the program's own buffer: a kernel bounce buffer here would have
     * to be sized against a length the kernel does not know, and the link's target length
     * is in the filesystem, not in the call. */
    uint64_t buf = syscall_arg(regs, 1);
    size_t len = (size_t)syscall_arg(regs, 2);

    if (len == 0)
        return -EINVAL;

    if (!syscall_user_range_ok(buf, 1, true))
        return -EFAULT;

    return vfs_readlink(sched_current(), path, (char *)(uintptr_t)buf, len);
}

long fs_sys_getdents(regs_t *regs) {
    int fd = (int)(int64_t)syscall_arg(regs, 0);
    uint64_t buf = syscall_arg(regs, 1);
    size_t len = (size_t)syscall_arg(regs, 2);
    file_t *f = fd_file(fd);

    if (f == NULL)
        return -EBADF;

    if (!f->is_dir)
        return -ENOTDIR;

    if (len == 0)
        return -EINVAL;

    if (!syscall_user_range_ok(buf, len, true))
        return -EFAULT;

    vpath_t p = { f->mount, f->ino };
    long r = vfs_readdir(&p, &f->offset, (void *)(uintptr_t)buf, len);

    return r;
}

long fs_sys_dup(regs_t *regs) {
    int fd = (int)(int64_t)syscall_arg(regs, 0);
    fd_entry_t *e = fd_get(fd);

    if (e == NULL)
        return -EBADF;

    /* A duplicate names the same *object*, and says so for the console: 0, 1 and 2 belong
     * to the process table, so duplicating one gives a number the program owns.  A pipe
     * end duplicated is one more open end of that pipe, which is what `fork` does. */
    int copy = fd_alloc(e->kind, e->obj, e->pipe_end);

    if (copy < 0)
        return -EMFILE;

    if (e->kind == FD_PIPE)
        pipe_retain((pipe_t *)e->obj, e->pipe_end);

    return copy;
}

long fs_sys_dup2(regs_t *regs) {
    int old = (int)(int64_t)syscall_arg(regs, 0);
    int neu = (int)(int64_t)syscall_arg(regs, 1);
    fd_entry_t *e = fd_get(old);

    if (e == NULL || neu < 0 || neu >= FD_TABLE_ENTRIES)
        return -EBADF;

    /* dup2 onto a number that is already open closes it first.  Not closing it is the bug
     * that leaves a pipe with an end nothing can account for, and the symptom is a reader
     * that never sees end of file. */
    if (neu != old) {
        fd_entry_t *victim = fd_get(neu);

        if (victim != NULL && victim->closable) {
            if (victim->kind == FD_PIPE)
                pipe_close_end((pipe_t *)victim->obj, victim->pipe_end);
            memset(victim, 0, sizeof(*victim));
        }
    }

    fd_entry_t *slot = fd_get(neu);

    if (slot == NULL) {
        int fresh = fd_alloc(e->kind, e->obj, e->pipe_end);

        if (fresh != neu)
            return -EMFILE;
    } else {
        slot->kind = e->kind;
        slot->obj = e->obj;
        slot->pipe_end = e->pipe_end;
        slot->closable = true;
    }

    if (e->kind == FD_PIPE)
        pipe_retain((pipe_t *)e->obj, e->pipe_end);

    return neu;
}

/* --- read and write, for a descriptor that names a file --------------------- */

long fs_sys_read_fd(int fd, uint64_t buf, size_t bytes) {
    file_t *f = fd_file(fd);

    if (f == NULL)
        return -EBADF;

    /* A procfs file is generated a whole buffer at a time and copied out of a kernel
     * buffer, because it does not exist anywhere else: there is no page behind it to point
     * a read at.  Everything else is copied straight into the program's memory. */
    if (vfs_mount_kind(f->mount) == FS_PROCFS) {
        uint64_t id = procfs_id_of(&(vpath_t){ f->mount, f->ino }, sched_current());

        /* Two calls: one to learn the length, one to take it.  The file is generated twice,
         * which is the price of "there is one copy of these numbers in the machine and it
         * is generated when it is read", and it is a page of text either way. */
        size_t total = procfs_size(&(vpath_t){ f->mount, f->ino }, id);
        size_t left = f->offset < total ? total - (size_t)f->offset : 0;
        size_t take = left < bytes ? left : bytes;

        if (take == 0)
            return 0;

        uint8_t *tmp = (uint8_t *)kmalloc(take);

        if (tmp == NULL)
            return -ENOMEM;

        size_t used = 0;

        if (procfs_read(&(vpath_t){ f->mount, f->ino }, id, tmp, take, &used) < 0) {
            kfree(tmp);
            return -EIO;
        }

        if (used > 0 && !syscall_copy_to_user(buf, tmp, used)) {
            kfree(tmp);
            return -EFAULT;
        }

        kfree(tmp);
        f->offset += used;
        return (long)used;
    }

    /* A regular file: the kernel reads into the program's own buffer.  The range was
     * checked by the caller before it got here, and a kernel bounce buffer for a file read
     * would be a second copy of every byte the program is about to own. */
    if (bytes == 0)
        return 0;

    return vfs_read(f, (void *)(uintptr_t)buf, bytes);
}

long fs_sys_write_fd(int fd, const void *buf, size_t bytes) {
    file_t *f = fd_file(fd);

    if (f == NULL)
        return -EBADF;

    return vfs_write(f, buf, bytes);
}

/* --- the tree --------------------------------------------------------------- */

long fs_sys_mkdir(regs_t *regs) {
    char path[VFS_NAME_MAX * 2];
    long r = path_arg(regs, 0, path, sizeof(path));

    if (r < 0)
        return r;

    mode_t mode = (mode_t)(syscall_arg(regs, 1) & 07777);

    return vfs_mkdir(sched_current(), path, mode);
}

long fs_sys_rmdir(regs_t *regs) {
    char path[VFS_NAME_MAX * 2];
    long r = path_arg(regs, 0, path, sizeof(path));

    if (r < 0)
        return r;

    return vfs_rmdir(sched_current(), path);
}

long fs_sys_unlink(regs_t *regs) {
    char path[VFS_NAME_MAX * 2];
    long r = path_arg(regs, 0, path, sizeof(path));

    if (r < 0)
        return r;

    return vfs_unlink(sched_current(), path);
}

long fs_sys_rename(regs_t *regs) {
    char from[VFS_NAME_MAX * 2];
    char to[VFS_NAME_MAX * 2];
    long r = path_arg(regs, 0, from, sizeof(from));

    if (r < 0)
        return r;

    r = path_arg(regs, 1, to, sizeof(to));

    if (r < 0)
        return r;

    return vfs_rename(sched_current(), from, to);
}

long fs_sys_link(regs_t *regs) {
    char from[VFS_NAME_MAX * 2];
    char to[VFS_NAME_MAX * 2];
    long r = path_arg(regs, 0, from, sizeof(from));

    if (r < 0)
        return r;

    r = path_arg(regs, 1, to, sizeof(to));

    if (r < 0)
        return r;

    return vfs_link(sched_current(), from, to);
}

long fs_sys_symlink(regs_t *regs) {
    char target[VFS_NAME_MAX * 2];
    char path[VFS_NAME_MAX * 2];
    long r = path_arg(regs, 0, target, sizeof(target));

    if (r < 0)
        return r;

    r = path_arg(regs, 1, path, sizeof(path));

    if (r < 0)
        return r;

    return vfs_symlink(sched_current(), target, path);
}

long fs_sys_chmod(regs_t *regs) {
    char path[VFS_NAME_MAX * 2];
    long r = path_arg(regs, 0, path, sizeof(path));

    if (r < 0)
        return r;

    return vfs_chmod(sched_current(), path, (mode_t)(syscall_arg(regs, 1) & 07777));
}

long fs_sys_chown(regs_t *regs) {
    char path[VFS_NAME_MAX * 2];
    long r = path_arg(regs, 0, path, sizeof(path));

    if (r < 0)
        return r;

    return vfs_chown(sched_current(), path, syscall_arg(regs, 1), syscall_arg(regs, 2));
}

long fs_sys_truncate(regs_t *regs) {
    char path[VFS_NAME_MAX * 2];
    long r = path_arg(regs, 0, path, sizeof(path));

    if (r < 0)
        return r;

    return vfs_truncate_path(sched_current(), path, syscall_arg(regs, 1));
}

long fs_sys_ftruncate(regs_t *regs) {
    int fd = (int)(int64_t)syscall_arg(regs, 0);
    file_t *f = fd_file(fd);

    if (f == NULL)
        return -EBADF;

    return vfs_ftruncate(f, syscall_arg(regs, 1));
}

long fs_sys_utimes(regs_t *regs) {
    char path[VFS_NAME_MAX * 2];
    long r = path_arg(regs, 0, path, sizeof(path));

    if (r < 0)
        return r;

    uint64_t times[2] = { 0, 0 };
    uint64_t ptr = syscall_arg(regs, 1);

    /* A NULL times pointer means "now", which is what touch does and what every other
     * system means by it; the two zeroes are therefore the current time rather than the
     * epoch, and the difference is a file that looks 55 years old. */
    if (ptr != 0) {
        if (!syscall_copy_from_user(times, ptr, sizeof(times)))
            return -EFAULT;
    } else {
        times[0] = wallclock_seconds();
        times[1] = times[0];
    }

    return vfs_utimes(sched_current(), path, times);
}

long fs_sys_chdir(regs_t *regs) {
    char path[VFS_NAME_MAX * 2];
    long r = path_arg(regs, 0, path, sizeof(path));

    if (r < 0)
        return r;

    return vfs_chdir(sched_current(), path);
}

long fs_sys_getcwd(regs_t *regs) {
    uint64_t buf = syscall_arg(regs, 0);
    size_t len = (size_t)syscall_arg(regs, 1);

    if (len == 0)
        return -EINVAL;

    if (!syscall_user_range_ok(buf, 1, true))
        return -EFAULT;

    /* Reported straight into the program's buffer: the path is the task's own, and copying
     * it into a kernel bounce buffer first would be a copy whose length the kernel has to
     * guess. */
    return vfs_getcwd(sched_current(), (char *)(uintptr_t)buf, len);
}

long fs_sys_umask(regs_t *regs) {
    task_t *t = sched_current();

    if (t == NULL)
        return -ESRCH;

    mode_t old = t->umask;

    /* -1 is "just tell me"; anything else replaces it.  `open` applies the mask to a file
     * it creates, which is the whole of what a umask is for. */
    if ((int64_t)syscall_arg(regs, 0) != -1)
        t->umask = (mode_t)(syscall_arg(regs, 0) & 0777);

    return old;
}

long fs_sys_mount(regs_t *regs) {
    char source[64];
    char target[VFS_NAME_MAX * 2];
    char fstype[32];

    long r = path_arg(regs, 0, source, sizeof(source));

    if (r < 0)
        return r;

    r = path_arg(regs, 1, target, sizeof(target));

    if (r < 0)
        return r;

    r = path_arg(regs, 2, fstype, sizeof(fstype));

    if (r < 0)
        return r;

    fs_kind_t kind;

    if (strcmp(fstype, "devfs") == 0)
        kind = FS_DEVFS;
    else if (strcmp(fstype, "procfs") == 0)
        kind = FS_PROCFS;
    else if (strcmp(fstype, "ramfs") == 0)
        kind = FS_RAMFS;
    else
        return -ENODEV;

    /* The filesystems that have a builder build themselves: a `mount` of devfs on a system
     * that already has one should be a no-op that reports what is there, not a second copy.
     * The builders are idempotent, so calling them is that no-op. */
    if (kind == FS_DEVFS && devfs_init() != 0)
        return -EIO;
    if (kind == FS_PROCFS && procfs_init() != 0)
        return -EIO;

    r = vfs_mount(source, target, kind);

    /* Already mounted there, and of the same kind, is success.
     *
     * Not at the syscall level -- `vfs_mount()` returning EBUSY is right, because there is
     * one and mounting over it would hide what is behind it -- but `mount(2)` is a request
     * for a filesystem to be *available* at a path, and a system where it already is has
     * done what was asked.  This is what `init` does: it mounts /dev and /proc, which the
     * kernel has usually already done, and an init that reported an error every boot would
     * be an init nobody reads the log of. */
    if (r == -EBUSY) {
        for (unsigned i = 0; i < vfs_mount_count(); i++) {
            if (strcmp(vfs_mount_path(i), target) == 0 && vfs_mount_kind(i) == kind)
                return 0;
        }
    }

    return r;
}

long fs_sys_umount(regs_t *regs) {
    char target[VFS_NAME_MAX * 2];
    long r = path_arg(regs, 0, target, sizeof(target));

    if (r < 0)
        return r;

    return vfs_umount(target);
}

long fs_sys_access(regs_t *regs) {
    char path[VFS_NAME_MAX * 2];
    long r = path_arg(regs, 0, path, sizeof(path));

    if (r < 0)
        return r;

    bool exists = false;
    bool is_dir = false;

    r = vfs_access(sched_current(), path, &exists, &is_dir);

    if (r != 0)
        return r;

    /* F_OK and its relatives.  Every process here is root, so the permission half of the
     * answer is always yes and only the kind is a question -- and saying so is better than
     * a check that looks real and is not. */
    int mode = (int)(int64_t)syscall_arg(regs, 1);

    if (mode == 4)               /* R_OK */
        return 0;
    if (mode == 2)               /* W_OK */
        return is_dir ? -EISDIR : 0;
    if (mode == 1)               /* X_OK */
        return 0;
    if (mode == 0)               /* F_OK */
        return 0;

    return -EINVAL;
}

/* --- descriptor control ------------------------------------------------------ */

/* The few `fcntl` commands that mean something here.
 *
 * F_DUPFD, F_GETFD and F_SETFD are the ones a shell and an editor actually use; the rest
 * return EINVAL rather than ENOSYS, because ENOSYS on a `fcntl` says "this kernel does not
 * have descriptors" and EINVAL says "not that one", which is the truth. */
#define F_DUPFD   0
#define F_GETFD   1
#define F_SETFD   2
#define F_GETFL   3
#define F_SETFL   4

long fs_sys_fcntl(regs_t *regs) {
    int fd = (int)(int64_t)syscall_arg(regs, 0);
    int cmd = (int)(int64_t)syscall_arg(regs, 1);
    fd_entry_t *e = fd_get(fd);

    if (e == NULL)
        return -EBADF;

    switch (cmd) {
    case F_DUPFD: {
        int want = (int)(int64_t)syscall_arg(regs, 2);

        if (want < 0 || want >= FD_TABLE_ENTRIES)
            return -EINVAL;

        for (int i = want; i < FD_TABLE_ENTRIES; i++) {
            if (fd_get(i) != NULL)
                continue;
            int copy = fd_alloc(e->kind, e->obj, e->pipe_end);

            if (copy < 0)
                return -EMFILE;
            if (e->kind == FD_PIPE)
                pipe_retain((pipe_t *)e->obj, e->pipe_end);
            return copy;
        }
        return -EMFILE;
    }

    case F_GETFD:
        return 0;           /* no descriptor is close-on-exec yet */

    case F_SETFD:
        return 0;

    case F_GETFL: {
        file_t *f = fd_file(fd);

        if (f != NULL)
            return (long)f->flags;
        return O_RDWR;
    }

    case F_SETFL: {
        file_t *f = fd_file(fd);

        if (f == NULL)
            return -EBADF;

        /* Only the flags that change behaviour from here on are taken; the access mode is
         * fixed at open, because changing it afterwards would mean a program could open a
         * file read-only and then make it writable. */
        f->flags = (f->flags & (uint32_t)(O_WRONLY | O_RDWR)) |
                   ((uint32_t)syscall_arg(regs, 2) & ~(uint32_t)(O_WRONLY | O_RDWR));
        return 0;
    }

    default:
        return -EINVAL;
    }
}

/* `ioctl`: the terminal's commands and nothing else.
 *
 * Every one of the terminal calls needs a *task* on the other end of the console, and that
 * is the tty layer.  Until it exists the answer is ENOTTY -- "this is not a terminal" --
 * rather than ENOSYS, because for a file descriptor that is not a terminal, ENOTTY is
 * exactly right, and a program that checks gets the answer it can act on. */
long fs_sys_ioctl(regs_t *regs) {
    int fd = (int)(int64_t)syscall_arg(regs, 0);
    file_t *f = fd_file(fd);

    if (f == NULL)
        return -ENOTTY;

    if (ramfs_node_kind(f->ino) != RAMFS_DEVICE)
        return -ENOTTY;

    ramfs_dev_t dev = ramfs_node_device(f->ino);

    if (dev != RAMFS_DEV_CONSOLE)
        return -ENOTTY;

    /* Four names for the same terminal: /dev/console, /dev/tty, /dev/tty0, /dev/ttyS0.
     * A program that opens /dev/tty and one that opens /dev/ttyS0 want the same terminal
     * and must not have to care which the filesystem gave it. */
    return tty_ioctl(f, (uint32_t)syscall_arg(regs, 1), syscall_arg(regs, 2),
                     syscall_arg(regs, 3));
}