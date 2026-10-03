#!/usr/bin/env python3
"""Build an initrd archive in "newc" cpio format.

Usage: mkinitrd.py <output.cpio> <root-directory> [--mode MODE]

Used instead of cpio(1) so that a clean checkout builds on hosts without the
cpio tool installed.  Directories are emitted with mode 0755 (or MODE), files
keep their permission bits with the set-uid/sticky bits masked off, and the
entry order is stable so rebuilds are reproducible.
"""

import os
import stat
import sys

TRAILER = "TRAILER!!!"
NEW_MAGIC = b"070701"

# 6 bytes of magic plus 13 fields of 8 hex characters each.
HEADER_SIZE = 6 + 13 * 8

# Field order of the newc header, all 8-character uppercase hex.
_FIELDS = (
    "ino", "mode", "uid", "gid", "nlink", "mtime", "filesize",
    "devmajor", "devminor", "rdevmajor", "rdevminor", "namesize", "check",
)


def newc_header(**kwargs) -> bytes:
    """Render one newc entry header.  Values default to 0; name is required."""
    name = kwargs.pop("name")
    for key in _FIELDS:
        if key not in kwargs:
            kwargs[key] = 0
    # The name field is stored NUL-terminated and its length includes that NUL.
    kwargs["namesize"] = len(name) + 1
    unknown = set(kwargs) - set(_FIELDS)
    if unknown:
        raise ValueError(f"unknown newc fields: {sorted(unknown)}")

    out = bytearray(NEW_MAGIC)
    for key in _FIELDS:
        out += b"%08X" % (kwargs[key] & 0xFFFFFFFF)
    out += name + b"\0"
    while len(out) % 4 != 0:
        out += b"\0"
    return bytes(out)


def collect(root: str):
    """Return (name, path, is_dir) entries rooted at *root*, deterministically."""
    entries = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        filenames.sort()
        rel = os.path.relpath(dirpath, root)
        if rel == ".":
            entries.append(("./", dirpath, True))
        else:
            entries.append((rel + "/", dirpath, True))
        for name in filenames:
            full = os.path.join(dirpath, name)
            if os.path.islink(full):
                continue
            # Build stamps are an artefact of the Makefile, not part of the
            # filesystem the guest sees.
            if name.endswith((".stamp", "~")):
                continue
            entries.append((os.path.join(rel, name), full, False))
    entries.sort(key=lambda e: e[0])
    return entries


def write_archive(out, root: str, dir_mode: int):
    entries = collect(root)
    ino = 1
    mtime = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
    for name, path, is_dir in entries:
        if is_dir:
            data = b""
            mode = dir_mode | stat.S_IFDIR
            nlink = 2
        else:
            st = os.lstat(path)
            data = open(path, "rb").read()
            mode = stat.S_IMODE(st.st_mode) & 0o7777 & ~0o7000
            mode |= stat.S_IFREG
            nlink = 1
        raw = name.encode()
        out.write(newc_header(name=raw, ino=ino, mode=mode, nlink=nlink,
                              mtime=mtime, filesize=len(data)))
        out.write(data)
        pad = (-len(data)) % 4
        if pad:
            out.write(b"\0" * pad)
        ino += 1

    # The trailer entry ends the archive: a header with an all-zero payload and
    # the name "TRAILER!!!".  Everything is padded to a 4-byte boundary.
    raw = TRAILER.encode()
    out.write(newc_header(name=raw, nlink=1))
    trailer_pad = -(HEADER_SIZE + len(raw) + 1) % 4
    if trailer_pad:
        out.write(b"\0" * trailer_pad)

    # The archive as a whole is padded to 4 bytes so that loaders which copy it
    # around in word-sized chunks never read past the end.
    tail_pad = -out.tell() % 4
    if tail_pad:
        out.write(b"\0" * tail_pad)


def main() -> int:
    args = sys.argv[1:]
    mode = 0o755
    if "--mode" in args:
        i = args.index("--mode")
        mode = int(args[i + 1], 8)
        del args[i:i + 2]
    if len(args) != 2:
        sys.stderr.write(__doc__)
        return 2
    out_path, root = args
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "wb") as out:
        write_archive(out, root, mode)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())