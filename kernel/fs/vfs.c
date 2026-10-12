/* minx - the virtual filesystem.  See vfs.h for what this is and what it deliberately does
 * not cache. */
#include "vfs.h"

#include "console.h"
#include "devfs.h"
#include "percpu.h"
#include "procfs.h"
#include "delay.h"
#include "timer.h"
#include "kprintf.h"
#include "kmalloc.h"
#include "string.h"
#include "syscall.h"
#include "task.h"

/* --- the mount table ------------------------------------------------------ */

#define VFS_MAX_MOUNTS 8
#define VFS_MOUNT_PATH 64

typedef struct {
    char        path[VFS_MOUNT_PATH];
    fs_kind_t   kind;
    uint64_t    root;          /* the ramfs node at the root of a ramfs mount */
} mount_t;

/* A fixed table with a fixed limit, because a mount table that can grow is a mount table
 * that has to be locked and a `mount` that has to be able to fail half way.  Four mounts is
 * two more than this kernel uses; eight leaves room to grow without the code that has to
 * check the bound being new. */
static mount_t mounts[VFS_MAX_MOUNTS];
static unsigned mount_count;

unsigned vfs_mount_count(void) { return mount_count; }

const char *vfs_mount_path(unsigned index) {
    return index < mount_count ? mounts[index].path : NULL;
}

fs_kind_t vfs_mount_kind(unsigned index) {
    return index < mount_count ? mounts[index].kind : FS_RAMFS;
}

int vfs_init(void) {
    /* Already there: both builders are idempotent and this is called once, but a second
     * call from a test must not add a second "/" to the table. */
    for (unsigned i = 0; i < mount_count; i++) {
        if (strcmp(mounts[i].path, "/") == 0)
            return 0;
    }

    mount_t *m = &mounts[mount_count];

    memset(m, 0, sizeof(*m));
    strlcpy(m->path, "/", VFS_MOUNT_PATH);
    m->kind = FS_RAMFS;
    m->root = ramfs_root();
    mount_count++;
    return 0;
}

long vfs_mount(const char *source, const char *target, fs_kind_t kind) {
    (void)source;

    if (target == NULL || target[0] != '/')
        return -EINVAL;

    if (mount_count >= VFS_MAX_MOUNTS)
        return -ENOSPC;

    /* An absolute path with no trailing slash except when it is "/" itself, so that
     * "/dev" and "/dev/" are one mount and not two prefixes that both match. */
    size_t len = strlen(target);

    while (len > 1 && target[len - 1] == '/')
        len--;

    if (len >= VFS_MOUNT_PATH)
        return -ENAMETOOLONG;

    /* Already mounted there is not an error that needs inventing a number for: mounting
     * /dev over /dev is what a second `mount -t devfs dev /dev` does, and the honest
     * answer is that it is already mounted.  Reporting it as success means a system with
     * two filesystems at one path has two and the second is invisible. */
    for (unsigned i = 0; i < mount_count; i++) {
        if (strncmp(mounts[i].path, target, len) == 0 && mounts[i].path[len] == 0)
            return -EBUSY;
    }

    /* The new filesystem's root is the *directory it is mounted on*, found in the
     * filesystem that contains it.
     *
     * Every filesystem here being the same ramfs is the trap: they are separate namespaces
     * that happen to share a node pool, so "the root" for a mount at /dev is the node named
     * `dev`, not node 1.  Using node 1 is the bug this replaces, and its symptom is that
     * every path *at* the mount point resolves and every path *under* it does not --
     * `/dev` is the root filesystem's own directory and exists, and `/dev/console` is not in
     * it and does not. */
    uint64_t root = ramfs_root();

    if (root == 0)
        return -ENOENT;

    if (len != 1) {
        char dir[VFS_MOUNT_PATH];
        vpath_t found;

        memcpy(dir, target, len);
        dir[len] = 0;

        if (vfs_resolve_abs(dir, true, &found) != 0)
            return -ENOENT;
        if (ramfs_node_kind(found.ino) != RAMFS_DIR)
            return -ENOTDIR;

        root = found.ino;
    }

    mount_t *m = &mounts[mount_count];

    memset(m, 0, sizeof(*m));
    memcpy(m->path, target, len);
    m->path[len] = 0;
    m->kind = kind;
    m->root = root;

    /* Mounting a filesystem at a directory that does not exist is not an error worth
     * refusing over: `/dev` in this initrd is created by the unpacker, and a mount that
     * arrives before the directory does would otherwise fail on ordering rather than on
     * anything about the mount. */
    mount_count++;

    kprintf("vfs: mounted %s at %s\n",
            kind == FS_DEVFS ? "devfs" : (kind == FS_PROCFS ? "procfs" : "ramfs"),
            m->path);
    return 0;
}

long vfs_umount(const char *target) {
    if (target == NULL)
        return -EINVAL;

    /* The root is not something a process can take away from the machine. */
    if (strcmp(target, "/") == 0)
        return -EBUSY;

    for (unsigned i = 0; i < mount_count; i++) {
        if (strcmp(mounts[i].path, target) != 0)
            continue;

        for (unsigned j = i; j + 1 < mount_count; j++)
            mounts[j] = mounts[j + 1];

        mount_count--;
        return 0;
    }

    return -EINVAL;
}

/* The longest mount whose path is a whole-component prefix of `path`.
 *
 * Longest rather than first, and that is the whole algorithm: `/dev/console` matches both
 * `/` and `/dev`, and the mount that has to win is the longer one.  The components are
 * compared rather than the raw bytes so that `/devel` does not match `/dev` -- a prefix
 * match on strings says it does, and a mount at `/dev` then owns a directory it never
 * asked for. */
static unsigned mount_for(const char *path, const char **rest) {
    unsigned best = 0;
    size_t best_len = 0;

    for (unsigned i = 0; i < mount_count; i++) {
        size_t len = strlen(mounts[i].path);

        if (strncmp(path, mounts[i].path, len) != 0)
            continue;
        if (len != 1 && path[len] != '/')
            continue;
        if (len > best_len) {
            best = i;
            best_len = len;
        }
    }

    *rest = path + best_len;

    /* "/" leaves a leading slash on what is left, and "/dev" leaves none.  Strip exactly
     * one leading separator either way so the walk below never sees an empty component
     * from the boundary. */
    if (**rest == '/')
        (*rest)++;

    return best;
}

/* --- resolution ----------------------------------------------------------- */

/* How many symbolic links one resolution may follow.
 *
 * Forty is the traditional answer and it is here for the same reason: a link that points
 * at itself is an infinite loop, and a kernel that follows it is a machine that stops
 * answering.  ELOOP says what happened, which "no such file" does not. */
#define VFS_MAX_LINKS 40

/* Resolve within one filesystem, from `start`, walking `rest`.
 *
 * `rest` is a `/`-separated remainder.  The walk is the whole of it: `.` is skipped, `..`
 * goes to the node's parent, and everything else is a name looked up in the current
 * directory. */
static long walk(uint32_t mount, uint64_t start, const char *rest, bool follow_final,
                 vpath_t *out) {
    uint64_t ino = start;
    unsigned links = 0;

    for (;;) {
        while (*rest == '/')
            rest++;
        if (*rest == 0)
            break;

        const char *slash = strchr(rest, '/');
        size_t len = slash != NULL ? (size_t)(slash - rest) : strlen(rest);

        if (len == 0 || len >= VFS_NAME_MAX)
            return -ENAMETOOLONG;

        if (len == 1 && rest[0] == '.') {
            rest += 1;
            continue;
        }

        if (len == 2 && rest[0] == '.' && rest[1] == '.') {
            /* "Up" from the root of the filesystem is the root of the filesystem, not an
             * error and not the parent of the mount point.  A process can `cd ..` all day
             * and stay in its own filesystem, which is what a chroot-ish root is for. */
            ino = ramfs_node_parent(ino);
            if (ino == 0)
                ino = mounts[mount].root;
            rest += 2;
            continue;
        }

        char name[VFS_NAME_MAX];
        memcpy(name, rest, len);
        name[len] = 0;

        /* `/proc/self` is the calling process's own directory, and it cannot be a stored
         * name.
         *
         * A link with a fixed target would be a link to *some* process -- whichever one
         * happened to be current when that directory was built -- which is wrong for every
         * reader but one.  A plain file does not resolve at all, so `/proc/self/status`,
         * which is the line half of every program that reports on itself, comes back
         * ENOENT.
         *
         * So the component is replaced here, while the walk is still looking at a name,
         * and everything after it is the ordinary lookup of a numeric directory that every
         * other `/proc/<pid>/...` path already does.  One rule, two paths, and `self`
         * cannot fall out of step with the numbering it stands for. */
        if (mounts[mount].kind == FS_PROCFS && len == 4 &&
            memcmp(name, "self", 4) == 0) {
            task_t *me = sched_current();

            if (me != NULL && utoa(me->id, 10, false, name, sizeof(name)) < 0)
                return -ENOENT;
        }

        uint64_t next = ramfs_lookup(ino, name);

        if (next == 0)
            return -ENOENT;

        rest += len;

        /* A link in the middle of a path is always followed, and a link at the end is
         * followed unless the caller asked for the link itself -- which is the difference
         * between `stat` and `lstat`, and it is the only difference between them. */
        if (ramfs_node_kind(next) == RAMFS_SYMLINK &&
            (follow_final || *rest != 0)) {
            if (++links > VFS_MAX_LINKS)
                return -ELOOP;

            const char *target = ramfs_node_link(next);

            if (target == NULL || target[0] == 0)
                return -ENOENT;

            /* An absolute link restarts at the root, which means re-deciding which
             * filesystem it is in; a relative one is resolved against the directory the
             * link is in, which is the node we are standing in. */
            if (target[0] == '/') {
                const char *ignored;
                mount = mount_for(target, &ignored);
                ino = mounts[mount].root;
                rest = ignored;
                while (*rest == '/')
                    rest++;
                continue;
            }

            /* Splice the target in front of what is left.  Copied into a buffer rather
             * than pointed at, because the target's own storage is the node's and the
             * remainder is the caller's string -- and a symbolic link to a relative path
             * is the common case, so this has to work, not just not crash. */
            char spliced[VFS_MOUNT_PATH + VFS_NAME_MAX + 2];
            size_t tlen = strlen(target);
            size_t rlen = strlen(rest);

            if (tlen + rlen + 2 > sizeof(spliced))
                return -ENAMETOOLONG;

            memcpy(spliced, target, tlen);
            spliced[tlen] = '/';
            memcpy(spliced + tlen + 1, rest, rlen + 1);

            /* The remainder after a relative link belongs to the link's *directory*, not
             * to the original caller, so the walk continues from here. */
            rest = spliced;
            continue;
        }

        ino = next;
    }

    out->mount = mount;
    out->ino = ino;
    return 0;
}

long vfs_resolve_abs(const char *path, bool follow_final, vpath_t *out) {
    if (path == NULL || mount_count == 0)
        return -ENOENT;

    if (path[0] != '/') {
        /* No task, so there is no current directory to be relative to.  Resolving a
         * relative path with nothing to be relative to is a caller's mistake, and saying
         * so is better than quietly treating it as absolute -- which would make
         * "open(\"etc/passwd\")" find /etc/passwd, a file the caller did not name. */
        return -ENOENT;
    }

    const char *rest;
    uint32_t mount = mount_for(path, &rest);

    char work[VFS_MOUNT_PATH + VFS_NAME_MAX + 2];
    size_t len = strlen(rest);

    if (len + 1 > sizeof(work))
        return -ENAMETOOLONG;

    memcpy(work, rest, len + 1);
    return walk(mount, mounts[mount].root, work, follow_final, out);
}

long vfs_resolve(task_t *t, const char *path, bool follow_final, vpath_t *out) {
    if (path == NULL || mount_count == 0)
        return -ENOENT;

    if (path[0] == '/')
        return vfs_resolve_abs(path, follow_final, out);

    /* Relative, so it starts where the caller is.  `cwd` is a resolved node rather than a
     * string so that a directory that has been renamed underneath the process still works:
     * a process holding a path has to be told the path is gone, and a process holding a
     * directory is where it is. */
    const char *start = (t != NULL && t->cwd[0] != 0) ? t->cwd : "/";
    const char *rest;
    uint32_t mount = mount_for(start, &rest);

    /* A relative path can walk out of the filesystem the cwd is in and into another one
     * only through `..`, and `..` from a mount root stays there, so the rest of the walk
     * stays in this filesystem.  That is checked rather than assumed: it is what makes
     * "/dev/../etc/passwd" name a file rather than a hole. */
    if (ramfs_node_kind(mounts[mount].root) != RAMFS_DIR)
        return -ENOTDIR;

    uint64_t start_node = t != NULL ? t->cwd_ino : 0;

    if (start_node == 0)
        return -ENOENT;

    char work[VFS_MOUNT_PATH + VFS_NAME_MAX + 2];
    size_t len = strlen(path);

    if (len + 1 > sizeof(work))
        return -ENAMETOOLONG;

    memcpy(work, path, len + 1);
    return walk(mount, start_node, work, follow_final, out);
}

/* The path of a node, built from the chain of parents.
 *
 * Walked backwards: a node's name plus its parent, all the way to the mount root, then the
 * mount's own path in front.  The limit is on the number of components rather than on the
 * length, because a deep tree is what makes the *string* long and the string is what the
 * buffer is for. */
const char *vfs_pathname(const vpath_t *p, char *buf, size_t len) {
    if (p == NULL || p->mount >= mount_count || len == 0) {
        if (len > 0)
            buf[0] = 0;
        return buf;
    }

    /* Collected back to front, then reversed.  Eight components is 128 bytes, which is
     * deeper than anything this kernel's own tree gets and shallow enough that a cycle --
     * which `..` cannot produce but a corrupted node could -- stops rather than spins. */
    char parts[8][VFS_NAME_MAX];
    unsigned count = 0;
    uint64_t ino = p->ino;

    while (ino != 0 && count < 8) {
        const char *name = ramfs_node_name(ino);

        if (name == NULL || name[0] == 0)
            break;

        strlcpy(parts[count], name, VFS_NAME_MAX);
        count++;

        uint64_t parent = ramfs_node_parent(ino);

        if (parent == ino)
            break;
        ino = parent;
    }

    const mount_t *m = &mounts[p->mount];
    size_t used = 0;

    buf[0] = 0;
    if (strcmp(m->path, "/") != 0) {
        used = strlcpy(buf, m->path, len);
        if (used >= len)
            return "?";
    }

    for (unsigned i = count; i > 0; i--) {
        if (buf[0] != 0) {
            if (used + 1 < len)
                buf[used++] = '/';
            buf[used] = 0;
        }
        size_t n = strlcpy(buf + used, parts[i - 1], len - used);

        if (n >= len - used)
            return "?";
        used += n;
    }

    if (buf[0] == 0)
        strlcpy(buf, "/", len);

    return buf;
}

/* --- the random device's generator ---------------------------------------- */

/* An xorshift64*, seeded once from the timebase and the processor that asked first.
 *
 * Not cryptographic, and nothing here pretends otherwise: a program that needs a key needs
 * a real one and this is not it.  What it *is* good for is exactly what /dev/urandom is
 * usually asked for inside an operating system -- spreading hash seeds, choosing ports,
 * filling scratch -- and it is here rather than left unimplemented because a program that
 * opens /dev/urandom and gets zeroes has silently stopped being random, which is the worst
 * possible failure for that device. */
static uint64_t random_state;

static uint64_t random_next(void) {
    if (random_state == 0) {
        uint64_t seed = monotonic_ns() ^ ((uint64_t)percpu_self_index() << 56);

        /* A zero state is a fixed point of xorshift, so a seed that comes out zero is
         * replaced rather than left alone: a generator stuck at zero is a device that
         * returns the same byte for ever. */
        random_state = seed != 0 ? seed : 0x9e3779b97f4a7c15ull;
    }

    random_state ^= random_state >> 12;
    random_state ^= random_state << 25;
    random_state ^= random_state >> 27;
    return random_state * 0x2545f4914f6cdd1dull;
}

/* --- per-task state ------------------------------------------------------- */

const char *vfs_cwd(task_t *t, char *buf, size_t len) {
    if (t == NULL) {
        if (len > 0)
            buf[0] = 0;
        return buf;
    }
    strlcpy(buf, t->cwd, len);
    return buf;
}

long vfs_getcwd(task_t *t, char *buf, size_t len) {
    if (t == NULL || buf == NULL || len == 0)
        return -EFAULT;

    if (t->cwd[0] == 0)
        return -ENOENT;

    size_t n = strlcpy(buf, t->cwd, len);

    /* A caller that asked for too little is told how much it needs rather than handed a
     * truncated path: a truncated path names a different directory, and a program that
     * chdir's into it ends up somewhere it did not ask to be. */
    if (n >= len)
        return -(int)n;

    return (long)n;
}

long vfs_chdir(task_t *t, const char *path) {
    if (t == NULL)
        return -EFAULT;

    /* An empty path means the process's own directory, which is what it already is. */
    if (path == NULL || path[0] == 0) {
        strlcpy(t->cwd, ".", sizeof(t->cwd));
        return 0;
    }

    vpath_t p;
    long r = vfs_resolve(t, path, true, &p);

    if (r != 0)
        return r;

    if (ramfs_node_kind(p.ino) != RAMFS_DIR)
        return -ENOTDIR;

    /* The name is kept as written rather than as resolved, because that is what makes
     * `getcwd` answer with something a person recognises.  The node is kept too, because
     * that is what makes the *next* lookup work after the directory has been renamed. */
    strlcpy(t->cwd, path[0] == '/' ? path : path, sizeof(t->cwd));
    t->cwd_ino = p.ino;
    t->cwd_mount = p.mount;
    return 0;
}

/* --- stat ----------------------------------------------------------------- */

static mode_t kind_to_mode(ramfs_kind_t kind, mode_t mode) {
    /* The type bits come from the node's kind rather than from its mode, because the mode
     * a node is created with is a permission set and the type is what it *is*.  A node
     * created with mode 0644 and no type bits is a regular file, not a file of unknown
     * type, and `ls -l` has to be able to say which. */
    switch (kind) {
    case RAMFS_DIR:    return S_IFDIR | (mode & 07777);
    case RAMFS_SYMLINK: return S_IFLNK | (mode & 07777);
    case RAMFS_DEVICE: return S_IFCHR | (mode & 07777);
    case RAMFS_FILE:
    default:           return S_IFREG | (mode & 07777);
    }
}

/* What goes in a directory entry's `d_type`.
 *
 * Not the `S_IFMT` bits, which `uint8_t` truncates to zero -- see the comment on the DT_*
 * constants in vfs.h.  A filesystem that cannot name its own node types here forces every
 * program to `stat` every name, which is a syscall per directory entry to learn something
 * the kernel already knows. */
static uint8_t kind_to_dtype(ramfs_kind_t kind) {
    switch (kind) {
    case RAMFS_DIR:     return DT_DIR;
    case RAMFS_SYMLINK: return DT_LNK;
    case RAMFS_DEVICE:  return DT_CHR;
    case RAMFS_FILE:
    default:            return DT_REG;
    }
}

/* Append one directory entry to the caller's buffer.
 *
 * False means the record did not fit, which is not a short record: the buffer is full, the
 * caller's `d_off` is where it should resume, and the entry is still there next time.  A
 * `getdents` that truncates instead returns a name with no terminator, which every reader
 * then treats as a very long one. */
static bool readdir_emit(uint8_t *out, size_t len, size_t *used, uint64_t ino,
                         uint64_t next_off, uint8_t type, const char *name) {
    vfs_dirent_t d;

    if (*used + sizeof(d) > len)
        return false;

    memset(&d, 0, sizeof(d));
    d.d_ino = ino;
    d.d_off = next_off;
    d.d_type = type;
    strlcpy(d.d_name, name, VFS_NAME_MAX);
    d.d_reclen = (uint16_t)sizeof(d);

    memcpy(out + *used, &d, sizeof(d));
    *used += sizeof(d);
    return true;
}

long vfs_stat(const vpath_t *p, stat_t *out) {
    if (p == NULL || out == NULL)
        return -EFAULT;
    if (p->mount >= mount_count)
        return -ENOENT;

    const mount_t *m = &mounts[p->mount];

    memset(out, 0, sizeof(*out));
    out->st_dev = p->mount + 1;      /* 1 for the root, which is what dev_t 0 is not */
    out->st_ino = p->ino;
    out->st_blksize = 4096;

    if (m->kind == FS_PROCFS) {
        out->st_mode = S_IFREG | 0444;
        out->st_nlink = 1;
        out->st_size = procfs_size(p, 0);
        return 0;
    }

    ramfs_kind_t kind = ramfs_node_kind(p->ino);

    if (kind == RAMFS_NONE)
        return -ENOENT;

    out->st_mode = kind_to_mode(kind, ramfs_node_mode(p->ino));
    out->st_nlink = (uint32_t)ramfs_node_nlink(p->ino);
    out->st_uid = (uint32_t)ramfs_node_uid(p->ino);
    out->st_gid = (uint32_t)ramfs_node_gid(p->ino);
    out->st_size = kind == RAMFS_SYMLINK
                       ? (ramfs_node_link(p->ino) != NULL
                              ? strlen(ramfs_node_link(p->ino))
                              : 0)
                       : ramfs_node_size(p->ino);
    out->st_blocks = out->st_size / 512;
    out->st_atime = ramfs_node_atime(p->ino);
    out->st_mtime = ramfs_node_mtime(p->ino);
    out->st_ctime = ramfs_node_ctime(p->ino);
    return 0;
}

long vfs_stat_path(task_t *t, const char *path, bool follow, stat_t *out) {
    vpath_t p;
    long r = vfs_resolve(t, path, follow, &p);

    if (r != 0)
        return r;

    return vfs_stat(&p, out);
}

/* --- directories ----------------------------------------------------------- */

long vfs_readdir(const vpath_t *p, uint64_t *offset, void *buf, size_t len) {
    if (p == NULL || buf == NULL || offset == NULL)
        return -EFAULT;
    if (p->mount >= mount_count || p->ino == 0)
        return -ENOENT;

    if (ramfs_node_kind(p->ino) != RAMFS_DIR)
        return -ENOTDIR;

    /* One pass over the node table per call, picking out the names whose parent is this
     * directory.  The offset is a count of names already returned, which is what makes it
     * resumable: a caller that comes back with the offset it was given picks up where it
     * left off without the kernel having to remember a position.
     *
     * It is O(nodes) per call rather than O(children), which is the wrong way round at
     * this size and the right way at no size: a directory that can be modified while it
     * is being read has no stable child list to hold a cursor against, and an index that
     * is "how many names have I produced" stays correct when another process renames
     * something else. */
    unsigned skip = (unsigned)*offset;
    unsigned produced = 0;
    size_t used = 0;
    uint8_t *out = (uint8_t *)buf;
    unsigned total = ramfs_node_total();

    /* `.` and `..` first, then the real names.
     *
     * A directory has no link to itself and no link to its parent -- storing either would
     * put the same node in the tree twice, which is exactly what keeps the tree a tree --
     * so they are made here, at the point where a directory becomes a list of names.
     *
     * They are not decoration.  `find` cannot terminate on a directory with no `..`, and
     * `chdir("..")` resolves to nothing, which is a resolver that cannot go back up a path
     * it did not come down.  And `..` in the root is the root, which is the one place it
     * is not a link to anything and still has to be right.
     *
     * Counting them in `produced` is what keeps the resume offset honest: the offset is a
     * count of names already handed out, and `.` was handed out before any real name, so it
     * has to be in the count or every call after the first restarts in the wrong place. */
    struct {
        uint64_t ino;
        const char *name;
    } synth[2] = {
        { p->ino,                                  "."  },
        { ramfs_node_parent(p->ino) ?: p->ino,     ".." },
    };

    for (unsigned k = 0; k < 2; k++) {
        if (produced++ < skip)
            continue;

        if (!readdir_emit(out, len, &used, synth[k].ino, produced, DT_DIR,
                          synth[k].name)) {
            if (produced - 1 == skip)
                return -ENOSPC;   /* the buffer cannot hold even one entry */
            *offset = produced - 1;
            return (long)used;
        }
    }

    for (unsigned i = 0; i < total; i++) {
        const ramfs_node_t *n = ramfs_node_at(i);

        if (n == NULL || n->kind == RAMFS_NONE)
            continue;

        for (unsigned l = 0; l < RAMFS_MAX_LINKS; l++) {
            if (!n->links[l].used || n->links[l].parent != p->ino)
                continue;
            if (produced++ < skip)
                continue;

            if (!readdir_emit(out, len, &used, n->ino, produced,
                              kind_to_dtype(n->kind), n->links[l].name)) {
                /* A record that does not fit is not a short record: the caller's buffer
                 * is full, and the entry stays for the next call because `offset` has not
                 * been advanced past it. */
                if (produced - 1 == skip)
                    return -ENOSPC;
                *offset = produced - 1;
                return (long)used;
            }
        }
    }

    *offset = produced;
    return (long)used;
}

/* --- files ----------------------------------------------------------------- */

long vfs_open(const vpath_t *p, uint32_t flags, file_t **out) {
    if (p == NULL || out == NULL)
        return -EFAULT;
    if (p->mount >= mount_count)
        return -ENOENT;

    file_t *f = (file_t *)kmalloc(sizeof(*f));

    if (f == NULL)
        return -ENOMEM;

    memset(f, 0, sizeof(*f));
    f->mount = p->mount;
    f->ino = p->ino;
    f->flags = flags;
    f->refs = 1;
    f->writable = (flags & (O_WRONLY | O_RDWR)) != 0;

    ramfs_kind_t kind = ramfs_node_kind(p->ino);

    if (mounts[p->mount].kind != FS_PROCFS) {
        if (kind == RAMFS_NONE) {
            kfree(f);
            return -ENOENT;
        }

        if (kind == RAMFS_DIR && (flags & (O_WRONLY | O_RDWR))) {
            kfree(f);
            return -EISDIR;
        }

        if ((flags & O_DIRECTORY) && kind != RAMFS_DIR) {
            kfree(f);
            return -ENOTDIR;
        }

        f->is_dir = kind == RAMFS_DIR;

        if (kind == RAMFS_DEVICE)
            f->dev = ramfs_node_device(p->ino);
    } else {
        f->is_dir = false;
        f->proc_id = PROCFS_SELF;
    }

    if (flags & O_TRUNC) {
        if (!f->writable) {
            kfree(f);
            return -EACCES;
        }
        (void)ramfs_truncate(p->ino, 0);
    }

    if (flags & O_APPEND)
        f->offset = ramfs_node_size(p->ino);

    *out = f;
    return 0;
}

long vfs_close(file_t *f) {
    if (f == NULL)
        return -EBADF;

    kfree(f);
    return 0;
}

long vfs_read(file_t *f, void *buf, size_t len) {
    if (f == NULL || buf == NULL)
        return -EFAULT;

    if (mounts[f->mount].kind == FS_PROCFS) {
        size_t used = 0;
        long r = procfs_read(&(vpath_t){ f->mount, f->ino }, f->proc_id, buf, len, &used);

        f->offset += used;
        return r;
    }

    switch (ramfs_node_kind(f->ino)) {
    case RAMFS_DEVICE:
        switch (f->dev) {
        case RAMFS_DEV_ZERO: {
            /* Endless zeroes, and endless means *endless*: a read of any length from
             * /dev/zero succeeds, because there is always more.  A loop that fills a
             * buffer a kilobyte at a time is the whole reason this device exists. */
            memset(buf, 0, len);
            return (long)len;
        }
        case RAMFS_DEV_RANDOM:
        case RAMFS_DEV_URANDOM:
            /* Different bytes on every read, or the device is not a random device and a
             * program seeding from it has a key that is a constant.
             *
             * The one behaviour of the two names that is honestly wrong is that `/dev/random`
             * does not block until the pool has been seeded from somewhere better than a
             * clock -- there is nowhere better here, and a device that refuses reads is a
             * device a program cannot ask for bytes.  docs/DECISIONS.md says so by name
             * rather than letting it be quietly relied on for something it is not. */
            for (size_t i = 0; i < len; i++)
                ((uint8_t *)buf)[i] = (uint8_t)random_next();
            return (long)len;

        case RAMFS_DEV_NULL:
        case RAMFS_DEV_CONSOLE:
            /* The console has no input wired to a *file* yet -- the terminal is reached
             * through the descriptor table, which is what the tty layer is for -- so a read
             * of /dev/console is end of file rather than a hang. */
            return 0;

        default:
            return 0;
        }

    case RAMFS_DIR:
        return -EISDIR;

    default:
        break;
    }

    if ((f->flags & O_WRONLY) != 0)
        return -EBADF;

    uint64_t at = (f->flags & O_APPEND) ? ramfs_node_size(f->ino) : f->offset;
    long r = ramfs_read(f->ino, at, buf, len);

    if (r > 0)
        f->offset = at + (uint64_t)r;

    return r;
}

long vfs_write(file_t *f, const void *buf, size_t len) {
    if (f == NULL || buf == NULL)
        return -EFAULT;

    if (!f->writable)
        return -EBADF;

    if (mounts[f->mount].kind == FS_PROCFS)
        return -EROFS;

    /* Every device accepts a write.  Two of them keep it -- the console and its serial
     * mirror, which is the whole point of /dev/console -- and the rest throw it away, and
     * a device that refused writes would need a write-side error for a case the kernel has
     * no memory pressure yet.
     *
     * The test is on the node's *kind*, not on its device: `RAMFS_DEV_NULL` and
     * `RAMFS_DEV_ZERO` are values inside a device node and not node kinds, and switching
     * on them beside `RAMFS_DIR` compiles to two arms with the same value -- which the
     * compiler says, and which is a silent wrong answer everywhere else it happens. */
    if (ramfs_node_kind(f->ino) == RAMFS_DEVICE) {
        if (f->dev == RAMFS_DEV_CONSOLE)
            console_write((const char *)buf, len);
        return (long)len;
    }

    if (ramfs_node_kind(f->ino) == RAMFS_DIR)
        return -EISDIR;

    uint64_t at = (f->flags & O_APPEND) ? ramfs_node_size(f->ino) : f->offset;
    long r = ramfs_write(f->ino, at, buf, len);

    if (r > 0)
        f->offset = at + (uint64_t)r;

    return r;
}

long vfs_seek(file_t *f, int64_t offset, int whence) {
    if (f == NULL)
        return -EBADF;

    if (f->is_dir)
        return -ESPIPE;

    int64_t base;

    switch (whence) {
    case 0: base = 0; break;                          /* SEEK_SET */
    case 1: base = (int64_t)f->offset; break;        /* SEEK_CUR */
    case 2: base = (int64_t)ramfs_node_size(f->ino); break;   /* SEEK_END */
    default: return -EINVAL;
    }

    int64_t next = base + offset;

    /* A position before the start is not a position.  Refusing it rather than clamping
     * means `lseek` reports the mistake, and a caller that did not check gets EBADF-ish
     * behaviour instead of a silently different file offset. */
    if (next < 0)
        return -EINVAL;

    f->offset = (uint64_t)next;
    return (long)next;
}

long vfs_ftruncate(file_t *f, uint64_t size) {
    if (f == NULL)
        return -EBADF;
    if (!f->writable)
        return -EINVAL;

    return ramfs_truncate(f->ino, size);
}

long vfs_fstat(file_t *f, stat_t *out) {
    if (f == NULL || out == NULL)
        return -EFAULT;

    vpath_t p = { f->mount, f->ino };
    long r = vfs_stat(&p, out);

    if (r == 0 && (f->flags & O_APPEND))
        out->st_size = ramfs_node_size(f->ino);

    return r;
}

/* --- creating and removing ------------------------------------------------- */

/* Resolve a path for something that *changes* the tree.
 *
 * The last component is not resolved: it is the name being created, and resolving it would
 * report ENOENT for exactly the files that are supposed to be made.  What is resolved is
 * the directory the name goes in, which is the part that can be wrong in a way a caller
 * needs told about -- it may not exist, or it may not be a directory. */
static long resolve_parent(task_t *t, const char *path, uint32_t *mount, uint64_t *parent,
                           char *name, size_t name_len, bool *exists, uint64_t *existing) {
    if (path == NULL || name_len < VFS_NAME_MAX)
        return -EINVAL;

    const char *slash = strrchr(path, '/');
    const char *base = slash != NULL ? slash + 1 : path;

    if (base[0] == 0)
        return -EINVAL;

    strlcpy(name, base, name_len);

    char dir[VFS_MOUNT_PATH + VFS_NAME_MAX + 2];
    size_t dlen = (size_t)(base - path);

    if (dlen == 0) {
        dir[0] = '/';
        dir[1] = 0;
        dlen = 1;
    } else {
        if (dlen >= sizeof(dir))
            return -ENAMETOOLONG;
        memcpy(dir, path, dlen - 1);       /* drop the trailing slash */
        dir[dlen - 1] = 0;
    }

    vpath_t p;
    long r = vfs_resolve(t, dir[0] == 0 ? "/" : dir, true, &p);

    if (r != 0)
        return r;

    if (ramfs_node_kind(p.ino) != RAMFS_DIR)
        return -ENOTDIR;

    *mount = p.mount;
    *parent = p.ino;

    if (exists != NULL) {
        *exists = ramfs_lookup(p.ino, name) != 0;
        if (existing != NULL)
            *existing = ramfs_lookup(p.ino, name);
    }

    return 0;
}

long vfs_mkdir(task_t *t, const char *path, mode_t mode) {
    uint32_t mount;
    uint64_t parent, existing;
    char name[VFS_NAME_MAX];
    bool exists;

    long r = resolve_parent(t, path, &mount, &parent, name, sizeof(name), &exists,
                            &existing);

    if (r != 0)
        return r;

    if (exists)
        return -EEXIST;

    /* Only the root filesystem takes writes here.  devfs and procfs are generated, and a
     * directory created in one of them would be a name that resolves to nothing -- the
     * first of which is `/dev/mydir`, which every beginner makes and every filesystem
     * here has to refuse rather than pretend. */
    if (mounts[mount].kind != FS_RAMFS)
        return -EROFS;

    return ramfs_create(parent, name, RAMFS_DIR, mode | S_IFDIR) == 0 ? -EIO : 0;
}

long vfs_rmdir(task_t *t, const char *path) {
    uint32_t mount;
    uint64_t parent;
    char name[VFS_NAME_MAX];

    long r = resolve_parent(t, path, &mount, &parent, name, sizeof(name), NULL, NULL);

    if (r != 0)
        return r;

    if (mounts[mount].kind != FS_RAMFS)
        return -EROFS;

    return ramfs_rmdir(parent, name);
}

long vfs_unlink(task_t *t, const char *path) {
    uint32_t mount;
    uint64_t parent;
    char name[VFS_NAME_MAX];

    long r = resolve_parent(t, path, &mount, &parent, name, sizeof(name), NULL, NULL);

    if (r != 0)
        return r;

    if (mounts[mount].kind != FS_RAMFS)
        return -EROFS;

    return ramfs_unlink(parent, name);
}

long vfs_rename(task_t *t, const char *from, const char *to) {
    uint32_t m1, m2;
    uint64_t p1, p2;
    char n1[VFS_NAME_MAX], n2[VFS_NAME_MAX];
    long r = resolve_parent(t, from, &m1, &p1, n1, sizeof(n1), NULL, NULL);

    if (r != 0)
        return r;

    r = resolve_parent(t, to, &m2, &p2, n2, sizeof(n2), NULL, NULL);

    if (r != 0)
        return r;

    /* Across filesystems is EXDEV, not a copy.  A copy would be right for a user and
     * surprising for everything else, and a caller that wanted a copy has `cp`. */
    if (m1 != m2)
        return -EXDEV;

    return ramfs_rename(p1, n1, p2, n2);
}

long vfs_link(task_t *t, const char *from, const char *to) {
    vpath_t target;
    long r = vfs_resolve(t, from, false, &target);

    if (r != 0)
        return r;

    uint32_t mount;
    uint64_t parent;
    char name[VFS_NAME_MAX];

    r = resolve_parent(t, to, &mount, &parent, name, sizeof(name), NULL, NULL);

    if (r != 0)
        return r;

    if (mount != target.mount)
        return -EXDEV;

    return ramfs_link(parent, name, target.ino);
}

long vfs_symlink(task_t *t, const char *target, const char *path) {
    uint32_t mount;
    uint64_t parent;
    char name[VFS_NAME_MAX];
    long r = resolve_parent(t, path, &mount, &parent, name, sizeof(name), NULL, NULL);

    if (r != 0)
        return r;

    if (mounts[mount].kind != FS_RAMFS)
        return -EROFS;

    return ramfs_symlink(parent, name, target) == 0 ? -EIO : 0;
}

long vfs_readlink(task_t *t, const char *path, char *buf, size_t len) {
    vpath_t p;
    long r = vfs_resolve(t, path, false, &p);

    if (r != 0)
        return r;

    if (ramfs_node_kind(p.ino) != RAMFS_SYMLINK)
        return -EINVAL;

    const char *target = ramfs_node_link(p.ino);

    if (target == NULL)
        return -EINVAL;

    size_t n = strlen(target);

    if (n >= len)
        return -ERANGE;

    memcpy(buf, target, n + 1);
    return (long)n;
}

long vfs_truncate_path(task_t *t, const char *path, uint64_t size) {
    vpath_t p;
    long r = vfs_resolve(t, path, true, &p);

    if (r != 0)
        return r;

    if (ramfs_node_kind(p.ino) != RAMFS_FILE)
        return -EINVAL;

    return ramfs_truncate(p.ino, size);
}

long vfs_chmod(task_t *t, const char *path, mode_t mode) {
    vpath_t p;
    long r = vfs_resolve(t, path, true, &p);

    if (r != 0)
        return r;

    return ramfs_set_mode(p.ino, mode);
}

long vfs_chown(task_t *t, const char *path, uint64_t uid, uint64_t gid) {
    vpath_t p;
    long r = vfs_resolve(t, path, true, &p);

    if (r != 0)
        return r;

    return ramfs_chown(p.ino, uid, gid);
}

long vfs_utimes(task_t *t, const char *path, const uint64_t times[2]) {
    vpath_t p;
    long r = vfs_resolve(t, path, true, &p);

    if (r != 0)
        return r;

    return ramfs_set_times(p.ino, times[0], times[1]);
}

long vfs_access(task_t *t, const char *path, bool *exists, bool *is_dir) {
    vpath_t p;
    long r = vfs_resolve(t, path, true, &p);

    if (exists != NULL)
        *exists = r == 0;

    if (is_dir != NULL)
        *is_dir = r == 0 && ramfs_node_kind(p.ino) == RAMFS_DIR;

    return r;
}

long vfs_read_whole(const vpath_t *p, void **out, size_t *bytes_out) {
    if (p == NULL || out == NULL || bytes_out == NULL)
        return -EFAULT;

    if (ramfs_node_kind(p->ino) != RAMFS_FILE)
        return -EISDIR;

    return ramfs_read_whole(p->ino, out, bytes_out);
}