/* minx - the virtual filesystem.
 *
 * Three filesystems and the thing that makes them one: a path.  ramfs is the root and
 * holds everything the initrd unpacked into it; devfs is a device tree that has names and
 * no bytes; procfs is the kernel's own state as files.  None of them knows about the
 * others, and none of them needs to: `vfs_resolve()` finds which filesystem a path is in
 * and hands back a {mount, node} pair, and everything above that works in those two words.
 *
 * A dentry cache is the obvious thing to reach for next and is deliberately absent.  A path
 * is resolved by comparing one string against one table entry, so a cache would cache an
 * answer that is already one comparison -- and would then need invalidating on every
 * create, unlink and rename, which is where the bugs would be.
 *
 * What is here instead:
 *
 *   - a mount table, searched longest-prefix-first, because a mount at `/dev` must win
 *     over the root for `/dev/console` and a mount at `/` must lose to it;
 *   - path resolution with `.`, `..`, relative paths and a symbolic-link loop counter;
 *   - open files as a separate object from nodes, so that unlinking a file somebody has
 *     open does not pull it out from under them;
 *   - devfs and procfs behind the same interface, which is what makes `/dev/null` and
 *     `/proc/meminfo` ordinary paths rather than special cases in every call.
 */
#ifndef MINX_VFS_H
#define MINX_VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ramfs.h"
#include "types.h"

/* Forward-declared at file scope, and the forward declaration is the whole point.
 *
 * A struct first mentioned inside a function's parameter list has *prototype* scope: it
 * names a type that does not exist outside that one declaration.  So `vfs_resolve(struct
 * task *t, ...)` here and `typedef struct task { ... } task_t;` in task.h declare two
 * different, incompatible types with the same name -- and every file that sees both
 * rejects the other's declarations.  The first mention has to be at file scope, which is
 * what this line is, and everything below it can then use the same type task.h defines. */
struct task;

/* --- filesystems ---------------------------------------------------------- */

typedef enum {
    FS_RAMFS = 0,     /* the root: everything the initrd unpacked */
    FS_DEVFS,         /* /dev: names bound to things that are not bytes */
    FS_PROCFS,        /* /proc: the kernel's own state, as files */
} fs_kind_t;

/* What a path resolves to.
 *
 * `mount` is an index into the mount table and `ino` is a node number *inside that
 * filesystem*, so nothing outside this header has to know which of the three it is
 * looking at.  Zero means "no such path"; a real node's number is never zero, because
 * ramfs numbers its nodes from 1. */
typedef struct {
    uint32_t mount;
    uint64_t ino;
} vpath_t;

#define VPATH_NONE ((vpath_t){ 0, 0 })

/* --- the mount table ------------------------------------------------------ */

/* Every filesystem this kernel has.  `/` is in the table from the start rather than being
 * special: a root that is not a mount cannot be unmounted, cannot be reported by
 * `/proc/mounts`, and is one more thing that has to be checked for everywhere. */
unsigned        vfs_mount_count(void);
const char     *vfs_mount_path(unsigned index);
fs_kind_t       vfs_mount_kind(unsigned index);

/* Put the root filesystem in the table.  Called once, after ramfs_mount_initrd().
 *
 * Without it the mount table is empty and *nothing* resolves -- not even `/bin/hello` --
 * because path resolution starts by asking the table which filesystem a path is in.  The
 * root is a mount like any other, and saying so here is what keeps `vfs_resolve_abs()` from
 * quietly working for a path with no components in it and failing for every other one. */
int  vfs_init(void);

/* Mount a filesystem at an absolute path.  `source` is ignored by every filesystem here
 * and exists because `mount()` takes one.  0, or -errno. */
long vfs_mount(const char *source, const char *target, fs_kind_t kind);
long vfs_umount(const char *target);

/* --- resolution ----------------------------------------------------------- */

/* Resolve `path` for `t`, following a final symbolic link unless `follow_final` is false.
 *
 * `path` may be absolute or relative; a relative one starts at the calling task's current
 * directory.  0 on success, or -errno with nothing written to `out`.
 *
 * -ENOENT, -ENOTDIR and -ELOOP are the interesting failures, and all three are worth
 * distinguishing: "there is no such thing", "that is not a directory so there is nothing
 * under it", and "a link points at itself" are three different mistakes in a caller and
 * one answer in a kernel that conflates them. */
long vfs_resolve(struct task *t, const char *path, bool follow_final, vpath_t *out);

/* As above, but starting from an absolute path with no task, for the places that have no
 * calling task -- the kernel reading its own initrd, the tests. */
long vfs_resolve_abs(const char *path, bool follow_final, vpath_t *out);

/* The path a resolved node is at, as a caller would write it.  Fills `buf` and returns it,
 * or "?" if it cannot be written -- which happens when the path is longer than the
 * buffer, and saying so is better than a truncated path that names a different file. */
const char *vfs_pathname(const vpath_t *p, char *buf, size_t len);

/* Attributes, and the one structure a `stat` is. */
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    mode_t   st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
    uint64_t st_atime;
    uint64_t st_mtime;
    uint64_t st_ctime;
} stat_t;

/* `lstat` follows no final link; `stat` does; `fstat` has no path at all. */
long vfs_stat(const vpath_t *p, stat_t *out);
long vfs_stat_path(struct task *t, const char *path, bool follow, stat_t *out);
/* --- directories ---------------------------------------------------------- */

/* One entry, as `getdents` hands them out.  The record length is in the structure itself,
 * so a caller that does not like the name length can skip to the next one -- which is the
 * whole reason the name is a fixed 256 bytes rather than a pointer into a kernel buffer. */
#define VFS_NAME_MAX 256

/* What `d_type` holds: the `DT_*` values, not the `S_IFMT` mode bits.
 *
 * This was `S_IFREG` and `S_IFDIR` in a `uint8_t`, which cannot work: `S_IFMT` is 0170000,
 * the values are 0100000 and 0040000, and a cast to eight bits turns both of them into 0.
 * Every entry came back saying "unknown", which is a value a program is *required* to
 * handle but which tells it nothing.
 *
 * The `DT_*` values are the right answer for two reasons.  They fit, which the mode bits
 * do not; and they are what every program already switches on -- `d_type == DT_DIR` is
 * how `ls`, `find` and a shell's tab completion avoid a `stat` per name.  A kernel that
 * reports the mode bits instead is a kernel whose programs each have to know that here the
 * numbers mean something else. */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

typedef struct {
    uint64_t d_ino;
    uint64_t d_off;        /* the offset to resume from, so `d_off` of this entry */
    uint16_t d_reclen;
    uint8_t  d_type;       /* one of the DT_* values above */
    uint8_t  d_pad[7];
    char     d_name[VFS_NAME_MAX];
} vfs_dirent_t;

/* Read directory entries into `buf`, starting at `*offset` and updating it.  Returns the
 * number of bytes written, 0 at the end of the directory, or -errno. */
long vfs_readdir(const vpath_t *p, uint64_t *offset, void *buf, size_t len);

/* --- files ---------------------------------------------------------------- */

/* An open file.  Separate from a node because a node outlives its names: unlinking a file
 * that somebody has open must not make their descriptor read something else. */
typedef struct file {
    uint32_t mount;
    uint64_t ino;
    uint64_t offset;
    uint32_t flags;        /* the open flags, for O_APPEND and the rest */
    uint32_t refs;         /* how many descriptors name it */
    ramfs_dev_t dev;       /* FS_DEVFS: what it is bound to */
    uint64_t proc_id;      /* FS_PROCFS: which process, or PROCFS_SELF */
    bool     is_dir;
    bool     writable;
} file_t;

/* Open flags.  The same numbers as every other Unix, because a program's source says
 * O_RDONLY and means it. */
#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_CREAT    0x0040
#define O_EXCL     0x0080
#define O_TRUNC    0x0200
#define O_APPEND   0x0400
#define O_DIRECTORY 0x10000

/* Which process's status a procfs node describes, when the name is not a number. */
#define PROCFS_SELF  (~0ull)

/* Open, close, read, write, seek.  All take the resolved path rather than a string, so a
 * caller that has already resolved something does not resolve it twice. */
long vfs_open(const vpath_t *p, uint32_t flags, file_t **out);
long vfs_close(file_t *f);

/* The reference count on a `file_t`, moved by every path that makes a second descriptor
 * name the same open file: `dup`, `dup2` and `fork`.  Without them `close` frees a file
 * that another descriptor still points at. */
void vfs_file_retain(file_t *f);
long vfs_file_release(file_t *f);
long vfs_read(file_t *f, void *buf, size_t len);
long vfs_write(file_t *f, const void *buf, size_t len);
long vfs_seek(file_t *f, int64_t offset, int whence);
long vfs_ftruncate(file_t *f, uint64_t size);

/* Attributes of something that is open, which is `fstat` and has no path at all. */
long vfs_fstat(file_t *f, stat_t *out);

/* --- creating and removing ------------------------------------------------ */

long vfs_mkdir(struct task *t, const char *path, mode_t mode);
long vfs_rmdir(struct task *t, const char *path);
long vfs_unlink(struct task *t, const char *path);
long vfs_rename(struct task *t, const char *from, const char *to);
long vfs_link(struct task *t, const char *from, const char *to);
long vfs_symlink(struct task *t, const char *target, const char *path);
long vfs_readlink(struct task *t, const char *path, char *buf, size_t len);
long vfs_truncate_path(struct task *t, const char *path, uint64_t size);
long vfs_chmod(struct task *t, const char *path, mode_t mode);
long vfs_chown(struct task *t, const char *path, uint64_t uid, uint64_t gid);
long vfs_utimes(struct task *t, const char *path, const uint64_t times[2]);

/* Whether a path exists at all, and what it is.  The one question several callers ask
 * before doing something that would create it. */
long vfs_access(struct task *t, const char *path, bool *exists, bool *is_dir);

/* --- reading a whole file ------------------------------------------------- */

/* The contents of a regular file as one allocation, for `exec` and for the loader.
 * `ramfs_image_free()` gives it back.  0, or -errno. */
long vfs_read_whole(const vpath_t *p, void **out, size_t *bytes_out);

/* --- devfs ---------------------------------------------------------------- */

/* Build /dev.  Called once from kmain(), after the console exists -- /dev/console is the
 * console, and a device node pointing at nothing would be a name that lies. */
int  devfs_init(void);

/* --- procfs --------------------------------------------------------------- */

/* Build /proc.  Every file in it is generated when it is read, so there is one copy of
 * the numbers in the machine and it is the kernel's own. */
int  procfs_init(void);

/* Read a procfs file into `buf`.  `len` is the room there is; the caller sizes it from a
 * first call with len 0, which reports the size without writing. */
long procfs_read(const vpath_t *p, uint64_t proc_id, void *buf, size_t len, size_t *used);
size_t procfs_size(const vpath_t *p, uint64_t proc_id);

/* --- the process's own view of the tree ----------------------------------- */

/* Per-task: the current directory, the root it may not escape, and the process group and
 * session that job control needs.  In task.h because task_t embeds them; these are the
 * operations. */
long vfs_chdir(struct task *t, const char *path);
const char *vfs_cwd(struct task *t, char *buf, size_t len);
long vfs_getcwd(struct task *t, char *buf, size_t len);

#endif /* MINX_VFS_H */