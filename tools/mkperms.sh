#!/bin/sh
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
# mkperms.sh ROOTDIR PERMSFILE - emit debugfs commands that make every file
# owned by root and then apply the modes/owners/device nodes in PERMSFILE.
ROOT=$1
PERMS=$2
(cd "$ROOT" && find . -mindepth 1) | sed 's|^\.||' | while read -r p; do
    echo "sif \"$p\" uid 0"
    echo "sif \"$p\" gid 0"
done
grep -v '^#' "$PERMS" | while read -r path mode uid gid type major minor; do
    [ -z "$path" ] && continue
    if [ -n "$type" ]; then
        echo "cd \"$(dirname "$path")\""
        echo "mknod \"$(basename "$path")\" $type $major $minor"
        echo "cd /"
        typebits=020000
        [ "$type" = b ] && typebits=060000
    elif [ -d "$ROOT$path" ]; then
        typebits=040000
    else
        typebits=0100000
    fi
    printf 'sif "%s" mode 0%o\n' "$path" $(( typebits + 0$mode ))
    echo "sif \"$path\" uid $uid"
    echo "sif \"$path\" gid $gid"
done
