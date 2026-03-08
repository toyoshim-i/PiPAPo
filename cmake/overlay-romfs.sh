#!/bin/sh
# overlay-romfs.sh SRC DST — copy bin/, sbin/, usr/ from SRC into DST
# Preserves symlinks (cp -a) for busybox applets.
SRC="$1"
DST="$2"
for d in bin sbin usr; do
    [ -d "$SRC/$d" ] && cp -a "$SRC/$d" "$DST/"
done
exit 0
