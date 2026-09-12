#!/bin/sh
# tools/bsaudit.sh — the seam is narrow, MEASURED.
#
# Tier 10b. This is one half of the measured replacement for the syscall-lean
# design the project started with and abandoned when FreeBSD became the
# primary platform. What syscall-lean was really buying -- one op, one code
# path, no hidden I/O -- is bought here instead, and bought better: this reads
# the OBJECTS the compiler emitted, not the source it was given. A comment
# saying "no allocation on the ABI path" is a claim; an empty `nm -u` is a
# measurement. tools/bscalls.sh is the other half.
#
# The seven rules, each named with the defect it exists to catch:
#
#   R1  every external symbol a unit references is on the allowlist, and
#       every allowlist entry is referenced by something. Catches an op that
#       grew an fopen, a second I/O route, a helper that pulled in half of
#       libc because it was easier than passing a buffer.
#   R2  nothing anywhere allocates. Catches the ABI path acquiring a malloc,
#       which would make a frame's cost depend on the heap and give the
#       broker a failure mode with no status code.
#   R3  the ops and the codec reference NOTHING external. Catches a printf
#       left in from debugging, and is the strongest single statement this
#       tool makes.
#   R4  no op reads errno. Catches an op mapping a platform number itself
#       instead of leaving it at the seam, which is how an errno reaches the
#       wire and platform parity quietly dies.
#   R5  sys_errmap is called only from the seam. The same defect, approached
#       from the other end.
#   R6  every op handler is referenced exactly once, by ops.o. Catches an op
#       calling another op, and any path to a handler that is not the table.
#   R7  exactly one platform object is linked, and it is the one build.sh
#       chose. Catches a stale object left behind by an earlier build.
#
# Usage:  sh tools/bsaudit.sh              audit build/obj
#         sh tools/bsaudit.sh --selftest   prove each rule fires
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/.." && pwd)

NM=${NM:-nm}

# Symbols that say nothing about what the code asked for. Listed by name
# rather than matched by pattern, so that adding one is a visible decision
# rather than a widened regex.
#
# Two groups. The first is what the compiler and linker put there on their
# own. The second is the pure memory and string helpers, and they are here
# because WHETHER THEY APPEAR AT ALL IS THE TOOLCHAIN'S CHOICE: gcc inlines
# the strcmp in det.c and clang may not, clang synthesises a memcpy call from
# a struct assignment gcc expanded in place. An allowlist that pinned those
# would fail on the primary platform for a reason that has nothing to do with
# this program -- which is the exact shape of defect this project has hit
# three times already. None of them performs I/O, allocates, or touches
# platform state, so excluding them costs the audit nothing it was measuring.
IGNORABLE="__stack_chk_fail __stack_chk_guard __stack_smash_handler
_GLOBAL_OFFSET_TABLE_ __gmon_start__ _DYNAMIC __dso_handle
__cxa_finalize __cxa_atexit
memcpy memmove memset memcmp
strlen strnlen strcmp strncmp strchr strrchr"

# Normalise a symbol to the name the SOURCE used.
#
# The decorations differ per toolchain and per hardening flag and mean nothing
# here: glibc's _FORTIFY_SOURCE turns fprintf into __fprintf_chk and sscanf
# into __isoc99_sscanf, errno is __errno_location on glibc and __error on
# FreeBSD, and signal can arrive as __sysv_signal. An allowlist written in
# decorated names would be a list of this week's compiler flags rather than a
# statement about the program.
normalise() {
    awk '{
        s = $0
        if (s == "__errno_location" || s == "__error" || s == "__errno") s = "errno"
        sub(/^__isoc99_/, "", s)
        sub(/_chk$/, "", s)
        sub(/^__sysv_/, "", s)
        sub(/^__/, "", s)
        print s
    }'
}

# Produce "UNIT KIND SYMBOL" for every object in a directory, where KIND is U
# for a symbol the unit needs and D for one it provides.
#
# Portable across GNU nm and FreeBSD's elftoolchain nm without asking either
# for a flag the other lacks: both print an undefined global as a two field
# line beginning with U, and a defined one as three fields beginning with an
# address. --defined-only would have been tidier and is not on both.
collect() {
    for o in "$1"/*.o; do
        [ -e "$o" ] || continue
        u=$(basename "$o" .o)
        $NM -g "$o" 2>/dev/null | awk -v u="$u" '
            NF == 2 && $1 == "U" { print u, "U", $2 }
            NF == 3 && $2 != "U" { print u, "D", $3 }'
    done
}

fail=0
say_fail() { echo "bsaudit: FAIL $*"; fail=1; }

# The audit proper, over a "UNIT KIND SYMBOL" listing.
#
# Taking a listing rather than a directory is what makes --selftest possible:
# a synthetic listing can violate a rule that no compiler on this machine
# would ever be persuaded to produce, and every rule below is therefore
# observed failing before it is trusted.
audit() {
    listing=$1
    allow=$2
    want_seam=$3
    tmp=${TMPDIR:-/tmp}/bsaudit.$$
    rm -rf "$tmp"; mkdir -p "$tmp"

    # A symbol some unit in this build defines is internal. What is left is
    # the external surface, and the external surface is the subject.
    awk '$2 == "D" { print $3 }' "$listing" | sort -u > "$tmp/defined"
    printf '%s\n' $IGNORABLE | grep . | normalise | sort -u > "$tmp/ignore"

    awk '$2 == "U" { print $1, $3 }' "$listing" > "$tmp/wanted.raw"
    : > "$tmp/wanted"
    while read -r u s; do
        printf '%s %s\n' "$u" "$(printf '%s\n' "$s" | normalise)" >> "$tmp/wanted"
    done < "$tmp/wanted.raw"
    sort -u "$tmp/wanted" -o "$tmp/wanted"

    awk 'NR == FNR { d[$1] = 1; next } !($2 in d) { print }' \
        "$tmp/defined" "$tmp/wanted" > "$tmp/ext.raw"
    awk 'NR == FNR { g[$1] = 1; next } !($2 in g) { print }' \
        "$tmp/ignore" "$tmp/ext.raw" > "$tmp/ext"

    grep -v '^[[:space:]]*#' "$allow" | grep . | sort -u > "$tmp/allow"

    # R1, both directions. An allowlist entry nobody needs is how a list
    # stops describing anything -- it is the same rot as a stale tier table,
    # and it gets the same treatment.
    while read -r u s; do
        grep -qx "$u $s" "$tmp/allow" || say_fail "R1 $u references $s, which is not on the allowlist"
    done < "$tmp/ext"
    # Only for units this build actually produced: tests/audit/allow.txt is
    # one file covering both platforms, so its sys_freebsd rows are vacuous
    # on Linux and its sys_linux rows are vacuous on FreeBSD. Complaining
    # about those would force two allowlists, and two allowlists drift.
    awk '{ print $1 }' "$listing" | sort -u > "$tmp/units"
    while read -r u s; do
        grep -qx "$u" "$tmp/units" || continue
        grep -qx "$u $s" "$tmp/ext" || say_fail "R1 the allowlist has '$u $s', which $u does not reference"
    done < "$tmp/allow"

    # R2 -- no allocation, anywhere, allowlisted or not
    got=$(awk '{ print $2 }' "$tmp/ext" | grep -xE 'malloc|calloc|realloc|free|strdup|strndup|asprintf|vasprintf|mmap' | sort -u || true)
    [ -z "$got" ] || say_fail "R2 something allocates: $(echo $got)"

    # R3 -- the ops and the codec reference nothing external at all
    got=$(awk '$1 ~ /^op_/ || $1 == "frame" || $1 == "err" { print $1 "/" $2 }' "$tmp/ext" || true)
    [ -z "$got" ] || say_fail "R3 an op or the codec reaches outside itself: $(echo $got)"

    # R4 -- no op reads errno
    got=$(awk '$1 ~ /^op_/ && $2 == "errno" { print $1 }' "$tmp/ext" || true)
    [ -z "$got" ] || say_fail "R4 an op reads errno: $(echo $got)"

    # R5 -- sys_errmap is called only from the seam
    got=$(awk '$2 == "U" && $3 ~ /sys_errmap/ && $1 !~ /^sys_/ { print $1 }' "$listing" | sort -u || true)
    [ -z "$got" ] || say_fail "R5 sys_errmap is called from $(echo $got)"

    # R6 -- every handler is reached exactly once, and from the table
    awk '$2 == "D" && $3 ~ /^op_/ { print $3 }' "$listing" | sort -u > "$tmp/handlers"
    while read -r h; do
        [ -n "$h" ] || continue
        callers=$(awk -v h="$h" '$2 == "U" && $3 == h { print $1 }' "$listing" | sort -u)
        n=$(printf '%s\n' "$callers" | grep -c . || true)
        if [ "$n" != 1 ]; then
            say_fail "R6 $h is referenced by $n units: $(echo $callers)"
        elif [ "$callers" != ops ]; then
            say_fail "R6 $h is referenced by $callers, not by the op table"
        fi
    done < "$tmp/handlers"

    # R7 -- one seam, and the one build.sh chose
    seams=$(awk '{ print $1 }' "$listing" | grep '^sys_' | grep -v '^sys_posix$' | sort -u || true)
    n=$(printf '%s\n' "$seams" | grep -c . || true)
    if [ "$n" != 1 ]; then
        say_fail "R7 $n platform objects are linked: $(echo $seams)"
    elif [ -n "$want_seam" ] && [ "$seams" != "$want_seam" ]; then
        say_fail "R7 the linked seam is $seams but build.sh chose $want_seam"
    fi

    rm -rf "$tmp"
}

# ---- self-test ------------------------------------------------------------
#
# Every rule is shown failing on a listing built to violate it, and the clean
# listing is shown passing before and after. A checker that has never been
# observed failing is indistinguishable from a clean corpus, which is the
# whole of tier 0.
selftest() {
    d=${TMPDIR:-/tmp}/bsaudit-self.$$
    rm -rf "$d"; mkdir -p "$d"
    bad=0

    base() {
        cat > "$d/listing" <<'EOT'
ops D bs_op_lookup
ops U op_ctl_hello
ops U __fprintf_chk
op_ctl D op_ctl_hello
frame D bs_get_u8
err D bs_err_name
sys_posix D sys_errmap
sys_posix U clock_gettime
sys_posix U __stack_chk_fail
sys_linux D sys_platform
sys_linux U sys_errmap
EOT
        cat > "$d/allow" <<'EOT'
# unit  symbol
ops fprintf
sys_posix clock_gettime
EOT
    }

    expect() {  # expect WANT LABEL
        want=$1; label=$2
        fail=0
        audit "$d/listing" "$d/allow" sys_linux > "$d/out" 2>&1 || true
        if [ "$fail" = "$want" ]; then
            echo "bsaudit selftest ok: $label"
        else
            echo "bsaudit SELFTEST FAIL: $label (fail=$fail, wanted $want)"
            cat "$d/out"
            bad=1
        fi
    }

    base; expect 0 "a clean listing passes, and a decorated symbol matches its plain name"

    base; echo "op_time U fopen" >> "$d/listing"
    expect 1 "R1 catches a symbol that is not on the allowlist"

    base; echo "ops nosuchsymbol" >> "$d/allow"
    expect 1 "R1 catches an allowlist entry nothing references"

    base; echo "broker U malloc" >> "$d/listing"; echo "broker malloc" >> "$d/allow"
    expect 1 "R2 catches an allocation even when the allowlist permits it"

    base; echo "op_rand U puts" >> "$d/listing"; echo "op_rand puts" >> "$d/allow"
    expect 1 "R3 catches an op reaching outside itself"

    base; echo "op_net U __errno_location" >> "$d/listing"; echo "op_net errno" >> "$d/allow"
    expect 1 "R4 catches an op reading errno"

    base; echo "broker U sys_errmap" >> "$d/listing"
    expect 1 "R5 catches sys_errmap called off the seam"

    base; echo "broker U op_ctl_hello" >> "$d/listing"
    expect 1 "R6 catches a second path to a handler"

    # A handler reached from somewhere other than the table, referenced
    # exactly once -- so the count is right and the caller is wrong, which is
    # the case a count alone would miss.
    cat > "$d/listing" <<'EOT'
ops D bs_op_lookup
broker U op_ctl_hello
op_ctl D op_ctl_hello
sys_linux D sys_platform
EOT
    : > "$d/allow"
    expect 1 "R6 catches a handler reached from outside the table"

    base; echo "sys_freebsd D sys_platform_other" >> "$d/listing"
    expect 1 "R7 catches two platform objects"

    base; expect 0 "and the clean listing still passes afterwards"

    rm -rf "$d"
    return $bad
}

if [ "${1:-}" = "--selftest" ]; then
    selftest
    exit $?
fi

cd "$repo"
[ -d build/obj ] || { echo "bsaudit: no build/obj -- run sh tools/build.sh first" >&2; exit 2; }
seam=$(cat build/obj/SEAM 2>/dev/null || echo "")
listing=${TMPDIR:-/tmp}/bsaudit-listing.$$
collect build/obj > "$listing"
audit "$listing" tests/audit/allow.txt "$seam"
rm -f "$listing"
[ "$fail" = 0 ] && echo "bsaudit: the seam is as narrow as tests/audit/allow.txt says"
exit $fail
