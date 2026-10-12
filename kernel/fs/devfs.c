/* minx - devfs.  See devfs.h. */
#include "devfs.h"

#include "console.h"
#include "kprintf.h"
#include "ramfs.h"
#include "vfs.h"

static bool ready;

bool devfs_ready(void) {
    return ready;
}

/* Every node, in one table, because the list is the whole file and a table is what a list
 * of five things should be when the next one arrives.
 *
 * The random devices are the reason there is a generator at all rather than four more
 * entries in here.  `/dev/random` and `/dev/urandom` have to produce different bytes on
 * every read or they are not random, and a program that seeds from them and gets the same
 * answer twice has a key that is not one.  The generator lives in vfs.c because a device's
 * bytes are produced on the read path, and it is an xorshift64\* over a seed taken from the
 * timebase and the processor that is asking -- which is *not* cryptographically strong, and
 * nothing here claims to be.  docs/DECISIONS.md says so by name rather than letting it be
 * quietly relied on for something it is not.
 *
 * `/dev/full` is /dev/null under a different name because that is what it is for the
 * things this kernel can express: a write that must succeed and keep nothing.  A full
 * device has to fail writes with ENOSPC, which arrives with a writable filesystem; until
 * then the name exists and points at the sink, and says so where it is read. */
typedef struct {
    const char  *name;
    ramfs_dev_t  dev;
    uint32_t     mode;
} devnode_t;

static const devnode_t nodes[] = {
    { "console",  RAMFS_DEV_CONSOLE, 0666 },
    { "tty",      RAMFS_DEV_CONSOLE, 0666 },
    { "tty0",     RAMFS_DEV_CONSOLE, 0666 },
    { "ttyS0",    RAMFS_DEV_CONSOLE, 0666 },
    { "null",     RAMFS_DEV_NULL,    0666 },
    { "zero",     RAMFS_DEV_ZERO,    0666 },
    { "full",     RAMFS_DEV_NULL,    0666 },
    { "random",   RAMFS_DEV_RANDOM,  0444 },
    { "urandom",  RAMFS_DEV_URANDOM, 0444 },
};

int devfs_init(void) {
    if (ready)
        return 0;

    /* The directory first.  The initrd creates `/dev` as an empty directory, and this
     * fills it rather than replacing it: the directory is what makes `/dev` a mount point
     * and what a `ls /dev` walks. */
    uint64_t dev = ramfs_lookup(ramfs_root(), "dev");

    if (dev == 0 || ramfs_node_kind(dev) != RAMFS_DIR) {
        dev = ramfs_create(ramfs_root(), "dev", RAMFS_DIR, S_IFDIR | 0755);

        if (dev == 0)
            return -1;
    }

    for (unsigned i = 0; i < array_len(nodes); i++) {
        /* Already there is not an error: the initrd may have shipped a node, and
         * overwriting one would be a way of losing whatever it was bound to. */
        if (ramfs_lookup(dev, nodes[i].name) != 0)
            continue;

        if (ramfs_create_dev(dev, nodes[i].name, nodes[i].dev) == 0)
            return -1;
    }

    if (vfs_mount("devfs", "/dev", FS_DEVFS) != 0 && vfs_mount_count() > 1) {
        /* Already mounted is fine; anything else means the directory is not usable and the
         * nodes below it would be unreachable, which is worth saying out loud. */
        for (unsigned i = 0; i < vfs_mount_count(); i++) {
            if (vfs_mount_kind(i) == FS_DEVFS)
                goto mounted;
        }
        return -1;
    }

mounted:
    ready = true;
    kprintf("devfs: %u device(s) under /dev\n", (unsigned)array_len(nodes));
    return 0;
}