#!/usr/bin/env bash
# 在 MSYS2 bash 里编译 MELODY PSP
set -e

if [ -z "$PSPDEV" ]; then
    PSPDEV=/e/pspdev
fi
export PSPDEV
export PATH="$PSPDEV/bin:/usr/bin:/bin"

cd "$(dirname "$0")"
make "$@"
