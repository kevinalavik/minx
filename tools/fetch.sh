#!/bin/sh
# Pinned downloader for third-party assets (bootloader, fonts, libraries).
#
# Usage: fetch.sh <name> <url> <sha256> <destination>
#
# Downloads only if the destination is missing.  Verifies the SHA-256 of the
# downloaded payload before accepting it; an existing file whose hash matches is
# left alone so repeated builds never hit the network.
set -eu

if [ $# -ne 4 ]; then
    echo "usage: $0 <name> <url> <sha256> <destination>" >&2
    exit 2
fi

name=$1
url=$2
want=$3
dest=$4

if [ -f "$dest" ]; then
    have=$(sha256sum "$dest" | cut -d' ' -f1)
    if [ "$have" = "$want" ]; then
        exit 0
    fi
    echo "fetch: $name exists but hash mismatch, refetching" >&2
    rm -f "$dest"
fi

mkdir -p "$(dirname "$dest")"
tmp="$dest.fetch.$$"

echo "fetch: $name <- $url" >&2

if command -v curl >/dev/null 2>&1; then
    curl -fsSL --retry 3 -o "$tmp" "$url"
elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$tmp" "$url"
else
    echo "fetch: neither curl nor wget is available" >&2
    rm -f "$tmp"
    exit 1
fi

have=$(sha256sum "$tmp" | cut -d' ' -f1)
if [ "$have" != "$want" ]; then
    echo "fetch: $name SHA-256 mismatch" >&2
    echo "  expected $want" >&2
    echo "  got      $have" >&2
    rm -f "$tmp"
    exit 1
fi

mv -f "$tmp" "$dest"
exit 0