#!/bin/sh
# tools/bscalls.sh — what each op actually asks the kernel for, MEASURED.
#
# Tier 10a, and the other half of the measured replacement for syscall-lean.
# bsaudit.sh reads the objects; this one runs the program under the platform's
# tracer and diffs what it observed against a multiset committed to this
# repository. Between them they buy the property the hand-written syscall
# wrappers were going to buy -- one op, one code path, no hidden I/O -- on the
# primary platform, where hand-written wrappers could never have gone.
#
# THE WINDOW IS FROM THE FORK ONWARDS. Everything before it is the dynamic
# loader, libc's startup and brainstem's own argument handling, none of which
# is ABI surface and all of which varies with the C library, the hardening
# flags and the phase of the moon. Measuring from the fork is not a
# convenience: it is the boundary between what this ABI does and what the C
# library does on its way to the first frame.
#
# It was chosen to match where sys_lockdown() would install a filter, so that
# M8 would change what was ENFORCED and not what was measured. That lockdown
# was deleted at M7 and the window is unchanged, because the reason above was
# always the better one.
#
# WHAT IS PINNED IS THE OP SPECIFIC PART. The broker's own plumbing -- the
# pipe reads and writes, the poll before each, the reaping and the exit -- is
# baseline and is excluded, because its counts depend on how a pipe chose to
# deliver bytes rather than on what an op did. What is left is the
# interesting part, and for most of these fixtures it is EMPTY, which is the
# strongest thing this tool can say:
#
#     rand.seeded  expects nothing at all. Under a seed the generator is
#                  ChaCha20 in this process and the kernel is not consulted,
#                  so a replay can be produced with the kernel out of the
#                  picture entirely. rand.live, next to it, expects the one
#                  call that fetches entropy. The pair is the measurement.
#
# PLATFORM NAMES ARE NORMALISED before comparison -- exit_group and _exit are
# both exit, clone and vfork are both fork, rt_sigprocmask is sigprocmask --
# because those spellings are the tracer's, not the program's. The expectation
# files stay per platform anyway, since which calls appear at all genuinely
# differs: Linux serves clock_gettime from the vDSO and FreeBSD from its
# timekeeping page, so neither shows it, while getrandom is a real syscall and
# arc4random_buf is a userspace generator.
#
# Usage:  sh tools/bscalls.sh              measure and diff
#         sh tools/bscalls.sh --record     write the expectation files
#         sh tools/bscalls.sh --report     measure the not-yet-pinned cases
#         sh tools/bscalls.sh --selftest   prove the extraction and the diff
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/.." && pwd)

# The broker's own plumbing. Excluded from every expectation, and listed here
# once so that a new entry is a visible decision. A call that is not on this
# list and not in an expectation file is a finding.
#
# fstat and lseek were on this list until M4 and are not any more. Nothing in
# the broker's own loop calls either; they were here on the assumption that
# libc startup would, which is the wrong side of the fork to be worrying
# about. Leaving them would have hidden the two syscalls behind fs.stat and
# fs.seek -- a baseline that swallows an op's own call makes the tier pass by
# not looking, which is the failure this file already warns about twice.
# fork, pipe, dup2 and execve came off this list at M6, and for the same
# reason fstat and lseek came off it at M4: the broker's own uses of them all
# happen BEFORE the window opens -- the window starts at the fork that starts
# the interpreter -- so anything the tracer sees afterwards belongs to
# proc.spawn and proc.pipe. A baseline that swallows an op's own call makes
# the tier pass by not looking.
#
# wait4 stays, and is the one genuinely ambiguous entry: the broker reaps its
# own interpreter inside the window, so a count here would mix that with
# proc.wait's. Named rather than quietly dropped, because the ambiguity is
# worth knowing about before someone adds a case that depends on it.
#
# brk joined this list after it changed from 2 to 3 between two versions of the
# same fixture. It is the C library's HEAP GROWING, which is a function of
# allocation history rather than of anything an op asked the kernel for -- and
# tier 10b already proves the broker itself allocates nothing, so a brk here
# can only ever be libc's. Pinning it meant pinning glibc's malloc behaviour,
# which is not what this tier is for and not something either platform
# promises. mmap stays visible: it carried real information on FreeBSD, where
# arc4random_buf allocates its generator state with an mmap and a minherit.
BASELINE="read write poll close wait4 sigprocmask sigaction sigreturn
exit kill ioctl brk"

# Spellings that belong to the tracer rather than to the program.
normalise_calls() {
    awk '{
        s = $0
        sub(/^rt_/, "", s)
        if (s == "exit_group" || s == "_exit") s = "exit"
        if (s == "clone" || s == "clone3" || s == "vfork" || s == "rfork") s = "fork"
        if (s == "ppoll" || s == "pselect6" || s == "kevent") s = "poll"
        if (s == "pipe2") s = "pipe"
        if (s == "wait6" || s == "waitpid" || s == "wait4") s = "wait4"
        if (s == "newfstatat" || s == "fstatat" || s == "fstat64") s = "fstat"
        if (s == "openat") s = "open"
        print s
    }'
}

# Pull the call sequence out of a tracer transcript. One name per line, in
# order, nothing else.
#
# strace prints "name(args) = ret" plus signal lines beginning with --- and
# exit lines beginning with +++; kdump prints "PID NAME CALL name(args)".
# Both are recognised here rather than in two scripts, because the only thing
# that actually differs between the platforms is this one parse.
extract() {
    case $1 in
        strace) sed -n 's/^\([a-z_][a-z_0-9]*\)(.*/\1/p' ;;
        kdump)  awk '$3 == "CALL" { sub(/\(.*/, "", $4); print $4 }' ;;
        *)      echo "bscalls: unknown transcript kind $1" >&2; return 2 ;;
    esac
}

# Everything from the fork onwards, counted, with the baseline dropped.
#
# If there is no fork in the transcript the tracer did not see the child being
# started, which means it was not tracing what this tool thinks it was. That
# is reported rather than silently yielding an empty -- and therefore
# passing -- multiset. A tier that passes when its measurement failed is worse
# than no tier.
window() {
    kind=$1
    tmp=$2
    extract "$kind" > "$tmp.calls"
    normalise_calls < "$tmp.calls" > "$tmp.norm"
    grep -qx fork "$tmp.norm" || { echo "bscalls: no fork in the transcript -- nothing was traced"; return 1; }
    awk 'seen { print } /^fork$/ { seen = 1 }' "$tmp.norm" > "$tmp.win"
    printf '%s\n' $BASELINE | grep . | sort -u > "$tmp.base"
    sort "$tmp.win" | uniq -c | awk '{ print $1, $2 }' | sort -k2 > "$tmp.counts"
    awk 'NR == FNR { b[$1] = 1; next } !($2 in b) { print $1, $2 }' \
        "$tmp.base" "$tmp.counts"
    return 0
}

# ---- the cases ------------------------------------------------------------
#
# NAME | broker options | fixture. One line per thing worth pinning, and the
# pairs are deliberate: frozen against live, seeded against live. A single
# reading proves a number; the pair proves the knob.
cases() {
    cat <<'EOT'
ctl.hello||bf/ctl/hello.bf
time.live||bf/time/clock.bf
time.frozen|--clock frozen=1700000000|bf/time/clock.bf
rand.live||bf/rand/bytes.bf
rand.seeded|--seed 000102030405060708090a0b0c0d0e0f|bf/rand/bytes.bf
fs.roundtrip|%W|bf/fs/roundtrip.bf
fs.refused|%W|bf/fs/refused.bf
proc.drive|--op-timeout 5000 %P|bf/proc/drive.bf
proc.refused|--op-timeout 5000 %P|bf/proc/refused.bf
EOT
}

# Cases that are MEASURED AND PRINTED but not yet compared against anything.
#
# This exists because of a lesson that cost two round trips through someone
# else's afternoon. The first freebsd/ expectations in this tree were written
# from what the platform documents rather than from a run, and two of the five
# were wrong -- FreeBSD issues two clock_gettime calls where Linux issues
# none, and arc4random_buf allocates its state with an mmap and a minherit on
# first use. Neither was guessable and both were obvious the moment a machine
# measured them.
#
# The filesystem ops landed at M4 on a development host that cannot reach the
# primary platform, so writing freebsd/fs.*.txt here would be the same guess a
# third time. Instead the suite MEASURES them on both guests and prints what
# it saw, the run that prints them is the run that produces the expectation,
# and they move into cases() above with a commit that says which platform said
# what. A report is not a check and is not counted as one; the tier table
# records tier 10a as covering the ops it actually covers.
#
# %W in the options is replaced with a directory created fresh for that case.
observe_cases() {
    cat <<'EOT'
EOT
}

platform_dir() {
    case "$(uname -s)" in
        FreeBSD) echo freebsd ;;
        Linux)   echo linux ;;
        *)       echo "bscalls: no tracer known for $(uname -s)" >&2; return 1 ;;
    esac
}

measure() {  # measure OPTS FIXTURE OUTFILE
    opts=$1; fixture=$2; out=$3
    t=${TMPDIR:-/tmp}/bscalls.$$
    # %W asks for a fresh empty WORKING DIRECTORY; %P for a fresh one with the
    # interpreter and the inner program in it, because a brainfuck program
    # cannot copy a binary. Two placeholders rather than one: fs.roundtrip
    # does a readdir and expects exactly one entry, so preparing every
    # directory the same way would break it.
    #
    # Neither is a broker option any more. Since the preopen model was
    # removed, a fixture reaches the filesystem the way any other process
    # does, so the setup is a cd rather than a flag -- and the binaries have
    # to be named absolutely from inside it.
    cwd=""
    case "$opts" in
        *%W*)
            rm -rf "$t.work"; mkdir -p "$t.work"
            cwd=$t.work
            opts=$(printf '%s' "$opts" | sed "s|%W||")
            ;;
        *%P*)
            rm -rf "$t.work"; mkdir -p "$t.work"
            cp build/bfi "$t.work/bfi"
            cp bf/proc/echo.bf "$t.work/echo.bf"
            cwd=$t.work
            opts=$(printf '%s' "$opts" | sed "s|%P||")
            ;;
    esac
    [ -n "$cwd" ] || cwd=$repo
    case "$(uname -s)" in
        Linux)
            command -v strace >/dev/null 2>&1 || { echo "bscalls: no strace"; return 1; }
            # No -f. The child is the interpreter, and what it asks the kernel
            # for is the interpreter's business, not this ABI's.
            (cd "$cwd" && strace -o "$t.raw" -qq "$repo/build/brainstem" $opts -- "$repo/build/bfi" "$repo/$fixture" </dev/null >/dev/null 2>&1) || true
            window strace "$t" < "$t.raw" > "$out" || return 1
            ;;
        FreeBSD)
            command -v ktrace >/dev/null 2>&1 || { echo "bscalls: no ktrace"; return 1; }
            rm -f "$t.ktrace"
            # No -i, for the same reason there is no -f above: -i would make
            # the trace inherit into the interpreter.
            (cd "$cwd" && ktrace -f "$t.ktrace" -t c "$repo/build/brainstem" $opts -- "$repo/build/bfi" "$repo/$fixture" </dev/null >/dev/null 2>&1) || true
            kdump -f "$t.ktrace" > "$t.raw" 2>/dev/null || true
            window kdump "$t" < "$t.raw" > "$out" || return 1
            ;;
        *) echo "bscalls: no tracer known for $(uname -s)"; return 1 ;;
    esac
    rm -f "$t.raw" "$t.calls" "$t.norm" "$t.win" "$t.base" "$t.counts" "$t.ktrace"
    return 0
}

# ---- self-test ------------------------------------------------------------
selftest() {
    d=${TMPDIR:-/tmp}/bscalls-self.$$
    rm -rf "$d"; mkdir -p "$d"
    bad=0
    check() {  # check LABEL EXPECTED ACTUAL
        if [ "$2" = "$3" ]; then echo "bscalls selftest ok: $1"
        else echo "bscalls SELFTEST FAIL: $1"; echo "  wanted: $2"; echo "  got:    $3"; bad=1; fi
    }

    cat > "$d/strace.txt" <<'EOT'
execve("/work/build/brainstem", ["brainstem"], 0x7ffd) = 0
openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY) = 3
getrandom("\x01\x02", 8, GRND_NONBLOCK) = 8
clone(child_stack=NULL, flags=CLONE_CHILD) = 42
read(3, "\1\n\0", 3)                    = 3
poll([{fd=3, events=POLLIN}], 1, 30000) = 1
getrandom("\xaa", 16, 0)                = 16
write(4, "\0\20\0", 3)                  = 3
--- SIGCHLD {si_signo=SIGCHLD} ---
+++ exited with 0 +++
EOT
    got=$(window strace "$d/s" < "$d/strace.txt")
    check "strace: the window starts at the clone and the baseline is dropped" \
          "1 getrandom" "$got"

    cat > "$d/kdump.txt" <<'EOT'
  9 brainstem CALL  execve(0x7fff,0x7fff,0x7fff)
  9 brainstem RET   execve 0
  9 brainstem CALL  sysarch(0xa,0x7fff)
  9 brainstem CALL  fork
  9 brainstem RET   fork 10/0xa
  9 brainstem CALL  read(0x3,0x8000,0x3)
  9 brainstem CALL  clock_gettime(0,0x7fff)
  9 brainstem CALL  clock_gettime(4,0x7fff)
  9 brainstem CALL  write(0x4,0x8000,0x3)
  9 brainstem CALL  exit(0)
EOT
    got=$(window kdump "$d/k" < "$d/kdump.txt")
    check "kdump: the same window, from the same fork, in the other dialect" \
          "2 clock_gettime" "$got"

    # The pre-fork calls must be invisible. getrandom appears twice in the
    # strace fixture above and only the second is inside the window; if the
    # window ever slipped, the first would show up as a count of 2.
    got=$(window strace "$d/s2" < "$d/strace.txt" | awk '$2 == "getrandom" { print $1 }')
    check "what libc did before the fork is not attributed to an op" "1" "$got"

    cat > "$d/nofork.txt" <<'EOT'
read(3, "x", 1) = 1
getrandom("\xaa", 16, 0) = 16
EOT
    if window strace "$d/n" < "$d/nofork.txt" > "$d/out" 2>&1; then
        echo "bscalls SELFTEST FAIL: a transcript with no fork was accepted"; bad=1
    else
        echo "bscalls selftest ok: a transcript with no fork is a failure, not an empty pass"
    fi

    # An empty expectation is the commonest one here, so it had better be
    # distinguishable from a measurement that produced nothing because it
    # never ran.
    cat > "$d/clean.txt" <<'EOT'
clone(child_stack=NULL) = 42
read(3, "x", 1) = 1
write(4, "y", 1) = 1
EOT
    got=$(window strace "$d/c" < "$d/clean.txt")
    check "a fixture that asks the kernel for nothing measures as nothing" "" "$got"

    cat > "$d/dirty.txt" <<'EOT'
clone(child_stack=NULL) = 42
openat(AT_FDCWD, "/etc/passwd", O_RDONLY) = 5
EOT
    got=$(window strace "$d/d" < "$d/dirty.txt")
    check "a call nobody declared shows up rather than being swallowed" "1 open" "$got"

    rm -rf "$d"
    return $bad
}

# ---- main -----------------------------------------------------------------

if [ "${1:-}" = "--selftest" ]; then
    selftest
    exit $?
fi

if [ "${1:-}" = "--report" ]; then
    cd "$repo"
    [ -x build/brainstem ] || { echo "bscalls: no build/brainstem" >&2; exit 2; }
    if [ -z "$(observe_cases)" ]; then
        echo "bscalls: nothing unpinned on $(platform_dir); every case is compared."
        exit 0
    fi
    echo "bscalls: NOT YET PINNED, measured on $(platform_dir). Paste these into"
    echo "bscalls: tests/syscalls/<platform>/ and move the case into cases()."
    observe_cases | while IFS='|' read -r name opts fixture; do
        [ -n "$name" ] || continue
        out=${TMPDIR:-/tmp}/bscalls-obs.$$
        if measure "$opts" "$fixture" "$out"; then
            echo "bscalls: --- $name"
            if [ -s "$out" ]; then sed 's/^/bscalls:     /' "$out"; else echo "bscalls:     (nothing beyond the baseline)"; fi
        else
            echo "bscalls: --- $name could not be measured"
        fi
        rm -f "$out"
    done
    exit 0
fi

cd "$repo"
[ -x build/brainstem ] || { echo "bscalls: no build/brainstem -- run sh tools/build.sh first" >&2; exit 2; }

plat=$(platform_dir)
dir=tests/syscalls/$plat
record=0
[ "${1:-}" = "--record" ] && record=1

[ "$record" = 1 ] && mkdir -p "$dir"

fail=0
cases | while IFS='|' read -r name opts fixture; do
    [ -n "$name" ] || continue
    out=${TMPDIR:-/tmp}/bscalls-obs.$$
    if ! measure "$opts" "$fixture" "$out"; then
        echo "bscalls: FAIL $name could not be measured"
        echo "$name" >> "${TMPDIR:-/tmp}/bscalls-fail.$$"
        continue
    fi
    if [ "$record" = 1 ]; then
        cp "$out" "$dir/$name.txt"
        echo "bscalls: recorded $dir/$name.txt"
        continue
    fi
    if [ ! -f "$dir/$name.txt" ]; then
        echo "bscalls: FAIL $name has no pinned expectation in $dir"
        echo "bscalls: observed:"; sed 's/^/bscalls:   /' "$out"
        echo "$name" >> "${TMPDIR:-/tmp}/bscalls-fail.$$"
        continue
    fi
    if cmp -s "$out" "$dir/$name.txt"; then
        echo "bscalls: ok $name"
    else
        echo "bscalls: FAIL $name"
        echo "bscalls: expected ($dir/$name.txt):"; sed 's/^/bscalls:   /' "$dir/$name.txt"
        echo "bscalls: observed:";                  sed 's/^/bscalls:   /' "$out"
        echo "$name" >> "${TMPDIR:-/tmp}/bscalls-fail.$$"
    fi
    rm -f "$out"
done

# The loop above runs in a subshell, so its failures come back through a file
# rather than through a variable. Written out plainly because the alternative
# is a pipeline whose exit status depends on which shell is reading it, and
# /bin/sh here is dash on one guest and ash-flavoured on the other.
if [ -f "${TMPDIR:-/tmp}/bscalls-fail.$$" ]; then
    fail=$(wc -l < "${TMPDIR:-/tmp}/bscalls-fail.$$" | tr -d ' ')
    rm -f "${TMPDIR:-/tmp}/bscalls-fail.$$"
    echo "bscalls: $fail case(s) disagree with the pinned surface"
    exit 1
fi
echo "bscalls: every op asks the kernel for exactly what $dir pins"
exit 0
