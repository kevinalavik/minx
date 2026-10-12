/* minx - ramfs: a filesystem in memory.
 *
 * The first filesystem this kernel has, and the one the initrd is unpacked into.  It is
 * not a toy: it holds directories, files, symlinks, hard links, permissions and timestamps,
 * and it is writable throughout, because a filesystem that cannot be written to is an
 * initramfs and not a ramfs, and the difference is exactly the part that phase 9's shell
 * discovers when it tries to build a pipe or redirect into /tmp.
 *
 * Everything lives in one allocation pool and one table of nodes, rather than in a tree of
 * separately allocated inodes.  That is a deliberate trade: a tree is what an on-disk
 * filesystem has to be, because each inode is separately addressable, and this one has no
 * such constraint -- the whole pool is one contiguous run of frames and a node's number is
 * its offset into it.  So adding a file is a bump of a free list, and a path is a walk.
 *
 * What that buys, and what it costs:
 *
 *   - Node numbers are stable for the lifetime of the filesystem, because nothing moves.
 *   - A dentry cache would be caching the answer to a function that is two comparisons,
 *     so there isn't one.  See vfs_resolve() for what replaces it.
 *   - The pool is fixed at boot.  A filesystem that cannot grow past its initial size is
 *     the first thing that breaks under load, so `ramfs_grow()` exists and grows it, and
 *     phase 11's stress test is what will find out whether it is enough.
 */
#ifndef MINX_RAMFS_H
#define MINX_RAMFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"

/* What a node is.  A directory and a file are distinguished everywhere rather than by a
 * flag that only some paths check. */
typedef enum {
    RAMFS_NONE = 0,
    RAMFS_FILE,
    RAMFS_DIR,
    RAMFS_SYMLINK,
    RAMFS_DEVICE,        /* a devfs node: a name bound to something that is not bytes */
} ramfs_kind_t;

/* What a devfs node is bound to.  A device is not bytes, and a filesystem that pretends it
 * is produces a device that reads as zeroes. */
typedef enum {
    RAMFS_DEV_CONSOLE = 0,   /* the framebuffer and its serial mirror */
    RAMFS_DEV_NULL,          /* reads nothing, writes nothing, always succeeds */
    RAMFS_DEV_ZERO,          /* reads zeroes forever */
    RAMFS_DEV_RANDOM,        /* reads random bytes, and blocks once it has any history */
    RAMFS_DEV_URANDOM,       /* reads random bytes, and never blocks */
} ramfs_dev_t;

/* One name for a node: which directory it is in, and what it is called there.
 *
 * A node holds several of these because a hard link is not a second node that happens to
 * share a buffer -- it is one node reached by two names.  Modelling it any other way
 * breaks as soon as the file is written through one of them: a write past the end of the
 * buffer reallocates it, and the copy that is not the one being written keeps the old
 * pointer and the old length, so the two names describe different files with the same
 * inode number.  That failure is silent, and it is the reason `nlink` counts names rather
 * than being decorative. */
#define RAMFS_MAX_LINKS 4

typedef struct ramfs_link {
    uint64_t  parent;
    char      name[64];
    bool      used;
} ramfs_link_t;

/* One node.  `links[0]` is the name it was created with; the rest are hard links to it. */
typedef struct ramfs_node {
    uint64_t       ino;           /* stable; an index into the pool */
    ramfs_kind_t   kind;
    ramfs_link_t   links[RAMFS_MAX_LINKS];
    uint64_t       nlink;         /* names pointing here, including unlinked ones */

    uint64_t      *data;          /* page-aligned contents, RAMFS_FILE only */
    uint64_t       size;
    uint64_t       capacity;      /* frames allocated for `data` */
    uint64_t       read_pos;      /* shared cursor for sequential readers */

    char          *link_target;   /* RAMFS_SYMLINK: the path it names, NUL-terminated */
    uint64_t       link_len;

    ramfs_dev_t    device;        /* RAMFS_DEVICE: what it is bound to */

    mode_t         mode;
    uint64_t       uid;
    uint64_t       gid;
    uint64_t       mtime;
    uint64_t       atime;
    uint64_t       ctime;

    bool           unlinked;      /* removed, still open: the names are gone, the data is not */
    uint64_t       next_free;     /* free-list link while unlinked */
} ramfs_node_t;

/* Mount the ramfs at `/` and unpack the initrd into it.
 *
 * The initrd is a cpio "newc" archive, the format mkinitrd.py writes and the format every
 * initramfs builder emits.  Returns 0, or -1 with the reason on the console: a machine that
 * boots with an empty root and no explanation is worse than one that stops and says so. */
long ramfs_mount_initrd(uint64_t initrd_base, uint64_t initrd_size);

/* The root node's number, for the handful of places that start from `/`. */
uint64_t ramfs_root(void);

/* Whether there is a root at all.  Anything that can run before the mount -- the console,
 * the tests -- has to be able to ask without having to try a lookup and guess from zero. */
bool ramfs_mounted(void);

/* A node's current length in bytes.  Zero for a node that is not a file, which is also zero
 * for an empty file; `ramfs_node_kind()` is what tells those two apart. */
uint64_t ramfs_node_size(uint64_t ino);
ramfs_kind_t ramfs_node_kind(uint64_t ino);
const char *ramfs_node_name(uint64_t ino);

/* Look up one path component's child of `parent`.  0 if there is none. */
uint64_t ramfs_lookup(uint64_t parent, const char *name);

/* Create a node under `parent`.  `kind` must not be RAMFS_SYMLINK, which takes a target
 * instead; use ramfs_symlink().  0 on failure. */
uint64_t ramfs_create(uint64_t parent, const char *name, ramfs_kind_t kind, mode_t mode);

/* A symbolic link whose contents are `target`, which is copied.  0 on failure. */
uint64_t ramfs_symlink(uint64_t parent, const char *name, const char *target);

/* Remove a name from a directory.
 *
 * The data goes away only when the last link does, so a program that has a file open while
 * somebody unlinks it keeps reading it -- which is what `unlink` means, and the alternative
 * is a filesystem where opening a file and then renaming it loses the file. */
long ramfs_unlink(uint64_t parent, const char *name);

/* Remove an empty directory.  Separate from `ramfs_unlink()` because POSIX separates them,
 * and for a good reason: `unlink()` on a directory has to fail, or `rm -rf` stops being
 * able to tell a typo from a directory.  A directory that is not empty is `-ENOTEMPTY`,
 * never removed -- a filesystem that empties a directory on request is one that loses
 * files the caller believed it was naming. */
long ramfs_rmdir(uint64_t parent, const char *name);

/* Give a name to an existing node, which is what a hard link is: `nlink` goes up and both
 * names reach the same data.  A directory cannot be hard-linked, which keeps the tree a
 * tree. */
long ramfs_link(uint64_t parent, const char *name, uint64_t target);

/* Rename within one filesystem.  Either both names resolve or neither does changed. */
long ramfs_rename(uint64_t old_parent, const char *old_name,
                  uint64_t new_parent, const char *new_name);

/* Read and write a file's contents.  Both work on the node number, not on a name, so an
 * open file keeps working after it is renamed or unlinked. */
long ramfs_read(uint64_t ino, uint64_t offset, void *buf, size_t bytes);
long ramfs_write(uint64_t ino, uint64_t offset, const void *buf, size_t bytes);

/* Truncate to `size`, growing with zeroes. */
long ramfs_truncate(uint64_t ino, uint64_t size);

/* Per-node times, and the node's own attributes. */
void    ramfs_touch(uint64_t ino, bool atime_only);
long    ramfs_set_mode(uint64_t ino, mode_t mode);

/* The rest of a node's attributes, for a VFS layer that has to answer `stat` without
 * reaching into `ramfs_node_at()` and depending on this file's private layout.
 *
 * A VFS that reads `ramfs_node_at(i)` and pulls fields out of the struct is one edit away
 * from breaking every time this file changes its node, and it makes the two files the same
 * file.  These are the fields, asked for by name. */
uint64_t ramfs_node_parent(uint64_t ino);
mode_t   ramfs_node_mode(uint64_t ino);
uint64_t ramfs_node_nlink(uint64_t ino);
uint64_t ramfs_node_uid(uint64_t ino);
uint64_t ramfs_node_gid(uint64_t ino);
uint64_t ramfs_node_atime(uint64_t ino);
uint64_t ramfs_node_mtime(uint64_t ino);
uint64_t ramfs_node_ctime(uint64_t ino);
ramfs_dev_t ramfs_node_device(uint64_t ino);

/* What a symbolic link names, or NULL for anything that is not one.  The returned string
 * is the node's own, and is NUL-terminated: `link_len` includes the terminator, and a
 * `readlink()` that reported `link_len` would hand a caller one byte more than the name. */
const char *ramfs_node_link(uint64_t ino);

/* The shared sequential cursor.  One per node, so `read()` through two descriptors on the
 * same file advances both -- which is what a program opening the same file twice expects,
 * and what makes `getdents` resumable. */
uint64_t ramfs_node_cursor(uint64_t ino);
void     ramfs_node_set_cursor(uint64_t ino, uint64_t value);

/* Create a device node.  Separate from ramfs_create() because a device is not a file and
 * making it one -- a RAMFS_FILE with a mode -- produces a device that reads as zeroes. */
uint64_t ramfs_create_dev(uint64_t parent, const char *name, ramfs_dev_t dev);

long ramfs_chown(uint64_t ino, uint64_t uid, uint64_t gid);
long ramfs_set_times(uint64_t ino, uint64_t atime, uint64_t mtime);

/* How many nodes exist, how many bytes of data they hold, and how much of the pool is
 * used.  For the self tests and for phase 11's accounting. */
unsigned ramfs_node_count(void);
uint64_t ramfs_data_bytes(void);
uint64_t ramfs_pool_used(void);

/* Every node, for a test that wants to walk the tree rather than look things up by name. */
unsigned ramfs_node_total(void);
const ramfs_node_t *ramfs_node_at(unsigned index);

/* Load an ELF image's bytes out of the filesystem and hand back the whole thing, for the
 * loader.  The caller frees with ramfs_image_free(). */
long ramfs_read_whole(uint64_t ino, void **out, size_t *bytes_out);
void  ramfs_image_free(void *image);

#endif /* MINX_RAMFS_H */