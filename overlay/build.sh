#!/bin/bash
# Builds the overlay. Run from the overlay folder via a path without spaces (build.ps1 does
# this through a junction). Usage: build.sh [make target]
export DEVKITPRO=/opt/devkitpro
# libultrahand's -DUI_OVERRIDE_PATH="/config/..." must not be rewritten to a Windows path
# by MSYS2; everything else (source paths) still needs conversion.
export MSYS2_ARG_CONV_EXCL="-DUI_OVERRIDE_PATH"
export PATH=/usr/bin:/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:/opt/devkitpro/portlibs/switch/bin

make -j8 "$@" > build.log 2>&1
rc=$?
# Show diagnostics from our own sources, then the tail of the build.
grep -E "source/(main|splash_cfg)\.(cpp|hpp).*(error|warning)" -A3 build.log | head -60
tail -4 build.log
exit $rc
