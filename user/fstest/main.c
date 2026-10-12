/* minx - fstest: the filesystem as a program sees it.
 *
 * Everything else that tests the filesystem does it from inside the kernel, calling
 * `vfs_*()` directly.  That tests the VFS and nothing else: it cannot catch a syscall that
 * forgets to validate a pointer, a descriptor that is handed back after the file behind it
 * is gone, or a path that resolves to the right node by the wrong route.  This program goes
 * through `int $0x80` and nothing else, so everything between the two ends is under test.
 *
 * The cases are the ones whose absence is silent.  A file that reads back the right bytes
 * still looks right in a tree nobody can walk, until something tries to unlink it or link
 * it a second time; a rename that moves a file into a directory that does not exist still
 * "renames" something as long as the caller never looks.  So almost every check here has a
 * second half that would fail if the first half had quietly done the wrong thing.
 *
 * Everything it creates is under /tmp, which init makes, and it removes it again: a test
 * that leaves files behind is a test that makes the next test's `getdents` counts wrong.
 *
 * The return is zero only if every check passed.
 */
#include "../common/minx_user.h"
#include "../common/print.h"

/* Scratch under /tmp, named after this program so two runs cannot tread on each other. */
#define DIR       "/tmp/fstest"
#define FILE      DIR "/greeting"
#define COPY      DIR "/copy"
#define LINK      DIR "/hard"
#define SYMLINK   DIR "/soft"
/* Relative, not absolute, and that is the point: a relative target is resolved against the
 * *link's* directory, so this only works if the kernel resolves it that way.  It names
 * `copy` rather than `greeting` because the rename above moved the file, and a symlink to a
 * name that is not there is a dangling link -- which is correct behaviour and would fail
 * every check below for the wrong reason. */
#define TARGET    "copy"

static int fail(const char *what) {
    minx_report("fstest", 0, what);
    return 1;
}

/* The same, for a failure whose interesting part is a number.
 *
 * The number goes *into* the report line rather than after it, because the report line is
 * the one thing the kernel's log is grepped for and a reader who has to match two adjacent
 * lines has half the evidence they need.  The digits are appended by hand rather than with
 * a printf: there is no libc here yet, and growing one to format a test's failure message
 * is backwards. */
static int fail_num(const char *what, unsigned long v) {
    char detail[160];
    char digits[24];
    unsigned dn = 0;
    unsigned n = 0;

    while (*what != 0 && n < sizeof(detail) - sizeof(digits) - 1)
        detail[n++] = *what++;

    if (v == 0) {
        digits[dn++] = '0';
    } else {
        while (v != 0) {
            digits[dn++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }

    while (dn > 0)
        detail[n++] = digits[--dn];

    detail[n] = 0;
    minx_report("fstest", 0, detail);
    return 1;
}

/* Every check is "and the other thing", because a check with only one half passes on a
 * filesystem that invented the answer. */
static int same(const char *a, const char *b, size_t_ n) {
    for (size_t_ i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

int minx_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    /* Start from a clean directory whatever happened last time.  `rm -rf` does not exist
     * yet, so this is the handful of calls it would be.  A failure to remove is not fatal:
     * the interesting case is a directory that was never created, and that is what the
     * EEXIST below reports. */
    (void)minx_unlink(FILE);
    (void)minx_unlink(COPY);
    (void)minx_unlink(LINK);
    (void)minx_unlink(SYMLINK);
    (void)minx_rmdir(DIR);

    /* --- mkdir, and mkdir that means it ---------------------------------- */

    if (minx_mkdir(DIR, 0755) != 0)
        return fail("could not create a directory");

    if (minx_mkdir(DIR, 0755) != -EEXIST)
        return fail("creating a directory that is already there did not say EEXIST");

    /* A directory inside a directory that is not there is ENOENT, not an implicit mkdir.
     * A filesystem that creates the parent quietly turns every typo in a path into a
     * successful command. */
    if (minx_mkdir(DIR "/nope/deeper", 0755) != -ENOENT)
        return fail("mkdir through a directory that does not exist did not say ENOENT");

    /* --- create, write, read --------------------------------------------- */

    const char *text = "hello fstest";
    size_t_ len = minx_strlen(text);

    int fd = (int)minx_open(FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return fail("could not create a file");

    if (minx_write(fd, text, len) != (long)len)
        return fail("a write of 11 bytes did not report 11");

    if (minx_close(fd) != 0)
        return fail("close of a writable descriptor failed");

    /* The size a program sees is the size the kernel has, which is the only way to tell a
     * write that was counted from a write that landed. */
    struct minx_stat st;
    if (minx_stat(FILE, &st) != 0)
        return fail("stat of a file that was just written failed");
    if ((unsigned long)st.st_size != len)
        return fail("a file of 11 bytes reports a different size");

    if ((st.st_mode & S_IFMT) != S_IFREG)
        return fail("a file does not report as a regular file");
    if ((st.st_mode & 0777) != 0644)
        return fail("the mode a file was created with did not survive");

    /* Read it back through a *new* descriptor.  Reusing the one just written would test
     * the page cache rather than the filesystem, and on a filesystem with no cache it
     * would pass by accident. */
    char buf[64];
    fd = (int)minx_open(FILE, O_RDONLY, 0);
    if (fd < 0)
        return fail("could not reopen a file for reading");

    memset(buf, 0, sizeof(buf));
    if (minx_read(fd, buf, sizeof(buf)) != (long)len)
        return fail("reading a file back did not return its length");
    if (!same(buf, text, len))
        return fail("the bytes read back are not the bytes written");

    /* A read at the end of the file is zero bytes, not an error and not a short read of
     * something.  A program appending to a file checks this every time it does. */
    if (minx_lseek(fd, 0, SEEK_END) != (long)len)
        return fail("lseek to the end did not report the file's length");
    if (minx_read(fd, buf, sizeof(buf)) != 0)
        return fail("reading at the end of a file did not return zero bytes");

    if (minx_close(fd) != 0)
        return fail("close of a readable descriptor failed");

    /* --- the errors a caller has to be able to see ----------------------- */

    if (minx_open(DIR "/absent", O_RDONLY, 0) != -ENOENT)
        return fail("opening a file that is not there did not say ENOENT");

    if (minx_open(FILE, O_WRONLY | O_CREAT | O_EXCL, 0644) != -EEXIST)
        return fail("O_EXCL on a file that exists did not say EEXIST");

    /* Writing through a descriptor opened read-only has to fail.  A descriptor that
     * ignores its own flags is a descriptor that will happily let a program overwrite the
     * file it was told it could only read. */
    fd = (int)minx_open(FILE, O_RDONLY, 0);
    if (fd < 0)
        return fail("reopening for the read-only check failed");
    if (minx_write(fd, "x", 1) != -EBADF)
        return fail("writing to a read-only descriptor did not say EBADF");
    minx_close(fd);

    /* A kernel pointer is not a user pointer, and this is the check that the copy in and
     * out of a syscall is bounds-checked.  Without it, a program that passes a wild address
     * takes down the machine instead of getting an errno. */
    if (minx_open((const char *)(unsigned long)0xffff800000000000UL, O_RDONLY, 0) >= 0)
        return fail("a kernel address was accepted as a path");

    if (minx_close(999) != -EBADF)
        return fail("closing a descriptor that was never open did not say EBADF");

    /* --- rename ---------------------------------------------------------- */

    if (minx_rename(FILE, COPY) != 0)
        return fail("rename failed");

    /* The old name is gone and the new one is here: either half alone would pass on a
     * filesystem that copied instead of moving, or that moved without unlinking. */
    if (minx_stat(FILE, &st) != -ENOENT)
        return fail("the old name still resolves after a rename");
    if (minx_stat(COPY, &st) != 0)
        return fail("the new name does not resolve after a rename");
    if ((unsigned long)st.st_size != len)
        return fail("the renamed file is a different size");

    /* Renaming something that is not there is ENOENT, and it must not have created the
     * destination -- a failed rename that leaves a file behind is how a directory slowly
     * fills up with files nobody can account for. */
    if (minx_rename(DIR "/absent", DIR "/landing") != -ENOENT)
        return fail("renaming a file that is not there did not say ENOENT");
    if (minx_stat(DIR "/landing", &st) != -ENOENT)
        return fail("a failed rename created its destination");

    /* A rename onto a directory is EISDIR; a rename that replaced it would have destroyed
     * a whole subtree to save a syscall. */
    if (minx_rename(COPY, DIR) != -EISDIR)
        return fail("renaming a file onto a directory did not say EISDIR");
    if (minx_stat(COPY, &st) != 0)
        return fail("a refused rename removed the file anyway");

    /* --- hard link ------------------------------------------------------- */

    if (minx_link(COPY, LINK) != 0)
        return fail("link failed");

    memset(buf, 0, sizeof(buf));
    fd = (int)minx_open(LINK, O_RDONLY, 0);
    if (fd < 0)
        return fail("a hard link does not open");
    if (minx_read(fd, buf, sizeof(buf)) != (long)len || !same(buf, text, len))
        return fail("a hard link does not read as the file it names");
    minx_close(fd);

    /* Two names, one node: the inode numbers are equal and the link count is 2.  A
     * filesystem that copies instead of linking passes the read above and fails here. */
    struct minx_stat a, b;
    if (minx_stat(COPY, &a) != 0 || minx_stat(LINK, &b) != 0)
        return fail("stat of a linked pair failed");
    if (a.st_ino != b.st_ino)
        return fail("a hard link is a different node, not a second name");
    if (a.st_nlink != 2)
        return fail_num("a linked file reports a link count other than 2: ", a.st_nlink);

    /* Removing one name leaves the other and drops the count by exactly one.  This is the
     * check that a filesystem cannot pass by accident, and the reason a link count is
     * worth keeping. */
    if (minx_unlink(LINK) != 0)
        return fail("unlink of one of a linked pair failed");
    if (minx_stat(LINK, &b) != -ENOENT)
        return fail("the unlinked name still resolves");
    if (minx_stat(COPY, &a) != 0)
        return fail("unlinking one name removed the other");
    if (a.st_nlink != 1)
        return fail_num("after unlink the link count is not 1: ", a.st_nlink);

    /* A directory cannot be hard-linked.  Two links to a directory would give the tree a
     * cycle, and every walker downstream -- getdents, tar, rm -r -- assumes there are
     * none. */
    if (minx_link(DIR, DIR "/loop") != -EPERM)
        return fail("hard-linking a directory did not say EPERM");

    /* --- symbolic link --------------------------------------------------- */

    if (minx_symlink(TARGET, SYMLINK) != 0)
        return fail("symlink failed");

    /* lstat does not follow: the link itself is a link.  stat does follow: it is the
     * file.  A filesystem where both report the same thing cannot support `ls -l` and
     * cannot support unlink-without-follow either. */
    if (minx_lstat(SYMLINK, &a) != 0)
        return fail("lstat of a symbolic link failed");
    if ((a.st_mode & S_IFMT) != S_IFLNK)
        return fail("a symbolic link does not report as a link");
    if (minx_stat(SYMLINK, &b) != 0)
        return fail("stat through a symbolic link failed");
    if ((b.st_mode & S_IFMT) != S_IFREG)
        return fail("stat through a symbolic link does not report the file");

    /* readlink gives the target, and does not follow it.  A relative target is the
     * interesting one: it is resolved against the *link's* directory, and a kernel that
     * resolves it against the root gives a file that does not exist. */
    memset(buf, 0, sizeof(buf));
    size_t_ tlen = minx_strlen(TARGET);
    if (minx_readlink(SYMLINK, buf, sizeof(buf)) != (long)tlen || !same(buf, TARGET, tlen))
        return fail("readlink did not give the target");

    fd = (int)minx_open(SYMLINK, O_RDONLY, 0);
    if (fd < 0)
        return fail("opening through a symbolic link failed");
    memset(buf, 0, sizeof(buf));
    if (minx_read(fd, buf, sizeof(buf)) != (long)len || !same(buf, text, len))
        return fail("reading through a symbolic link does not follow it");
    minx_close(fd);

    /* --- getdents: the tree has to be walkable --------------------------- */

    fd = (int)minx_open(DIR, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return fail("could not open a directory for reading");

    /* Three entries plus `.` and `..`.  A `getdents` that returns only the names it feels
     * like, or that forgets to resume where it left off, still returns something -- so the
     * check is both that every name arrived *and* that the second call returned zero. */
    struct minx_dirent dbuf[16];
    unsigned saw_dot = 0, saw_dotdot = 0, saw_copy = 0, saw_soft = 0;
    unsigned count = 0;
    long got;

    while ((got = minx_getdents(fd, dbuf, sizeof(dbuf))) > 0) {
        long off = 0;

        /* Walk by each record's *own* length.  The length is in the record precisely so
         * that a caller who does not like the name length can skip to the next one, which
         * means the stride has to come from the record being stepped over -- reading it out
         * of the first entry instead makes every step the first entry's size, and a
         * directory whose names are not all the same length then walks off into the
         * middle of a name.  Every name here is short and the records are all 288 bytes,
         * so this bug is invisible until a program has a long name in a directory. */
        while (off + (long)sizeof(struct minx_dirent) <= got) {
            struct minx_dirent *de = (struct minx_dirent *)((char *)dbuf + off);

            if (de->d_reclen < sizeof(struct minx_dirent))
                return fail("a directory entry is shorter than its own record length");
            off += de->d_reclen;
            count++;

            if (minx_strcmp(de->d_name, ".") == 0)
                saw_dot++;
            else if (minx_strcmp(de->d_name, "..") == 0)
                saw_dotdot++;
            else if (minx_strcmp(de->d_name, "copy") == 0)
                saw_copy = de->d_type;
            else if (minx_strcmp(de->d_name, "soft") == 0)
                saw_soft = de->d_type;
        }
    }

    if (got < 0)
        return fail("getdents returned an error");

    if (count != 4 || saw_dot != 1 || saw_dotdot != 1 || !saw_copy || !saw_soft)
        return fail("getdents did not list every entry, once each");

    /* The type is in the entry so that a program can tell a directory from a file without
     * a `stat` per name.  It is one of the `DT_*` values, the same ones every other Unix
     * puts there, so a program can write `d_type == MINX_DT_DIR` and mean it -- and a
     * getdents that always says "unknown" turns `ls` into a program that stats every name
     * it already has an answer for. */
    if (saw_copy != MINX_DT_REG)
        return fail_num("getdents reports a regular file as type ", saw_copy);
    if (saw_soft != MINX_DT_LNK)
        return fail_num("getdents reports a symbolic link as type ", saw_soft);

    if (minx_close(fd) != 0)
        return fail("close of a directory descriptor failed");

    /* --- unlink, rmdir, and the rules between them ----------------------- */

    if (minx_unlink(SYMLINK) != 0)
        return fail("unlink of a symbolic link failed");
    if (minx_lstat(SYMLINK, &a) != -ENOENT)
        return fail("an unlinked link still resolves");

    /* rmdir on a file is ENOTDIR, and unlink on a directory is EISDIR or EPERM.  A kernel
     * that accepts both turns `rm -rf` into something that does not know what it is
     * deleting. */
    if (minx_rmdir(COPY) != -ENOTDIR)
        return fail("rmdir on a regular file did not say ENOTDIR");
    if (minx_unlink(DIR) == 0)
        return fail("unlink on a directory was allowed");

    /* A directory with something in it is ENOTEMPTY.  This is the check that keeps `rm -r`
     * without `-r` honest, and it is the reason procfs has to empty a process's directory
     * before removing it. */
    if (minx_mkdir(DIR "/occupied", 0755) != 0)
        return fail("could not create the occupied directory");
    if (minx_rmdir(DIR) != -ENOTEMPTY)
        return fail("rmdir of a directory that is not empty did not say ENOTEMPTY");

    if (minx_unlink(COPY) != 0)
        return fail("unlink of the last file failed");
    if (minx_rmdir(DIR "/occupied") != 0)
        return fail("rmdir of an empty directory failed");
    if (minx_rmdir(DIR) != 0)
        return fail("rmdir of the test directory failed");
    if (minx_stat(DIR, &st) != -ENOENT)
        return fail("the test directory is still there after being removed");

    /* --- the generated filesystems --------------------------------------- */

    /* /dev and /proc are in the mount table, they answer, and their contents are
     * generated per process rather than stored.  A /proc that is an ordinary directory of
     * files written once at boot is a /proc that lies from the second process onwards. */
    if (minx_mkdir(DIR, 0755) != 0)
        return fail("could not recreate the test directory");

    fd = (int)minx_open("/dev", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return fail("/dev does not open as a directory");
    minx_close(fd);

    if (minx_lstat("/dev/console", &st) != 0)
        return fail("/dev/console does not exist");
    if ((st.st_mode & S_IFMT) != S_IFCHR)
        return fail("/dev/console is not a character device");

    if (minx_lstat("/proc/self/status", &st) != 0)
        return fail("/proc/self/status does not exist");

    /* The process that reads it is the one it describes: the pid in the file has to be
     * this program's, not the one that happened to be scheduled first. */
    fd = (int)minx_open("/proc/self/status", O_RDONLY, 0);
    if (fd < 0)
        return fail("/proc/self/status does not open");
    memset(buf, 0, sizeof(buf));
    got = minx_read(fd, buf, sizeof(buf) - 1);
    minx_close(fd);
    if (got <= 0)
        return fail("/proc/self/status read nothing");

    /* The pid in the file has to be *this* program's.  `/proc/self` resolving to the
     * caller's directory is half the check; the other half is that what the file then
     * describes is the process reading it, and not whichever process the directory was
     * made for.  A `/proc/self` that always answers for pid 1 passes every check above and
     * reports the wrong process for every program on the machine.
     *
     * The field is found rather than assumed: the format is "Name:...\nPid:\t<n>\n", and
     * a program that expects to read it at a fixed offset breaks the day somebody inserts a
     * field.  Looking for the line is what `grep` does, and it is equally correct. */
    const char *p = minx_strstr(buf, "\nPid:\t");

    if (p == NULL)
        return fail("/proc/self/status has no Pid field");

    p += 6;
    char digits[24];
    size_t_ dn = 0;
    unsigned long me = (unsigned long)minx_getpid();

    for (; *p >= '0' && *p <= '9'; p++)
        digits[dn++] = *p;

    if (dn == 0)
        return fail("/proc/self/status has an empty Pid field");

    digits[dn] = 0;
    if (*p != '\n')
        return fail("/proc/self/status Pid field is not terminated");

    /* Compared as a number rather than as text, so a leading zero or a shorter spelling
     * still counts as the right answer -- which is what a program parsing this is after. */
    unsigned long v = 0;

    for (size_t_ i = 0; i < dn; i++)
        v = v * 10 + (unsigned long)(digits[i] - '0');

    if (v != me)
        return fail_num("/proc/self/status describes pid ", v);

    /* /proc/mounts is the mount table, read as text, because a program that cannot ask
     * where a filesystem came from cannot report anything about it.
     *
     * The format is one line per mount -- "<path> <source> <type> rw 0 0" -- so the check
     * is line by line rather than a substring search: `/dev` has to appear as a *whole
     * mount point*, and a search for "/dev" anywhere in the file would also match
     * "/dev/console" if anything ever listed one. */
    /* Its own buffer, not the 64-byte one above: a read that stops in the middle of a
     * line leaves a parser looking at half a line, and a check that then fails says
     * something about the parser. */
    static char mbuf[512];

    fd = (int)minx_open("/proc/mounts", O_RDONLY, 0);
    if (fd < 0)
        return fail("/proc/mounts does not exist");
    memset(mbuf, 0, sizeof(mbuf));
    got = minx_read(fd, mbuf, sizeof(mbuf) - 1);
    minx_close(fd);
    if (got <= 0)
        return fail("/proc/mounts read nothing");

    if (mbuf[got - 1] != '\n')
        return fail("/proc/mounts did not end in a newline, so the read was truncated");


    int saw_root = 0, saw_dev = 0, saw_proc = 0;

    for (long i = 0; i < got; ) {
        long end2 = i;
        long arg = i;

        while (end2 < got && mbuf[end2] != '\n')
            end2++;
        if (end2 < got)
            mbuf[end2] = 0;          /* the line, as a string, for the comparisons below */

        if (mbuf[i] == '/') {
            /* Three fields before the options: the mount point, where the contents came
             * from, and what kind of filesystem it is.  The third is the one worth
             * checking -- each of the three has to be reported as the filesystem it is,
             * so `/dev` saying "ramfs" is as wrong as `/dev` missing.
             *
             * Skipped by counting fields rather than by searching, because "/proc/mounts"
             * may one day list "/dev/pts", and a search for "/dev" would find the wrong
             * line.  The mount point is compared as a whole field by putting a terminator
             * over the space that ends it and putting the space back afterwards, so that
             * `"/dev"` and `"/dev/pts"` cannot be confused for one another. */
            long sp = i + 1;

            while (mbuf[sp] != 0 && mbuf[sp] != ' ')
                sp++;

            /* The rest of the line is located *before* the mount point is cut down to a
             * field, because cutting it first turns the space that ends the mount point
             * into the terminator of the whole line and leaves nothing to walk to the
             * type with. */
            arg = sp;
            for (int field = 0; field < 2; field++) {
                while (mbuf[arg] != 0 && mbuf[arg] != ' ')
                    arg++;
                while (mbuf[arg] == ' ')
                    arg++;
            }

            /* Both compared fields are made into whole strings: the mount point by its
             * trailing space, and the type by the space after it.  Comparing the type
             * against the rest of the line would ask whether "ramfs rw 0 0" is "ramfs",
             * which is a different question and always answered no. */
            long te = arg;
            while (mbuf[te] != 0 && mbuf[te] != ' ')
                te++;

            char saved_sp = mbuf[sp];
            char saved_te = mbuf[te];

            mbuf[sp] = 0;
            mbuf[te] = 0;

            if (minx_strcmp(mbuf + i, "/") == 0)
                saw_root = minx_strcmp(mbuf + arg, "ramfs") == 0 ? 1 : -1;
            else if (minx_strcmp(mbuf + i, "/dev") == 0)
                saw_dev = minx_strcmp(mbuf + arg, "devfs") == 0 ? 1 : -1;
            else if (minx_strcmp(mbuf + i, "/proc") == 0)
                saw_proc = minx_strcmp(mbuf + arg, "procfs") == 0 ? 1 : -1;

            mbuf[sp] = saved_sp;
            mbuf[te] = saved_te;
        }

        i = end2 + 1;
    }

    if (saw_root != 1)
        return fail("/proc/mounts does not list / as a ramfs");
    if (saw_dev != 1)
        return fail("/proc/mounts does not list /dev as a devfs");
    if (saw_proc != 1)
        return fail("/proc/mounts does not list /proc as a procfs");

    /* --- tidying up ------------------------------------------------------ */

    (void)minx_rmdir(DIR);

    minx_report("fstest", 1, NULL);
    return 0;
}