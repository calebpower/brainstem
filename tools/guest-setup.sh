#!/bin/sh
# tools/guest-setup.sh — THE one definition of the toolchain.
#
# Both lanes run this file and only this file. reaper calls it with no argument
# (see .reaper.toml [build]); the Containerfile calls it with --toolchain so
# the slow half becomes a cached image layer; tools/container-test.sh calls it
# with --build against the tree under test.
#
# The phases live HERE, in the file reaper already runs, rather than being
# spelled out again in the Containerfile. A Containerfile carrying its own
# package list is a SECOND definition of the toolchain, and the day it drifts
# is the day the fallback lane starts passing what the gate would fail --
# silently, because a container with a different compiler still runs every
# test and still says PASS. tests/run.sh checks for that.
#
# Usage:  sh tools/guest-setup.sh              both phases (reaper's [build])
#         sh tools/guest-setup.sh --toolchain  provision only (the Containerfile)
#         sh tools/guest-setup.sh --build      compile only (container-test.sh)
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/.." && pwd)
cd "$repo"

usage() {
    echo "usage: sh tools/guest-setup.sh [--toolchain|--build]" >&2
    exit 2
}

do_toolchain=yes
do_build=yes
case "${1-}" in
    '')          [ $# -le 1 ] || usage ;;
    --toolchain) do_build=no ;;
    --build)     do_toolchain=no ;;
    *)           usage ;;
esac
[ $# -le 1 ] || usage

# The guest-to-uname mapping, declared where a machine can read it. reaper
# names guests ("ubuntu-26.04"); this script branches on uname ("Linux"); the
# two vocabularies do not line up on their own, so the correspondence is
# written down rather than inferred. tests/run.sh proves this list matches
# .reaper.toml's `guests` exactly, and that every OS named here has a branch
# below.
#
# Without that check, adding a guest silently takes whichever branch matches by
# accident and the tree then claims coverage of a platform nobody provisioned
# for -- or a branch is added here that no guest ever runs, and dead
# provisioning rots unnoticed.
#
# GUEST freebsd-15.1 FreeBSD
# GUEST ubuntu-26.04 Linux

if [ "$do_toolchain" = yes ]; then
    case "$(uname -s)" in
        FreeBSD)
            # Nothing is installed, and that is a fact about the base system
            # rather than an omission: clang, ktrace and kdump all ship in
            # base. This branch exists to SAY so, and to fail loudly if some
            # future image drops one, instead of letting it surface as a
            # compile error in the build phase.
            echo "guest-setup: FreeBSD -- toolchain is in base, verifying"
            for _gs_t in cc ktrace kdump; do
                command -v "$_gs_t" >/dev/null 2>&1 || {
                    echo "guest-setup: $_gs_t is missing from base" >&2
                    echo "guest-setup: this is not the guest brainstem targets" >&2
                    exit 1; }
            done
            ;;
        Linux)
            # ubuntu-26.04 ships NEITHER gcc NOR make. brainstem uses no make
            # at all -- tools/build.sh is the build -- but build-essential is
            # still the honest way to ask for a C compiler and a linker.
            # strace is the Linux half of the per-op syscall tier; FreeBSD
            # gets that from base.
            echo "guest-setup: Linux -- apt toolchain (C compiler, strace)"
            DEBIAN_FRONTEND=noninteractive; export DEBIAN_FRONTEND
            apt-get update -qq
            apt-get install -y -qq build-essential strace >/dev/null
            ;;
        *)
            echo "guest-setup: unsupported platform '$(uname -s)'" >&2
            echo "guest-setup: guests are declared in .reaper.toml; add a branch here first" >&2
            exit 1
            ;;
    esac
    echo "guest-setup: versions"
    cc --version 2>/dev/null | head -1
    uname -srm
fi

if [ "$do_build" = yes ]; then
    sh tools/build.sh
fi

echo "guest-setup: done"
