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

# The codec, and the second reading of the specification that checks it.
#
# bscodec links src/frame.c and src/err.c; bsframe links NOTHING from src/ and
# is compiled from one file on purpose. If a future edit makes bsframe need a
# header out of src/, that is the moment the two stopped being independent and
# the tier that compares them stopped meaning anything.
# shellcheck disable=SC2086
$CC $CFLAGS -o build/bscodec tools/bscodec.c src/frame.c src/err.c
# shellcheck disable=SC2086
$CC $CFLAGS -o build/bsframe tools/bsframe.c
# shellcheck disable=SC2086
$CC $CFLAGS -o build/bsbf    tools/bsbf.c

# The broker itself. src/ holds the product; tools/ holds the checkers, and
# the audit tier leans on that separation.
#
# EVERY TRANSLATION UNIT IS COMPILED SEPARATELY, and the objects are kept,
# because tools/bsaudit.sh reads them. An audit that read the source would be
# checking what the code says; reading `nm -u` on the object checks what the
# compiler actually emitted, which is the difference between a claim and a
# measurement. Keeping the objects is the whole reason this is not one cc
# line with eleven files on it.
BS_UNITS="main broker child ops op_ctl op_time op_rand frame err sys_posix det"

# The platform half of the seam is chosen here, by uname, and its object is
# the ONLY one compiled with a namespace widening macro. Everything above the
# seam builds against the common flags and cannot reach a platform extension
# even by accident; bsaudit.sh checks that against the objects rather than
# trusting this comment.
case "$(uname -s)" in
    FreeBSD) BS_SEAM=sys_freebsd; BS_SEAM_FLAGS=-D__BSD_VISIBLE=1 ;;
    Linux)   BS_SEAM=sys_linux;   BS_SEAM_FLAGS=-D_GNU_SOURCE ;;
    *)       echo "build: no platform seam for $(uname -s)" >&2; exit 1 ;;
esac
echo "build: seam is src/$BS_SEAM.c"

rm -rf build/obj
mkdir -p build/obj

for u in $BS_UNITS; do
    # shellcheck disable=SC2086
    $CC $CFLAGS -c -o "build/obj/$u.o" "src/$u.c"
done

# shellcheck disable=SC2086
$CC $CFLAGS $BS_SEAM_FLAGS -c -o "build/obj/$BS_SEAM.o" "src/$BS_SEAM.c"

# Recorded rather than inferred: bsaudit.sh must know which object is the
# platform half, and working it out from uname a second time would be a
# second definition of the thing this file exists to define once.
echo "$BS_SEAM" > build/obj/SEAM

# shellcheck disable=SC2086
$CC $CFLAGS -o build/brainstem build/obj/*.o

echo "build: done"
