/* minx - ramfs.  See ramfs.h for what this is and why the shape is what it is. */
#include "ramfs.h"

#include <string.h>

#include "boot.h"
#include "kprintf.h"
#include "kmalloc.h"
#include "pmm.h"
#include "string.h"
#include "syscall.h"

/* --- the pool ------------------------------------------------------------- */

/* Nodes are a fixed-size array, so a node number is an index and nothing moves.  The size
 * is the first thing phase 11 will be wrong about: it is a bound chosen so that the whole
 * table is a handful of frames, and `ramfs_grow()` exists so that being wrong is a refill
 * rather than a failure. */
#define RAMFS_NODES 2048

static ramfs_node_t nodes[RAMFS_NODES];
static uint64_t     free_head;         /* ino + 1, or 0 for "none" */
static unsigned      node_used;
static uint64_t      data_bytes;
static uint64_t      pool_used_bytes;
static bool          mounted;

/* cpio "newc": 6 bytes of magic, then 13 fields of 8 hex characters each.  Parsed with a
 * hand-rolled hex reader rather than `sscanf`, because this runs before there is a heap it
 * can trust and because a field-by-field read is the only way to say *which* field was
 * nonsense. */
#define NEWC_MAGIC   "070701"
#define NEWC_HEADER  110


/* --- nodes ---------------------------------------------------------------- */

static uint64_t node_alloc(void) {
    uint64_t ino = free_head;

    if (ino == 0)
        return 0;

    free_head = nodes[ino - 1].next_free;
    memset(&nodes[ino - 1], 0, sizeof(nodes[0]));
    nodes[ino - 1].ino = ino;
    node_used++;
    return ino;
}

static void node_free(uint64_t ino) {
    if (ino == 0 || ino > RAMFS_NODES)
        return;

    /* Anything still holding data gives it back now: a node nobody has open is the only
     * kind that can be reclaimed at all, and `ramfs_grow` is the answer for the rest. */
    if (nodes[ino - 1].data != NULL) {
        pool_used_bytes -= nodes[ino - 1].capacity;
        kfree(nodes[ino - 1].data);
        nodes[ino - 1].data = NULL;
    }
    if (nodes[ino - 1].link_target != NULL) {
        kfree(nodes[ino - 1].link_target);
        nodes[ino - 1].link_target = NULL;
    }

    nodes[ino - 1].kind = RAMFS_NONE;
    nodes[ino - 1].next_free = free_head;
    free_head = ino;
    node_used--;
}

/* Grow a file's buffer in whole pages, never shrinking it -- a truncate that reallocates
 * smaller would have to move the data, and moving it changes the offset every open file is
 * holding. */
static bool node_reserve(uint64_t ino, uint64_t want) {
    ramfs_node_t *n = &nodes[ino - 1];

    if (want <= n->capacity)
        return true;

    uint64_t frames = (want + page_size - 1) / page_size;

    if (frames > 4096)                  /* 16 MiB in one file; see ramfs.h on growth */
        return false;

    uint64_t *fresh = kmalloc(frames * page_size);

    if (fresh == NULL)
        return false;

    if (n->data != NULL) {
        memcpy(fresh, n->data, n->capacity);
        pool_used_bytes -= n->capacity;
        kfree(n->data);
    } else {
        memset(fresh, 0, frames * page_size);
    }

    n->data = fresh;
    n->capacity = frames * page_size;
    pool_used_bytes += n->capacity;
    return true;
}

/* --- the public interface ------------------------------------------------- */

uint64_t ramfs_root(void) {
    return mounted ? 1 : 0;
}

bool ramfs_mounted(void) {
    return mounted;
}

uint64_t ramfs_node_size(uint64_t ino) {
    return (!mounted || ino == 0 || ino > RAMFS_NODES) ? 0 : nodes[ino - 1].size;
}

ramfs_kind_t ramfs_node_kind(uint64_t ino) {
    return (!mounted || ino == 0 || ino > RAMFS_NODES) ? RAMFS_NONE
                                                        : nodes[ino - 1].kind;
}

const char *ramfs_node_name(uint64_t ino) {
    return (!mounted || ino == 0 || ino > RAMFS_NODES) ? ""
                                                         : nodes[ino - 1].links[0].name;
}

uint64_t ramfs_lookup(uint64_t parent, const char *name) {
    if (!mounted || parent == 0 || parent > RAMFS_NODES || name == NULL)
        return 0;

    if (nodes[parent - 1].kind != RAMFS_DIR)
        return 0;

    /* Every name slot of every node, because a hard link puts the second name on the same
     * node as the first and a scan that only compared `parent` would miss it.  Scanning the
     * slots rather than keeping a per-directory child list costs a comparison per live
     * node; at this size that is cheaper than maintaining a second structure that has to
     * stay consistent with this one, which is the thing that actually goes wrong. */
    for (unsigned i = 0; i < RAMFS_NODES; i++) {
        if (nodes[i].kind == RAMFS_NONE)
            continue;

        for (unsigned l = 0; l < RAMFS_MAX_LINKS; l++) {
            if (nodes[i].links[l].used && nodes[i].links[l].parent == parent &&
                strcmp(nodes[i].links[l].name, name) == 0)
                return nodes[i].ino;
        }
    }

    return 0;
}

uint64_t ramfs_create(uint64_t parent, const char *name, ramfs_kind_t kind,
                      mode_t mode) {
    if (!mounted || parent == 0 || parent > RAMFS_NODES || name == NULL || name[0] == 0)
        return 0;

    /* A name is one component and no longer, and enforcing that here means every path
     * walker downstream can assume it: `mkdir /a/b/c` cannot create a directory called
     * "b/c", and `open("/etc/../root")` cannot escape through a name that was never split. */
    if (strlen(name) >= sizeof(nodes[0].links[0].name) || strchr(name, '/') != NULL) {
        kprintf("ramfs: refusing the name \"%s\"\n", name);
        return 0;
    }

    if (nodes[parent - 1].kind != RAMFS_DIR) {
        kprintf("ramfs: cannot create \"%s\" in %lu, which is not a directory\n",
                name, parent);
        return 0;
    }

    if (ramfs_lookup(parent, name) != 0) {
        kprintf("ramfs: \"%s\" already exists\n", name);
        return 0;
    }

    uint64_t ino = node_alloc();

    if (ino == 0) {
        kprintf("ramfs: no node left for \"%s\"\n", name);
        return 0;
    }

    ramfs_node_t *n = &nodes[ino - 1];

    n->kind = kind;
    n->links[0].parent = parent;
    strlcpy(n->links[0].name, name, sizeof(n->links[0].name));
    n->links[0].used = true;
    n->nlink = 1;
    n->mode = mode;
    n->uid = 0;
    n->gid = 0;
    return ino;
}

uint64_t ramfs_symlink(uint64_t parent, const char *name, const char *target) {
    uint64_t ino = ramfs_create(parent, name, RAMFS_SYMLINK, 0777);

    if (ino == 0)
        return 0;

    size_t len = strlen(target) + 1;

    nodes[ino - 1].link_target = kmalloc(len);

    if (nodes[ino - 1].link_target == NULL) {
        node_free(ino);
        return 0;
    }

    memcpy(nodes[ino - 1].link_target, target, len);
    nodes[ino - 1].link_len = len;
    return ino;
}

/* Take one name off a node, and free the node if that was the last one.
 *
 * Shared by `unlink()` and `rmdir()` because the removal is the same operation and the
 * difference between them is only the checks in front of it.  Going the other way -- having
 * rmdir() set the link count and then call unlink() -- makes rmdir fail with EISDIR on
 * every directory, because unlink() is right to refuse one and is asked to remove one. */
static long detach_name(uint64_t ino, uint64_t parent, const char *name) {
    ramfs_node_t *n = &nodes[ino - 1];

    for (unsigned l = 0; l < RAMFS_MAX_LINKS; l++) {
        if (n->links[l].used && n->links[l].parent == parent &&
            strcmp(n->links[l].name, name) == 0) {
            n->links[l].used = false;
            n->links[l].parent = 0;
            break;
        }
    }

    if (--n->nlink == 0) {
        n->unlinked = true;
        data_bytes -= n->size;

        /* The last name is gone, so the node goes with it.  "Unlinked but still open" -- a
         * file whose name is removed while a process holds it, which must keep reading
         * until it closes -- is the VFS layer's job and not this one's: that layer holds
         * the reference, and until it exists there is nothing to hold one, so holding the
         * node here would leak every file that is ever removed. */
        node_free(ino);
    }

    return 0;
}

long ramfs_unlink(uint64_t parent, const char *name) {
    uint64_t ino = ramfs_lookup(parent, name);

    if (ino == 0)
        return -ENOENT;

    if (nodes[ino - 1].kind == RAMFS_DIR)
        return -EISDIR;

    /* The name goes; the data stays until the last name does.  Clearing one slot rather
     * than the whole node is what makes `rm` on one of two links safe -- and what makes the
     * node number survive for the names that are left. */
    return detach_name(ino, parent, name);
}

long ramfs_rmdir(uint64_t parent, const char *name) {
    uint64_t ino = ramfs_lookup(parent, name);

    if (ino == 0)
        return -ENOENT;

    if (nodes[ino - 1].kind != RAMFS_DIR)
        return -ENOTDIR;

    /* The root is not a child of anything, so it is never reached here, but the check is
     * cheap and the consequence of getting it wrong is unmounting the machine's root. */
    if (ino == ramfs_root())
        return -EBUSY;

    /* Empty means "nothing is in it", which is a question about other nodes' names, not
     * about this node's.  Every node has a name of its own -- links[0] is the one it was
     * created with -- so checking this node's own slots asks whether the directory is
     * itself named, which is always true and makes rmdir fail on every directory. */
    for (unsigned i = 0; i < RAMFS_NODES; i++) {
        if (nodes[i].kind == RAMFS_NONE)
            continue;

        for (unsigned l = 0; l < RAMFS_MAX_LINKS; l++) {
            if (nodes[i].links[l].used && nodes[i].links[l].parent == ino) {
                /* Named, because "not empty" with nothing in the message is the least
                 * useful error this function could return: the caller knows the name it
                 * asked to remove and has no way to find out what is in the way. */
                kprintf("ramfs: %llu is not empty, it still holds \"%s\" "
                        "(node %llu)\n", (unsigned long long)ino,
                        nodes[i].links[l].name, (unsigned long long)nodes[i].ino);
                return -ENOTEMPTY;
            }
        }
    }

    nodes[ino - 1].nlink = 1;       /* so detach_name() frees it */
    return detach_name(ino, parent, name);
}

long ramfs_link(uint64_t parent, const char *name, uint64_t target) {
    if (!mounted || target == 0 || target > RAMFS_NODES)
        return -ENOENT;

    if (nodes[target - 1].kind == RAMFS_DIR)
        return -EPERM;           /* a hard-linked directory is a cycle by another name */

    if (ramfs_lookup(parent, name) != 0)
        return -EEXIST;

    if (strlen(name) >= sizeof(nodes[0].links[0].name) || strchr(name, '/') != NULL)
        return -ENAMETOOLONG;

    ramfs_node_t *orig = &nodes[target - 1];

    for (unsigned l = 0; l < RAMFS_MAX_LINKS; l++) {
        if (orig->links[l].used)
            continue;

        /* The same node, the same buffer, one more name.  Not a copy of the node: a copy
         * would have its own `data` pointer, so the first write past the end of the buffer
         * reallocates it for the copy being written and leaves the original pointing at
         * freed memory with a stale length. */
        orig->links[l].parent = parent;
        strlcpy(orig->links[l].name, name, sizeof(orig->links[l].name));
        orig->links[l].used = true;
        orig->nlink++;
        return 0;
    }

    kprintf("ramfs: %s already has %u names, which is the limit\n",
            orig->links[0].name, RAMFS_MAX_LINKS);
    return -EMLINK;
}

long ramfs_rename(uint64_t old_parent, const char *old_name,
                  uint64_t new_parent, const char *new_name) {
    uint64_t ino = ramfs_lookup(old_parent, old_name);

    if (ino == 0)
        return -ENOENT;

    if (strlen(new_name) >= sizeof(nodes[0].links[0].name) ||
        strchr(new_name, '/') != NULL)
        return -ENAMETOOLONG;

    if (old_parent == new_parent && strcmp(old_name, new_name) == 0)
        return 0;

    uint64_t existing = ramfs_lookup(new_parent, new_name);

    if (existing != 0) {
        if (nodes[existing - 1].kind == RAMFS_DIR && nodes[ino - 1].kind != RAMFS_DIR)
            return -EISDIR;
        if (nodes[existing - 1].kind != RAMFS_DIR && nodes[ino - 1].kind == RAMFS_DIR)
            return -ENOTDIR;
        (void)ramfs_unlink(new_parent, new_name);
    }

    /* Move the one name, and leave every other name of this node exactly where it was: a
     * file that is also hard-linked somewhere else keeps that other name. */
    ramfs_node_t *n = &nodes[ino - 1];

    for (unsigned l = 0; l < RAMFS_MAX_LINKS; l++) {
        if (n->links[l].used && n->links[l].parent == old_parent &&
            strcmp(n->links[l].name, old_name) == 0) {
            strlcpy(n->links[l].name, new_name, sizeof(n->links[l].name));
            n->links[l].parent = new_parent;
            return 0;
        }
    }

    return -ENOENT;
}

long ramfs_read(uint64_t ino, uint64_t offset, void *buf, size_t bytes) {
    if (!mounted || ino == 0 || ino > RAMFS_NODES || buf == NULL)
        return -EFAULT;

    ramfs_node_t *n = &nodes[ino - 1];

    if (n->kind == RAMFS_DEVICE)
        return -EINVAL;              /* a device is not bytes; the caller must know which */

    if (n->kind == RAMFS_SYMLINK) {
        /* A symlink's stored contents include the terminator that `ramfs_symlink()`
         * allocated room for; a read of the link is a `readlink()`, and a `readlink()`
         * reports the length of the name and not the length of the buffer it is in.
         * Returning 9 for "greeting" would make every caller that compares against
         * strlen() disagree, and the usual thing it does about that is allocate one more
         * byte for a byte that is not part of the name. */
        size_t want = (n->link_len > 0 ? n->link_len - 1 : 0);

        if (want > bytes)
            want = bytes;

        memcpy(buf, n->link_target, want);
        return (long)want;
    }

    if (n->kind != RAMFS_FILE)
        return -EISDIR;

    if (offset >= n->size)
        return 0;

    size_t want = n->size - offset;

    if (want > bytes)
        want = bytes;

    if (n->data == NULL)
        return 0;

    memcpy(buf, (const uint8_t *)n->data + offset, want);
    ramfs_touch(ino, true);
    return (long)want;
}

long ramfs_write(uint64_t ino, uint64_t offset, const void *buf, size_t bytes) {
    if (!mounted || ino == 0 || ino > RAMFS_NODES || buf == NULL)
        return -EFAULT;

    ramfs_node_t *n = &nodes[ino - 1];

    if (n->kind != RAMFS_FILE)
        return -EINVAL;

    if (offset + bytes > n->size && !node_reserve(ino, offset + bytes))
        return -ENOSPC;

    if (n->data != NULL && bytes > 0)
        memcpy((uint8_t *)n->data + offset, buf, bytes);

    if (offset + bytes > n->size) {
        data_bytes += (offset + bytes) - n->size;
        n->size = offset + bytes;
    }

    ramfs_touch(ino, false);
    return (long)bytes;
}

long ramfs_truncate(uint64_t ino, uint64_t size) {
    if (!mounted || ino == 0 || ino > RAMFS_NODES)
        return -EFAULT;

    ramfs_node_t *n = &nodes[ino - 1];

    if (n->kind != RAMFS_FILE)
        return -EINVAL;

    if (size > n->size && !node_reserve(ino, size))
        return -ENOSPC;

    if (size > n->size && n->data != NULL)
        memset((uint8_t *)n->data + n->size, 0, size - n->size);

    if (size < n->size)
        data_bytes -= n->size - size;

    n->size = size;
    ramfs_touch(ino, false);
    return 0;
}

void ramfs_touch(uint64_t ino, bool atime_only) {
    if (!mounted || ino == 0 || ino > RAMFS_NODES)
        return;

    ramfs_node_t *n = &nodes[ino - 1];

    n->atime = n->mtime;
    if (!atime_only)
        n->ctime = n->mtime;
}

long ramfs_set_mode(uint64_t ino, mode_t mode) {
    if (!mounted || ino == 0 || ino > RAMFS_NODES)
        return -EFAULT;

    nodes[ino - 1].mode = mode;
    ramfs_touch(ino, false);
    return 0;
}

/* --- attributes, asked for by name --------------------------------------- */

uint64_t ramfs_node_parent(uint64_t ino) {
    return (!mounted || ino == 0 || ino > RAMFS_NODES) ? 0 : nodes[ino - 1].links[0].parent;
}

mode_t ramfs_node_mode(uint64_t ino) {
    return (!mounted || ino == 0 || ino > RAMFS_NODES) ? 0 : nodes[ino - 1].mode;
}

uint64_t ramfs_node_nlink(uint64_t ino) {
    return (!mounted || ino == 0 || ino > RAMFS_NODES) ? 0 : nodes[ino - 1].nlink;
}

uint64_t ramfs_node_uid(uint64_t ino) {
    return (!mounted || ino == 0 || ino > RAMFS_NODES) ? 0 : nodes[ino - 1].uid;
}

uint64_t ramfs_node_gid(uint64_t ino) {
    return (!mounted || ino == 0 || ino > RAMFS_NODES) ? 0 : nodes[ino - 1].gid;
}

uint64_t ramfs_node_atime(uint64_t ino) {
    return (!mounted || ino == 0 || ino > RAMFS_NODES) ? 0 : nodes[ino - 1].atime;
}

uint64_t ramfs_node_mtime(uint64_t ino) {
    return (!mounted || ino == 0 || ino > RAMFS_NODES) ? 0 : nodes[ino - 1].mtime;
}

uint64_t ramfs_node_ctime(uint64_t ino) {
    return (!mounted || ino == 0 || ino > RAMFS_NODES) ? 0 : nodes[ino - 1].ctime;
}

ramfs_dev_t ramfs_node_device(uint64_t ino) {
    if (!mounted || ino == 0 || ino > RAMFS_NODES)
        return RAMFS_DEV_NULL;
    return nodes[ino - 1].device;
}

const char *ramfs_node_link(uint64_t ino) {
    if (!mounted || ino == 0 || ino > RAMFS_NODES)
        return NULL;
    if (nodes[ino - 1].kind != RAMFS_SYMLINK)
        return NULL;
    return nodes[ino - 1].link_target;
}

uint64_t ramfs_node_cursor(uint64_t ino) {
    return (!mounted || ino == 0 || ino > RAMFS_NODES) ? 0 : nodes[ino - 1].read_pos;
}

void ramfs_node_set_cursor(uint64_t ino, uint64_t value) {
    if (mounted && ino != 0 && ino <= RAMFS_NODES)
        nodes[ino - 1].read_pos = value;
}

uint64_t ramfs_create_dev(uint64_t parent, const char *name, ramfs_dev_t dev) {
    uint64_t ino = ramfs_create(parent, name, RAMFS_DEVICE, S_IFCHR | 0666);

    if (ino == 0)
        return 0;

    nodes[ino - 1].device = dev;
    return ino;
}

long ramfs_chown(uint64_t ino, uint64_t uid, uint64_t gid) {
    if (!mounted || ino == 0 || ino > RAMFS_NODES)
        return -EFAULT;

    nodes[ino - 1].uid = uid;
    nodes[ino - 1].gid = gid;
    ramfs_touch(ino, false);
    return 0;
}

long ramfs_set_times(uint64_t ino, uint64_t atime, uint64_t mtime) {
    if (!mounted || ino == 0 || ino > RAMFS_NODES)
        return -EFAULT;

    /* Only the two the interface takes.  The change time is the kernel's to set, because
     * it is the time of the call rather than of anything the caller asked for. */
    nodes[ino - 1].atime = atime;
    nodes[ino - 1].mtime = mtime;
    return 0;
}

unsigned ramfs_node_count(void) { return node_used; }
uint64_t ramfs_data_bytes(void) { return data_bytes; }
uint64_t ramfs_pool_used(void) { return pool_used_bytes; }

unsigned ramfs_node_total(void) { return RAMFS_NODES; }

const ramfs_node_t *ramfs_node_at(unsigned index) {
    return index < RAMFS_NODES ? &nodes[index] : NULL;
}

long ramfs_read_whole(uint64_t ino, void **out, size_t *bytes_out) {
    if (!mounted || ino == 0 || ino > RAMFS_NODES || out == NULL)
        return -ENOENT;

    ramfs_node_t *n = &nodes[ino - 1];

    if (n->kind != RAMFS_FILE)
        return -EISDIR;

    if (n->size == 0) {
        *out = kmalloc(1);
        *bytes_out = 0;
        return *out != NULL ? 0 : -ENOMEM;
    }

    void *image = kmalloc((size_t)n->size);

    if (image == NULL)
        return -ENOMEM;

    memcpy(image, n->data, (size_t)n->size);
    *out = image;
    *bytes_out = (size_t)n->size;
    return 0;
}

void ramfs_image_free(void *image) {
    kfree(image);
}

/* --- unpacking the initrd ----------------------------------------------- */

/* The next field of a newc header, or 0 at the end of the archive. */
static const char *newc_field(const char *p, unsigned which, const char *end) {
    /* The first field starts *after* the six-byte magic, not at it: the header is
     * "070701" followed by thirteen 8-character hex fields, so field zero is at
     * offset 6.  Off by six reads the magic as `c_ino` and every field as the one
     * six bytes ahead of it, which is a value that parses cleanly and means nothing --
     * `c_filesize` lands on `c_devmajor` and `c_namesize` on `c_check`, so every entry
     * reads as a zero-length file with a zero-length name, and the walk falls off the
     * end of the header without ever noticing. */
    static const unsigned offsets[] = {
        6, 14, 22, 30, 38, 46, 54, 62, 70, 78, 86, 94, 102,
    };

    if (which >= sizeof(offsets) / sizeof(offsets[0]))
        return NULL;

    const char *at = p + offsets[which];

    if (at + 8 > end)
        return NULL;

    /* Both hex cases are accepted.  The cpio(1) spec says lowercase, and that is what most
     * writers emit; this project's own `tools/mkinitrd.py` uses Python's `%08X` and so
     * writes uppercase.  A field of `000041ED` is a valid `c_mode` written by either, and
     * rejecting the uppercase one makes the only initrd this kernel ever sees look like a
     * corrupt archive. */
    for (unsigned i = 0; i < 8; i++) {
        char c = at[i];

        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return NULL;
    }

    return at;
}

static uint64_t newc_value(const char *field) {
    uint64_t v = 0;

    if (field == NULL)
        return 0;

    for (unsigned i = 0; i < 8; i++) {
        char c = field[i];
        uint64_t digit = (c >= '0' && c <= '9') ? (uint64_t)(c - '0')
                          : (c >= 'a' && c <= 'f') ? (uint64_t)(c - 'a' + 10)
                                                    : (uint64_t)(c - 'A' + 10);
        v = (v << 4) | digit;
    }

    return v;
}

/* Create every *directory* in `path` and return the one they hang off.
 *
 * Only the components that are followed by a slash, and that is the whole of it: this
 * returns the directory the last component goes *into*, and creating the last component as
 * a directory is how an archive's top-level files turn into empty directories.  `init` at the
 * root of an initrd is the one every boot has, so it is the one that is always broken --
 * the archive says "./init" is a file, `make_dirs()` makes it a directory, the caller sees
 * a path with no slash in it and counts it as a directory it has already made, and the
 * kernel boots to "there is no /init" having unpacked twenty-one entries successfully.
 *
 * The distinction is not cosmetic in the other direction either: a name that *does* end in
 * a slash is a directory, and creating it here is what makes `bin/` exist before `bin/sh`
 * is unpacked into it. */
static uint64_t make_dirs(uint64_t parent, const char *path, size_t len) {
    uint64_t at = parent;
    char name[64];
    size_t start = 0;

    for (size_t i = 0; i < len; i++) {
        if (path[i] != '/')
            continue;

        /* Only the components a slash *terminates*, which is every component except the
         * last.
         *
         * A trailing slash is a directory entry in its own right -- "bin/" is how an
         * archive says "this is a directory", and it is why an initrd can list a directory
         * that has nothing in it -- and a slash in the middle terminates a component that is
         * therefore a directory too.  Either way the component ends here and is created.
         *
         * What must never happen is creating the *last* component when it is the file the
         * caller is about to make, and that is the difference between an initrd that boots
         * and one where every file at every level is an empty directory. */
        size_t n = i - start;

        if (n == 0 || n >= sizeof(name)) {
            start = i + 1;
            continue;
        }

        memcpy(name, path + start, n);
        name[n] = 0;

        uint64_t found = ramfs_lookup(at, name);

        if (found == 0)
            found = ramfs_create(at, name, RAMFS_DIR, 0755);

        if (found == 0)
            return 0;

        at = found;
        start = i + 1;
    }

    return at;
}

long ramfs_mount_initrd(uint64_t initrd_base, uint64_t initrd_size) {
    (void)mounted;

    memset(nodes, 0, sizeof(nodes));
    node_used = 0;
    data_bytes = 0;
    pool_used_bytes = 0;

    /* Every node number starts on the free list.  Handing them out lowest-first keeps a
     * directory listing in a predictable order, which matters more than it sounds: a test
     * that lists a directory and compares it against a list should not depend on
     * allocation order to pass. */
    free_head = 0;
    for (unsigned i = RAMFS_NODES; i > 0; i--) {
        nodes[i - 1].kind = RAMFS_NONE;
        nodes[i - 1].next_free = free_head;
        free_head = i;
    }

    uint64_t root = node_alloc();

    if (root == 0) {
        kprintf("ramfs: no node for the root directory\n");
        return -1;
    }

    /* The root is the one node with no name: it is reached by every absolute path rather
     * than by an entry in a parent directory, so its single name slot stays unused and
     * `nlink` is 2 -- itself and the entry its parent would have. */
    nodes[root - 1].kind = RAMFS_DIR;
    nodes[root - 1].mode = 0755;
    nodes[root - 1].nlink = 2;      /* itself and its parent's entry */
    mounted = true;

    kprintf("ramfs: mounted, %u nodes\n", RAMFS_NODES);

    if (initrd_base == 0 || initrd_size == 0) {
        kprintf("ramfs: no initrd to unpack, so the root is empty\n");
        return 0;
    }

    const char *base = (const char *)(uintptr_t)initrd_base;
    const char *at = base;
    const char *end = base + initrd_size;
    unsigned files = 0;
    unsigned dirs = 0;
    unsigned links = 0;

    while (at + NEWC_HEADER <= end) {
        if (memcmp(at, NEWC_MAGIC, 6) != 0) {
            kprintf("ramfs: initrd is not a cpio archive at %p\n", (void *)at);
            break;
        }

        const char *hdr = at;
        uint64_t mode = newc_value(newc_field(hdr, 1, end));      /* c_mode    */
        uint64_t filesize = newc_value(newc_field(hdr, 6, end));   /* c_filesize*/
        uint64_t namesize = newc_value(newc_field(hdr, 11, end));  /* c_namesize*/
        const char *name_at = hdr + NEWC_HEADER;

        /* Every field the entry is read through has to have parsed before any of them is
         * used.  `newc_value()` of a missing field is 0, and zero is a legal value for all
         * three, so "checked afterwards" cannot tell a zero-length entry from a header that
         * stopped making sense partway through -- and a truncated or corrupt archive is
         * exactly when this code has to stop rather than read the rest of physical memory
         * as if it were names. */
        if (newc_field(hdr, 1, end) == NULL ||
            newc_field(hdr, 6, end) == NULL ||
            newc_field(hdr, 11, end) == NULL) {
            kprintf("ramfs: initrd entry at %p has a malformed header\n",
                    (void *)at);
            break;
        }

        if (namesize == 0 || namesize > 256 || name_at + namesize > end) {
            kprintf("ramfs: initrd entry at %p has a %llu-byte name, which is not "
                    "one\n", (void *)at, (unsigned long long)namesize);
            break;
        }

        const char *name = name_at;
        /* The contents start on the next four-byte boundary *after* the name, not
         * immediately after it.
         *
         * `c_namesize` counts the pathname and its terminator and not the padding, and
         * newc's rule is that "the pathname is followed by NUL bytes so that the total size
         * of the fixed header and pathname is a multiple of four".  So for a seven-byte name
         * the contents begin at 120, not at 117, and taking `name_at + namesize` reads
         * three bytes of padding as the first three bytes of the file.
         *
         * It is invisible for every entry whose name length happens to make the sum a
         * multiple of four, and for a cpio with several such entries in it -- which is how
         * it survived until an initrd contained a file at the top level, whose name length
         * is the one that has to be six characters.  The symptom is an ELF whose class byte
         * reads 69, which is 'E': the loader is looking three bytes before the file and
         * finds the 'E' of its own magic number in the ELF class field. */
        const char *data_at = (const char *)(((uintptr_t)(name_at + namesize) + 3) &
                                             ~(uintptr_t)3);
        const char *data_end = data_at + filesize;

        if (data_end > end) {
            kprintf("ramfs: initrd entry \"%s\" runs past the end\n", name);
            break;
        }

        /* The archive's last entry is named TRAILER!!! and has no contents. */
        if (namesize == 10 && memcmp(name, "TRAILER!!!", 9) == 0)
            break;

        /* The contents are padded to a four-byte boundary, and so is the *header plus the
         * name* -- together, not separately.  newc's wording is that "the pathname is
         * followed by NUL bytes so that the total size of the fixed header and pathname is
         * a multiple of four", and with a 110-byte header and a 3-byte "./" name that
         * lands on 116, not on the 114 that padding the name alone gives.  Getting this
         * wrong is invisible on any entry whose name is a multiple of four minus nothing,
         * and on everything else the walk steps two bytes into the next header and reports
         * a valid archive as corrupt at the second entry. */
        const char *next_entry = hdr + ((NEWC_HEADER + namesize + 3) & ~3ull)
                                      + ((filesize + 3) & ~3ull);

        /* cpio's newc names are relative to the archive root and use a leading "./" for
         * the top level. */
        while (name[0] == '.' && (name[1] == '/' || name[1] == 0))
            name += 1 + (name[1] == '/');

        size_t len = strnlen(name, namesize);

        if (len == 0) {
            at = next_entry;
            continue;
        }

        uint64_t parent = make_dirs(root, name, len);
        const char *base_name = name + (len - 1);

        while (base_name > name && base_name[-1] != '/')
            base_name--;

        if (parent == 0) {
            kprintf("ramfs: could not create the directories for \"%s\"\n", name);
        } else if (len > 0 && name[len - 1] == '/') {
            /* A name that ends in a slash is a directory, and make_dirs() made it.  The
             * test is on the last *character* rather than on whether the name has a slash
             * in it at all: "bin/sh" has one and is a file, and treating every path with a
             * slash in it as a directory loses every file below the root -- which is all of
             * them. */
            dirs++;
        } else if ((mode & 0170000) == 0120000) {
            /* A symlink's contents are the target path, and an archive is free to store
             * them without a terminator.  `ramfs_symlink()` takes a C string, so the
             * archive's bytes are copied into one that is: handing it a pointer into the
             * initrd and letting strlen() run would read whatever follows the entry until
             * it happened to find a zero byte. */
            char target[256];
            size_t tlen = (size_t)filesize < sizeof(target) - 1 ? (size_t)filesize
                                                                 : sizeof(target) - 1;
            memcpy(target, data_at, tlen);
            target[tlen] = 0;

            if (ramfs_symlink(parent, base_name, target) != 0)
                kprintf("ramfs: could not make the link \"%s\"\n", name);
            else
                links++;
        } else if ((mode & 0170000) == 0040000) {
            /* Already made by make_dirs(). */
            dirs++;
        } else {
            uint64_t ino = ramfs_create(parent, base_name, RAMFS_FILE, mode & 07777);

            if (ino == 0) {
                kprintf("ramfs: could not create \"%s\"\n", name);
            } else {
                nodes[ino - 1].mode = mode & 07777;
                if (filesize > 0 &&
                    ramfs_write(ino, 0, data_at, (size_t)filesize) < 0)
                    kprintf("ramfs: could not store the contents of \"%s\"\n", name);
                else
                    files++;
            }
        }

        at = next_entry;
    }

    kprintf("ramfs: unpacked %u file(s), %u directory(ies), %u link(s)\n",
            files, dirs, links);
    return 0;
}