#!/bin/sh
# tools/build.sh — THE definition of how brainstem is compiled.
#
# Nothing else in this repository invokes a compiler. Not tests/run.sh, not the
# Containerfile, not guest-setup.sh beyond calling this script. That is a rule
# with a reason: bfsodium carries five `cc` lines in tests/run.sh AND five more
# in tools/guest-setup.sh, which is two definitions of the build living in the
# very repository whose Containerfile check exists to forbid two definitions of
# the toolchain. The day they drift, one lane compiles something the other does
# not, and the difference shows up as a test failure rather than as a build
# failure. tests/run.sh checks that no `cc` line has crept in beside this one.
#
# Usage:  sh tools/build.sh
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/.." && pwd)
cd "$repo"

CC=${CC:-cc}

# -std=c99 alone is not enough and the reason differs per platform, which is
# exactly why the flags live in one place.
#
# On glibc, -std=c99 defines __STRICT_ANSI__ and hides mkstemp and fdopen;
# bfsodium hit this and records it as "which is why this only ever failed off
# the development host". On FreeBSD the same macro hides arc4random_buf and
# most of <sys/socket.h>. _POSIX_C_SOURCE and _XOPEN_SOURCE put back exactly
# the surface both platforms agree on, and nothing wider: a file that needs
# more than this belongs behind the platform seam, and the audit tier will say
# so when there is one.
CFLAGS=${CFLAGS:-"-O2 -std=c99 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra"}

mkdir -p build

# Every binary the suite uses is built here, so a compile error is reported as
# a build failure rather than surfacing later as a mysterious missing tool.
# bfsodium's comment on the equivalent line is the one to remember: bffoot and
# bfstyle were missing from its list, "which meant the suite did not build at
# all on a current Linux guest, and said so only once it reached the run phase".
echo "build: cc is $($CC --version 2>/dev/null | head -1)"

# shellcheck disable=SC2086
$CC $CFLAGS -o build/bfi tools/bfi.c
# shellcheck disable=SC2086
$CC $CFLAGS -o build/hx  tools/hx.c

echo "build: done"
