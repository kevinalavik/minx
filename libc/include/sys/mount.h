/* minx - <sys/mount.h>
 *
 * Mounting a filesystem, and the two calls that take a machine down.
 *
 * All three are here rather than in <unistd.h> because POSIX puts them in <sys/mount.h> and
 * because a program that mounts something is a program with a particular kind of authority:
 * it is either init or a user who has been trusted with one.  Putting the declarations next
 * to the ordinary file calls would make the second kind look like the first.
 *
 * This kernel's `mount()` takes three arguments and ignores the other two.  `flags` and
 * `data` exist because every Unix has them and a program written for another one passes
 * them; they are accepted and have no effect rather than being refused, because refusing
 * them would make every such program fail for a reason it cannot see.
 */
#ifndef MINX_SYS_MOUNT_H
#define MINX_SYS_MOUNT_H

/* `flags`.  Defined so that a program which passes `0` has something to pass and a program
 * which passes one of these gets a defined value rather than whatever it guessed.  None of
 * them changes anything here: there is one filesystem per mount point and no remounting,
 * because there is no disk under any of them. */
#define MS_RDONLY  1       /* open the filesystem read-only         */
#define MS_NOSUID  2       /* ignore set-user-id and set-group-id   */
#define MS_NODEV   4       /* do not interpret device files         */
#define MS_NOEXEC  8       /* do not allow programs to be executed  */

/* What `reboot()` takes.
 *
 * `RB_AUTOBOOT` is the only value that reboots.  Every other value -- including the ones
 * that mean "halt" and "power off" on a system that can tell them apart -- stops the
 * machine.  A program that wanted a halt gets one, and a program that wanted a reboot and
 * spelled it wrong gets a halt, which is the safer of the two mistakes. */
#define RB_AUTOBOOT 0x01234567ul
#define RB_HALT      0x4321fedcul
#define RB_POWER_OFF 0xd4321ebcul

/* Mount `type` at `target`.  Zero on success, -1 with `errno` set on failure; the usual
 * `errno` being `EBUSY` when something is already mounted there, which is a normal answer
 * and not a failure to report. */
int mount(const char *type, const char *target, const char *fstype,
          unsigned long flags, const void *data);

/* Take `target` down.  A `umount()` of something that is not mounted is `EINVAL`, and a
 * `umount()` of the root filesystem is `EBUSY` forever -- which is a true answer, and a
 * program that asks for it is asking for a machine with no root. */
int umount(const char *target);

/* Flush everything to its backing store.
 *
 * Everything this kernel writes is already in memory -- there is no page cache behind a
 * file and no disk under it -- so this has nothing to do and succeeds.  It is here because
 * `init` calls it before stopping the machine, and because a program that does the same
 * must not have to know that there is nothing to flush. */
int sync(void);

/* Stop or restart the machine.
 *
 * Root only, and there is only root.  `RB_AUTOBOOT` reboots; anything else powers off. */
int reboot(int how);

#endif /* MINX_SYS_MOUNT_H */
