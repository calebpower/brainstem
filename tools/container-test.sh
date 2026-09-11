#!/bin/sh
# tools/container-test.sh -- run the whole suite in a container.
#
# THIS IS THE LINUX HALF OF THE FALLBACK, NOT THE GATE, AND NOT EVEN ALL OF
# THE FALLBACK. `reaper test` is what a change is judged by, and it is two
# sessions: freebsd-15.1 and ubuntu-26.04. This covers the second one. On a
# FreeBSD host the fallback for the first is `sh tests/run.sh` natively, run
# the way the gate runs it, since the tenant is exec = "host".
#
# So a green run here against a change touching src/sys_freebsd.c, or any op
# that crosses the platform seam, has proved approximately nothing about the
# primary target. Saying so at both ends of the run is the whole reason this
# banner exists.
#
# Usage:  sh tools/container-test.sh
#         CONTAINER_ENGINE=docker sh tools/container-test.sh
set -eu

IMAGE=${BRAINSTEM_IMAGE:-brainstem-suite}
engine=${CONTAINER_ENGINE:-podman}
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

command -v "$engine" >/dev/null 2>&1 || {
    echo "container-test: no '$engine' on PATH" >&2
    echo "container-test: set CONTAINER_ENGINE, or install podman" >&2
    exit 1
}

mkdir -p "$repo/out"

# The host paths, which are the engine's business, and the container paths,
# which are not. On a Unix host these are the same strings and the block below
# does nothing.
#
# Under Git Bash the engine is a native Windows binary, and MSYS rewrites every
# argument that LOOKS like a POSIX path before it arrives. Left alone it turns
# -v /repo:/src:ro into "\Program Files\Git\src;ro" -- it reads the whole
# argument as a colon-separated path LIST. Turning the rewriting off is
# necessary but not sufficient: podman then reads the host half of a mount, and
# the build context, as Windows paths and resolves /d/projects to
# D:\d\projects. Neither setting is right for both halves, so the halves are
# handled separately -- rewriting off, host paths converted explicitly with
# cygpath, container paths written literally.
host=$repo
hostout=$repo/out
hostfile=$repo/Containerfile
case ${OSTYPE:-$(uname -s)} in
    msys* | cygwin* | MINGW* | MSYS* | CYGWIN*)
        MSYS_NO_PATHCONV=1
        MSYS2_ARG_CONV_EXCL='*'
        export MSYS_NO_PATHCONV MSYS2_ARG_CONV_EXCL
        host=$(cygpath -w -- "$repo")
        hostout=$(cygpath -w -- "$repo/out")
        hostfile=$(cygpath -w -- "$repo/Containerfile")
        ;;
esac

echo "container-test: THE LINUX HALF of the fallback. 'reaper test' is the gate,"
echo "container-test: and the gate is freebsd-15.1 AND ubuntu-26.04."
echo "container-test: building $IMAGE (the first build needs a network)"
"$engine" build -t "$IMAGE" -f "$hostfile" "$host"

# The tree goes in READ ONLY and is copied to scratch inside the container.
#
# Two reasons, and the first is a defect the sibling has already had.
# .reaper.toml excludes host-built binaries from its sync because a
# FreeBSD-built bfi looks present to the build and then fails to exec; a bind
# mount from a Windows or FreeBSD checkout carries exactly the same foreign
# binaries, so build/ is deleted from the copy and rebuilt. Second, a Windows
# bind mount is a bad place to compile. The copy pays the crossing once.
#
# Writing to the mounted tree is refused rather than merely avoided: nothing
# this runs can leave a Linux ELF binary in the checkout you are editing.
#
# On an SELinux host add ,z to the :ro below if the mount is denied.
#
# --security-opt seccomp=unconfined, and the reason, because a bare security
# flag in a script invites someone to delete it: the default profile filters
# syscalls the broker legitimately makes -- clone3 behind posix_spawn has been
# blocked outright by older profiles -- and it would refuse the seccomp filter
# the purity tier installs. Either failure surfaces as an op returning a
# plausible-looking error rather than as a container problem, which is the
# worst shape a lane-specific defect can take: the fallback disagreeing with
# the gate about the PRODUCT. reaper runs exec = "host" and has no such
# filter, so unconfining here is what makes the two mean the same thing. It is
# not a loosening of the project's posture; brainstem makes no containment
# claim at all (see Named non-goals).
#
# There is no -w flag. The Containerfile's WORKDIR already lands the container
# in /work, so the flag was only ever a second way of saying it, and a badly
# behaved one: a podman 4.5 client against a 4.9 server refuses it outright,
# "workdir /work does not exist", for a directory that does exist. The `cd`
# below says the same thing where no engine can misread it.
#
# `set -e` would abort on a red suite before the status is captured, losing the
# closing lines at exactly the moment they are wanted, so the run is guarded.
status=0
"$engine" run --rm \
    --security-opt seccomp=unconfined \
    -v "$host:/src:ro" \
    -v "$hostout:/out" \
    "$IMAGE" \
    sh -euc '
        cd /work
        cp -a /src/. /work/
        rm -rf build out
        sh tools/guest-setup.sh --build
        # No pipe into tee. /bin/sh here is dash: no pipefail, no PIPESTATUS,
        # and the suite exit status is the entire point of running it. This is
        # the same idiom .reaper.toml uses, for the same reason.
        sh tests/run.sh > /out/suite.log 2>&1; s=$?
        cat /out/suite.log
        exit $s
    ' || status=$?

echo "container-test: log in out/suite.log"
echo "container-test: that was the LINUX half only. The FreeBSD half is unproven"
echo "container-test: by this run; gate with 'reaper test' before landing."
exit $status
