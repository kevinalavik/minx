/* minx - devfs.
 *
 * A device is not bytes, and a filesystem that pretends it is produces a device that reads
 * as zeroes.  So a node here is a *name bound to a thing*: `/dev/null` is the name, and
 * `RAMFS_DEV_NULL` is the thing, and the two are separate so that a caller can be told
 * which it has.
 *
 * Everything else about this file is ordinary ramfs: the nodes are ramfs nodes of kind
 * `RAMFS_DEVICE`, in a directory that happens to be a mount point, and every path in it
 * resolves through the same code as every other path.  What is here is the list of what the
 * names mean.
 *
 * Two of the seven are not devices at all and say so:
 *
 *   - `/dev/console` is the framebuffer and its serial mirror, so a program's output is
 *     both on screen and in the log.  This is what `init` writes to.
 *   - `/dev/tty`, `/dev/tty0` and `/dev/ttyS0` are three names for the same thing, because
 *     a program that opens `/dev/tty` and one that opens `/dev/ttyS0` want the same
 *     terminal and should not have to care which one the filesystem gave it.
 */
#ifndef MINX_DEVFS_H
#define MINX_DEVFS_H

#include <stdbool.h>

/* Build /dev.  Called once, after the console exists: /dev/console is the console, and a
 * device node pointing at nothing would be a name that lies. */
int devfs_init(void);

/* Whether /dev has been built.  `/init` mounts nothing if it has been already. */
bool devfs_ready(void);

#endif /* MINX_DEVFS_H */