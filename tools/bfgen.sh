#!/bin/sh
# bfgen.sh — expand a .poke skeleton into committed brainfuck.
#
# IT KNOWS NOTHING ABOUT THE ABI. No op names, no length computation, no frame
# construction, and no path into ABI.md. Every hex byte in every skeleton is a
# number a person read out of the specification and typed. That is the whole
# design: if a fixture built its request with the same encoder the broker
# parses with, a byte order bug would be invisible to every test in the suite.
#
# So this mechanises the DRUDGERY and not the KNOWLEDGE. Four directives:
#
#   EMIT <hex> ...   write these literal bytes with '.'
#   READ n           read n bytes with ',' and discard them
#   LOOP / END       '[' and ']'
#   anything else beginning with '#' is a comment and is dropped
#
# Raw brainfuck passes through untouched, so a fixture can always drop to the
# eight instructions when a directive would be a lie.
#
# WHY AN EXPANDER AT ALL, given the sibling deleted its transpiler. Emitting
# 0x8f means typing 143 '+' characters and counting them right; miscount by
# one and you have sent a WRONG BYTE, which at the far end is indistinguishable
# from a broker bug. That is not a hypothetical: writing tests/vec/frames.txt
# by hand, one of six frames came out with a spurious 00 in it. The committed
# .bf also cannot carry comments -- they are not portable -- so a hand written
# one is the single artifact form here with no review path at all.
#
# This is not a compiler. It chooses no layout, computes no offset from a
# name, and generates no loop. It is closer to the sibling's Rn/Ln expansion
# than to the transpiler that was deleted, and CONVENTIONS section 6 says so.
#
# Usage:  sh tools/bfgen.sh FILE.poke > FILE.bf
set -eu

[ $# -eq 1 ] || { echo "usage: sh tools/bfgen.sh FILE.poke" >&2; exit 2; }
[ -r "$1" ]  || { echo "bfgen: cannot read $1" >&2; exit 2; }

awk '
function rep(s, n,    _i, _o) { _o = ""; for (_i = 0; _i < n; _i++) _o = _o s; return _o }

# Hex to number, by hand. strtonum() is a gawk extension: Git Bash has gawk
# and happily ran the first draft, the container has mawk and did not, and
# FreeBSD base awk has neither. One line of convenience that would have
# failed on the PRIMARY platform, found only because the container disagreed
# with the development host. Worth the six lines.
function hexv(s,    _i, _c, _d, _v) {
    _v = 0
    for (_i = 1; _i <= length(s); _i++) {
        _c = tolower(substr(s, _i, 1))
        _d = index("0123456789abcdef", _c) - 1
        if (_d < 0) return -1
        _v = _v * 16 + _d
    }
    return _v
}

# Emit one byte as a delta from whatever the cell currently holds. Going up
# or down, whichever is shorter around the 256 wrap -- a byte is never more
# than 128 steps from any other, so no literal costs more than that.
#
# cur == -1 means the cell holds something the expander cannot know: a READ
# overwrote it, or raw brainfuck went past. Then the delta is meaningless and
# the cell must be CLEARED first. Getting this wrong is the worst failure
# this script has, because it does not produce an error -- it produces a
# program that emits the wrong bytes, which at the far end looks exactly like
# a broker bug. The first draft had it wrong.
function put(v,    d) {
    if (cur < 0) { printf "[-]"; cur = 0 }
    d = v - cur
    if (d < 0) d += 256
    if (d <= 128) printf "%s", rep("+", d)
    else          printf "%s", rep("-", 256 - d)
    printf "."
    cur = v
}

BEGIN { cur = 0 }

{
    line = $0
    sub(/[ \t]+$/, "", line)
    if (line ~ /^[ \t]*#/ || line ~ /^[ \t]*$/) next

    n = split(line, f, /[ \t]+/)
    # awk splits leading whitespace into an empty first field on some awks
    start = (f[1] == "") ? 2 : 1
    verb = f[start]

    if (verb == "EMIT") {
        for (i = start + 1; i <= n; i++) {
            if (f[i] == "") continue
            if (f[i] !~ /^[0-9a-fA-F][0-9a-fA-F]$/) {
                printf "bfgen: not a hex byte: %s\n", f[i] > "/dev/stderr"; bad = 1; exit 2
            }
            put(hexv(f[i]))
        }
        printf "\n"
        next
    }
    if (verb == "READ") {
        if (f[start+1] !~ /^[0-9]+$/) {
            printf "bfgen: READ needs a count\n" > "/dev/stderr"; bad = 1; exit 2
        }
        printf "%s\n", rep(",", f[start+1] + 0)
        # a read lands in the current cell and overwrites it, so whatever the
        # expander thought was there is no longer true
        cur = -1
        next
    }
    if (verb == "LOOP") { printf "[\n"; next }
    if (verb == "END")  { printf "]\n"; next }

    # raw brainfuck passes through; anything else is a typo worth refusing
    if (line ~ /^[ \t]*[><+.,\[\]-]+[ \t]*$/) {
        gsub(/[ \t]/, "", line)
        printf "%s\n", line
        cur = -1
        next
    }
    printf "bfgen: not a directive and not brainfuck: %s\n", line > "/dev/stderr"
    bad = 1
    exit 2
}
END { if (bad) exit 2 }
' "$1"
