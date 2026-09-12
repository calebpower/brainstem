#!/bin/sh
# tests/run.sh — the whole brainstem suite. This is what `reaper test` gates on.
#
# The gate is TWO sessions, freebsd-15.1 and ubuntu-26.04, and a change is not
# judged until both are green. The summary line names the platform it ran on,
# because a suite log pasted into a commit body with no indication of which
# kernel produced it will be read as though it were the whole gate.
#
# Tiers exercised here (CONVENTIONS section 8). At M0 the broker does not exist
# yet, so what is proved is the ground it will stand on:
#   1  interpreter self-test   the oracle of record must itself be sound,
#                              in all three EOF modes
#   10c the tables agree       the toolchain has one definition, the build has
#                              one definition, and the guests named in
#                              .reaper.toml are the ones guest-setup.sh knows
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/.." && pwd)
cd "$repo"

pass=0; fail=0
bs_out=${TMPDIR:-/tmp}/bs-run-output.$$

# Scratch, for the fixtures that need a filesystem and for the observed half
# of every trace comparison. Fixtures get a directory created FRESH per check:
# readdir would otherwise depend on whatever the last run left behind, and a
# fixture that depends on its own history is one that passes until somebody
# runs it twice.
BS_TMP=$(mktemp -d)
# The repository root, for the checks that cd into a scratch directory. Since
# the preopen model was removed, a fixture reaches the filesystem the way any
# other process does -- so the suite gives it a working DIRECTORY rather than
# a handle, and has to name the binaries absolutely from inside it.
# EVERY BROKER INVOCATION BELOW REDIRECTS ITS STDIN, and that is not tidiness.
# Since the three standard handles replaced preopens, the hello reply reports
# what the broker's own stdin, stdout and stderr actually ARE -- a terminal, a
# file, a pipe -- and their rights. That makes the reply depend on how the
# suite itself was started, which a pinned trace cannot survive. Naming
# /dev/null makes every check independent of whoever ran it.
BS_R=$repo
export BS_TMP BS_R

# Two masks, and both are named here rather than left for somebody to reverse
# engineer out of a regex.
#
# The hello reply is found by its MAGIC rather than by its length, because its
# length now depends on how many preopens the run was given. The first version
# matched len=48 and would have silently stopped matching anything at all the
# moment a fixture took a preopen -- a mask that matches nothing does not
# fail, it stops asking, which is the failure mode this whole tier fears.
#
# The THIRD mask is the handle table: thirty six bytes describing what the
# broker's own stdin, stdout and stderr actually are. Those kinds and rights
# are a property of HOW THE BROKER WAS INVOKED, not of the program or the
# protocol, so pinning them made every trace fail on a guest whose stdio was
# wired up differently. The name tail after it is deterministic and stays
# pinned, and the handles themselves are checked on their own below.
#
# The second mask is stat's mtime, twelve bytes of it. A file's modification
# time is not a property of the program and cannot be pinned; everything else
# in the 32 byte record is. stat is the only 32 byte reply the pinned fixtures
# produce, and the vacuity check below is what keeps both masks honest.
BS_NORM='s/^\(brainstem: < 00 len=[0-9]* 4253544d.\{32\}\)../\1%%/;s/^\(brainstem: < 00 len=32 .\{24\}\).\{24\}/\1MMMMMMMMMMMMMMMMMMMMMMMM/;s/^\(brainstem: < 00 len=[0-9]* 4253544d.\{88\}\).\{72\}/\1HHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHH/'
export BS_NORM
trap 'rm -rf "$BS_TMP"; rm -f "$bs_out"' EXIT

# ON FAILURE, SAY WHAT THE CHECK SAID.
#
# The first version of this threw the output away in both directions, and the
# first time it mattered it cost a round trip to a machine this host cannot
# reach: two tiers failed on freebsd-15.1, both of them had printed the exact
# expected-against-observed diff that would have identified the cause, and the
# suite discarded it and reported two lines of "FAIL".
#
# A gate that runs somewhere you cannot log in has to carry its own
# diagnosis. Output is still hidden on success, because 132 passing checks
# that each print a paragraph is a log nobody reads.
#
# BS_ONLY narrows the suite to the checks whose label contains one of the
# patterns in it, and BS_NOBUILD skips the build. The patterns are separated
# by "|" and not by spaces, because a check label is a sentence: a space
# separated BS_ONLY of "the handshake round trips" matched seventy six checks
# on the word "the", which is a filter that reports a pass having asked
# nothing. Both exist
# for tools/bsmut.sh, which runs this file once per mutation and needs the
# ONE check that mutation is supposed to break -- thirty one full suite runs
# would be most of tier 11's cost and none of its meaning.
#
# The failure mode of BS_ONLY is the safe one: a pattern that matches nothing
# runs no checks, the suite passes with a count of zero, and bsmut reports the
# mutant as having SURVIVED. A filter that quietly matched everything would
# have been the dangerous direction.
run() {  # run LABEL CMD...
    label="$1"; shift
    if [ -n "${BS_ONLY:-}" ]; then
        bs_keep=0
        bs_ifs=$IFS; IFS='|'
        for bs_pat in $BS_ONLY; do
            case "$label" in *"$bs_pat"*) bs_keep=1 ;; esac
        done
        IFS=$bs_ifs
        [ "$bs_keep" = 1 ] || return 0
    fi
    if "$@" > "$bs_out" 2>&1; then
        echo "PASS $label"; pass=$((pass+1))
    else
        echo "FAIL $label"; fail=$((fail+1))
        sed 's/^/     | /' "$bs_out"
    fi
}

echo "== build =="
# tools/build.sh is the only thing in this repository that invokes a compiler,
# and the check below proves it stays that way.
if [ -z "${BS_NOBUILD:-}" ]; then sh tools/build.sh >/dev/null; else echo "(BS_NOBUILD: using the binaries already here)"; fi
bfi=$repo/build/bfi
echo "built"

# TIER 0
echo
echo "== tier 0: checker self-tests =="
# Before any real file is examined, per CONVENTIONS section 8. A checker that
# has never been observed failing is indistinguishable from a clean corpus.
run "bscodec self-test" ./build/bscodec --selftest
run "bsframe self-test" ./build/bsframe --selftest
run "bstier self-test" sh tools/bstier.sh --selftest
run "bsbf self-test" ./build/bsbf --selftest
run "brainstem self-test" ./build/brainstem --selftest
run "bsaudit self-test" sh tools/bsaudit.sh --selftest
run "bscalls self-test" sh tools/bscalls.sh --selftest
run "bspoke self-test" sh tools/bspoke.sh --selftest
run "bsmut self-test" sh tools/bsmut.sh --selftest

# TIER 10c
echo
echo "== tier 10c: one definition of everything =="

# The Containerfile must carry no toolchain definition of its own. bfsodium's
# comment is the one to remember: a second definition is how the fallback lane
# starts passing what the gate would fail, silently, because a container with
# a different compiler still runs every test and still says PASS.
#
# Comment lines are stripped before the search, and that is not a loophole --
# it is the difference between checking what the file DOES and checking what
# it SAYS. The first version of this check read the whole file and failed on
# its own Containerfile, because the comment explaining that there is
# deliberately no apt-get line contains the words "apt-get line". A check that
# forbids a token and then trips over the prose explaining the ban teaches the
# next person to delete the prose, which is the opposite of what is wanted.
run "the container lane installs nothing of its own" sh -c '
    directives=$(grep -v "^[[:space:]]*#" Containerfile)
    printf "%s" "$directives" | grep -q "guest-setup.sh --toolchain" || exit 1
    ! printf "%s" "$directives" | grep -Eq "apt-get|apt install|pkg install|yum|dnf|apk add"'

# One definition of the BUILD, not just of the toolchain. This is the check
# bfsodium does not have and arguably needs: it carries five cc lines in its
# suite and five more in its guest-setup, which is exactly the shape of defect
# its Containerfile check exists to forbid, one level down.
run "tools/build.sh is the only thing that compiles" sh -c '
    got=$(grep -rlE "(^|[^A-Za-z_])(cc|gcc|clang|\\$CC)[[:space:]]+.*-o[[:space:]]" \
              --include="*.sh" --include="*.toml" --include="Containerfile" . \
          2>/dev/null | grep -v "^\\./tools/build\\.sh$" | grep -v "^\\./\\.git/")
    test -z "$got"'

# The guests reaper will run, and the platforms guest-setup.sh knows how to
# provision, must be the same set. See the GUEST markers in guest-setup.sh for
# why the correspondence is declared rather than inferred.
run "the declared guests are exactly the ones guest-setup knows" sh -c '
    declared=$(sed -n "s/^# GUEST \([^ ]*\) .*/\1/p" tools/guest-setup.sh | sort)
    tenant=$(sed -n "s/^guests *= *\[\(.*\)\].*/\1/p" .reaper.toml \
             | tr -d "\" " | tr "," "\n" | grep . | sort)
    test "$declared" = "$tenant"'

run "every platform guest-setup declares has a branch" sh -c '
    rc=0
    for os in $(sed -n "s/^# GUEST [^ ]* \(.*\)/\1/p" tools/guest-setup.sh); do
        grep -q "^        $os)" tools/guest-setup.sh || rc=1
    done
    exit $rc'

# CONVENTIONS section 8 says which tiers are REQUIRED; HANDOFF says which are
# BUILT. Keeping those in one column is what let the sibling project claim
# fuzz coverage it did not have, so here they are two tables and a tool reads
# both against the suite.
run "HANDOFF's tier table describes the suite" sh tools/bstier.sh

# THE FROZEN ABI VERSION, IN THREE PLACES, WHICH MUST BE ONE NUMBER.
#
# ABI.md is frozen at 1.0 as of M7 (its section 0), and a frozen specification
# nobody compares to anything is just an old specification. The version in the
# document's title, the constants the broker is compiled with, and the two u16
# fields the broker actually PUTS ON THE WIRE in its hello reply are checked
# against each other here.
#
# The third of those is the one that matters. The first two are both source
# files a person edits; only the wire says what a client would really see, and
# it is read out of a real conversation rather than out of a header.
run "the ABI version is one number in three places, including the wire" sh -c '
    doc=$(sed -n "s/^# brainstem ABI — version \([0-9]*\.[0-9]*\)$/\1/p" ABI.md)
    test -n "$doc" || { echo "ABI.md has no version in its title"; exit 1; }
    maj=$(sed -n "s/^#define BS_VER_MAJOR \([0-9]*\)$/\1/p" src/brainstem.h)
    min=$(sed -n "s/^#define BS_VER_MINOR \([0-9]*\)$/\1/p" src/brainstem.h)
    test "$doc" = "$maj.$min" || { echo "ABI.md says $doc, brainstem.h says $maj.$min"; exit 1; }
    # the hello record: magic{4} then major{u16 LE} then minor{u16 LE}
    hex=$(./build/brainstem --trace -- ./build/bfi bf/ctl/hello.bf </dev/null 2>&1 >/dev/null \
          | sed -n "s/^brainstem: < 00 len=[0-9]* 4253544d\(........\).*/\1/p" | head -1)
    want=$(printf "%02x00%02x00" "$maj" "$min")
    test "$hex" = "$want" || { echo "the wire says $hex, the headers say $want"; exit 1; }
    exit 0'

# The user documentation makes two claims a reader will act on, and both are
# checkable, so both are checked. A guide that documents a flag the binary does
# not have costs someone an afternoon deciding their quoting is wrong, and a
# worked example that has drifted from the fixture it was copied from teaches a
# frame layout that no longer exists.
run "GUIDE's option table and the argument parser agree" sh -c '
    # grep -o, not sed: three of these options share one line in the
    # parser, and a line oriented substitution keeps only the last match
    # on it. The first version of this check silently believed
    # --preopen-dir did not exist.
    parsed=$(grep -o "strcmp(a, \"--[a-z][a-z-]*\")" src/main.c | grep -o -- "--[a-z][a-z-]*" | sort -u)
    documented=$(sed -n "s/^| \`\(--[a-z][a-z-]*\)[^|]*| now |.*/\1/p" GUIDE.md | sort -u)
    if [ "$parsed" != "$documented" ]; then
        echo "the parser accepts:"; echo "$parsed"
        echo "GUIDE marks as built:"; echo "$documented"
        exit 1
    fi
    exit 0'
run "no option GUIDE defers to a later milestone is quietly already there" sh -c '
    # grep -o, not sed: three of these options share one line in the
    # parser, and a line oriented substitution keeps only the last match
    # on it. The first version of this check silently believed
    # --preopen-dir did not exist.
    parsed=$(grep -o "strcmp(a, \"--[a-z][a-z-]*\")" src/main.c | grep -o -- "--[a-z][a-z-]*" | sort -u)
    rc=0
    for o in $(sed -n "s/^| \`\(--[a-z][a-z-]*\)[^|]*| M[0-9] |.*/\1/p" GUIDE.md); do
        printf "%s\n" "$parsed" | grep -qx "$o" && { echo "$o is deferred in GUIDE but parsed today"; rc=1; }
    done
    exit $rc'
run "GUIDE's worked programs are the fixtures the suite runs" sh -c '
    rc=0
    for f in bf/ctl/hello.poke bf/time/clock.poke bf/rand/bytes.poke; do
        awk -v f="$f" "
            \$0 == \"<!-- \" f \" -->\" { want = 1; next }
            want && /^\`\`\`\$/ { inblock = !inblock; if (!inblock) { want = 0 }; next }
            inblock { print }
        " GUIDE.md > /tmp/guide-block.$$
        cmp -s /tmp/guide-block.$$ "$f" || { echo "GUIDE has drifted from $f"; rc=1; }
        rm -f /tmp/guide-block.$$
    done
    exit $rc'
run "the handshake GUIDE tells you to write is the one the fixture writes" sh -c '
    line=$(grep -m1 "^EMIT 01" bf/ctl/hello.poke)
    grep -q "\`$line\`" GUIDE.md'

# The vendored interpreter carries one intentional delta from upstream and the
# whole project rests on it. Someone tidying the vendored file back toward its
# source would reintroduce a deadlock whose only symptom is a hang, so the
# delta is pinned here rather than trusted to a comment.
run "the vendored interpreter still has the unbuffering fix" \
    grep -q "setvbuf(stdout, NULL, _IONBF, 0)" tools/bfi.c

# TIER 1
echo
echo "== tier 1: interpreter self-test =="
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

# The expected output goes in a FILE, and that is not stylistic. The first
# version of this piped the interpreter into `cmp -s - /dev/stdin` with the
# expectation in a heredoc -- at which point the heredoc IS cmp's stdin, both
# `-` and /dev/stdin name it, cmp compares it with itself, and the
# interpreter's output is discarded unexamined. It passed for a program that
# printed nothing. Mutation checking caught it; nothing else would have.
printf '%s' '++++++++[>++++[>++>+++>+++>+<<<<-]>+>+>->>+[<]<-]>>.>---.+++++++..+++.>>.<-.<.+++.------.--------.>>+.>++.' > "$tmp/hello.bf"
printf 'Hello World!\n' > "$tmp/hello.want"
run "hello world" sh -c "
    \"$bfi\" \"$tmp/hello.bf\" </dev/null | cmp -s - \"$tmp/hello.want\""

# Binary transparency. A protocol carries 0x00 and 0x0a as ordinary payload,
# so an interpreter that translates either is unusable here -- and the failure
# would look like a broker bug.
printf '%s' ',.,.,.' > "$tmp/echo3.bf"
run "binary echo (NUL and high byte)" sh -c "
    printf '00ff41' | \"$repo/build/hx\" -r | \"$bfi\" \"$tmp/echo3.bf\" \
    | \"$repo/build/hx\" | grep -qx 00ff41"

printf '%s' ',.,.,.' > "$tmp/echolf.bf"
run "binary echo (0x0a survives both directions)" sh -c "
    printf '0d0a1a' | \"$repo/build/hx\" -r | \"$bfi\" \"$tmp/echolf.bf\" \
    | \"$repo/build/hx\" | grep -qx 0d0a1a"

printf '%s' '-.' > "$tmp/wrap.bf"
run "cell wraps 0 to 255" sh -c "
    \"$bfi\" \"$tmp/wrap.bf\" </dev/null | \"$repo/build/hx\" | grep -qx ff"

printf '%s' '<' > "$tmp/left.bf"
run "pointer left of cell 0 is a hard error" sh -c "
    \"$bfi\" \"$tmp/left.bf\" </dev/null >/dev/null 2>&1; test \$? -eq 3"

printf '%s' '; this , and . and - are prose
+.' > "$tmp/cmt.bf"
run "semicolon comments are inert" sh -c "
    \"$bfi\" \"$tmp/cmt.bf\" </dev/null | \"$repo/build/hx\" | grep -qx 01"

# Nested loops and tape growth, because every fixture will read a length and
# then count down through a payload, which is exactly this shape.
printf '%s' '++++[>++++[>++++<-]<-]>>.' > "$tmp/nest.bf"
run "nested loops" sh -c "
    \"$bfi\" \"$tmp/nest.bf\" </dev/null | \"$repo/build/hx\" | grep -qx 40"

printf '%s' '++++++++[>++++++++<-]>[>+>+<<-]>>[<<+>>-]<<' > "$tmp/grow.bf"
run "tape grows without error" "$bfi" "$tmp/grow.bf"

echo
echo "== tier 1: the three EOF conventions =="
# Brainfuck does not specify what ',' does at end of input, and real
# interpreters disagree three ways. brainstem's claim is that a fixture works
# under ANY conforming interpreter, so the suite has to be able to BE all
# three. This is the knob that makes that possible; it is checked here so a
# later tier can rely on it.
printf '%s' ',.' > "$tmp/eof.bf"
run "BFI_EOF unset leaves the cell unchanged (upstream behaviour)" sh -c "
    \"$bfi\" \"$tmp/eof.bf\" </dev/null | \"$repo/build/hx\" | grep -qx 00"
run "BFI_EOF=unchanged leaves the cell unchanged" sh -c "
    BFI_EOF=unchanged \"$bfi\" \"$tmp/eof.bf\" </dev/null | \"$repo/build/hx\" | grep -qx 00"
run "BFI_EOF=zero stores nought" sh -c "
    BFI_EOF=zero \"$bfi\" \"$tmp/eof.bf\" </dev/null | \"$repo/build/hx\" | grep -qx 00"
run "BFI_EOF=minus1 stores 255" sh -c "
    BFI_EOF=minus1 \"$bfi\" \"$tmp/eof.bf\" </dev/null | \"$repo/build/hx\" | grep -qx ff"
run "an unknown BFI_EOF is refused rather than guessed" sh -c "
    BFI_EOF=wat \"$bfi\" \"$tmp/eof.bf\" </dev/null >/dev/null 2>&1; test \$? -eq 2"

# The cell-unchanged case above starts from a cleared cell, so it cannot tell
# 'unchanged' from 'stored nought'. This one starts from 1.
printf '%s' '+,.' > "$tmp/eof1.bf"
run "unchanged and zero are actually distinguishable" sh -c "
    u=\$(BFI_EOF=unchanged \"$bfi\" \"$tmp/eof1.bf\" </dev/null | \"$repo/build/hx\")
    z=\$(BFI_EOF=zero      \"$bfi\" \"$tmp/eof1.bf\" </dev/null | \"$repo/build/hx\")
    test \"\$u\" = 01 && test \"\$z\" = 00"

echo
echo "== tier 1: output buffering, both ways =="
# The liveness proof proper needs the broker and arrives with it; what is
# checked here is that the knob exists and that neither setting corrupts the
# bytes. BFI_FLUSH=block puts the upstream deadlock back on purpose, because a
# deadlock the suite cannot reproduce is a deadlock that comes back.
run "BFI_FLUSH=byte round trips" sh -c "
    printf '00ff41' | \"$repo/build/hx\" -r \
    | BFI_FLUSH=byte \"$bfi\" \"$tmp/echo3.bf\" | \"$repo/build/hx\" | grep -qx 00ff41"
run "BFI_FLUSH=block round trips (the bytes still arrive, just late)" sh -c "
    printf '00ff41' | \"$repo/build/hx\" -r \
    | BFI_FLUSH=block \"$bfi\" \"$tmp/echo3.bf\" | \"$repo/build/hx\" | grep -qx 00ff41"
run "an unknown BFI_FLUSH is refused rather than guessed" sh -c "
    BFI_FLUSH=wat \"$bfi\" \"$tmp/echo3.bf\" </dev/null >/dev/null 2>&1; test \$? -eq 2"

# TIER 4
echo
echo "== tier 4: the frame codec, in isolation =="
# No process, no descriptor, no kernel. A failure here cannot be a pipe
# problem, which is the entire reason this tier runs before anything that
# opens one.
#
# Every case is driven through BOTH implementations: build/bscodec, which is
# a skin over src/frame.c, and build/bsframe, which was written from ABI.md
# with no shared code. The pinned frames in tests/vec/frames.txt were derived
# by hand from the specification. So each assertion has two oracles behind it
# -- the document, and a second reading of the document -- and a byte order
# bug would have to be made twice, in two directions, to survive.
while read -r _v_type _v_a _v_b _v_c; do
    case "$_v_type" in
        ''|'#'*) continue ;;
    esac
    if [ "$_v_type" = enc ]; then
        _v_pay=$_v_b
        [ "$_v_pay" = "-" ] && _v_pay=""
        _v_len=$(( ${#_v_pay} / 2 ))
        for _v_impl in bscodec bsframe; do
            run "$_v_impl encodes op $_v_a len $_v_len" sh -c "
                test \"\$(./build/$_v_impl encode '$_v_a' '$_v_pay')\" = '$_v_c'"
            run "$_v_impl decodes op $_v_a len $_v_len" sh -c "
                test \"\$(./build/$_v_impl decode '$_v_c')\" = 'kind=$_v_a len=$_v_len payload=$_v_pay'"
        done
    elif [ "$_v_type" = err ]; then
        for _v_impl in bscodec bsframe; do
            run "$_v_impl refuses $_v_a with $_v_b" sh -c "
                test \"\$(./build/$_v_impl decode '$_v_a')\" = 'ERR $_v_b'"
        done
    fi
done < tests/vec/frames.txt

# The cases that are too long to write out by hand, checked differentially:
# the two implementations must agree, and the header must carry the length
# little end first. 256 is the byte boundary, where a big endian
# implementation would put 01 00 and a little endian one 00 01.
run "the length crosses the byte boundary little end first" sh -c '
    pay=$(awk "BEGIN{ for (i=0;i<256;i++) printf \"41\" }")
    a=$(./build/bscodec encode 06 "$pay")
    b=$(./build/bsframe encode 06 "$pay")
    test "$a" = "$b" || { echo "the two implementations disagree"; exit 1; }
    # header is 06 then 00 01 for 256, not 01 00
    test "$(printf %s "$a" | cut -c1-6)" = "060001"'

run "a maximum length payload round trips through both" sh -c '
    pay=$(awk "BEGIN{ for (i=0;i<65535;i++) printf \"5a\" }")
    a=$(./build/bscodec encode 07 "$pay")
    b=$(./build/bsframe encode 07 "$pay")
    test "$a" = "$b" || { echo "the two implementations disagree at the maximum"; exit 1; }
    test "$(printf %s "$a" | cut -c1-6)" = "07ffff"
    # The length comes from the shell rather than from wc, and that is not a
    # style choice. BSD wc right aligns its output with leading spaces --
    # "  131076" -- while GNU coreutils does not, and command substitution
    # strips trailing newlines but NOT leading spaces. So the obvious
    # spelling of this check passes on Linux and fails on FreeBSD, which is
    # exactly what it did. ${#a} has no such opinion.
    test "${#a}" = "$(( (3 + 65535) * 2 ))"'

run "the two implementations agree on every single byte value" sh -c '
    rc=0
    for v in 00 01 0a 0d 1a 20 7f 80 ff; do
        a=$(./build/bscodec encode 08 "$v")
        b=$(./build/bsframe encode 08 "$v")
        [ "$a" = "$b" ] || { echo "disagree on $v: $a vs $b"; rc=1; }
    done
    exit $rc'

# TIER 2
echo
echo "== tier 2: the program is still brainfuck =="
# The project's central claim, mechanised. No other tier can see it: every
# other check asks what a fixture ACHIEVES, and one that achieved it with a
# dialect extension would pass all of them while being unrunnable anywhere
# but here.
for f in bf/*/*.bf; do run "pure brainfuck $f" ./build/bsbf "$f"; done

# TIER 3
echo
echo "== tier 3: the committed brainfuck is what its skeleton says =="
# Paired with tier 2, this is what licenses the .poke to be the review
# artifact: the .bf carries no prose, so if it were not provably the
# expansion of something readable, nothing would be reviewable at all.
for s in bf/*/*.poke; do
    run "regenerates ${s%.poke}.bf" sh -c "sh tools/bfgen.sh '$s' | cmp -s - '${s%.poke}.bf'"
done

# TIER 3a
echo
echo "== tier 3a: a fixture can be reviewed =="
# A .bf carries no comments by design, so the skeleton is the only place
# review can happen, and a skeleton with no header is a fixture nobody can
# check the intent of.
run "every skeleton has a header naming what it does" sh -c '
    rc=0
    for s in bf/*/*.poke; do
        head -1 "$s" | grep -q "^# " || { echo "$s has no header line"; rc=1; }
    done
    exit $rc'

# A READ RETURNS UP TO n BYTES, so a fixture that asks a pipe or a socket for
# more than one and then reads a reply sized for more than one is a race. The
# child in bf/proc is an interpreter with unbuffered output: it emits each
# byte with its own write, and whether two of them are in the pipe when the
# read lands is a scheduling question. On an idle eight core development host
# the answer was always "both", through forty runs under deliberate CPU load.
# On a loaded single processor guest it is not.
#
# Everything in bf/proc, bf/net and bf/io reads from a stream, so the rule
# here has no exceptions and needs to know nothing about which handle is
# which: every read op in those three directories asks for exactly one byte. A
# program that wants more has to loop, which is what talking to a stream means
# anyway.
run "no fixture depends on a stream read returning more than one byte" sh -c '
    bad=$(grep -h "^EMIT 0a " bf/proc/*.poke bf/net/*.poke bf/io/*.poke \
          | grep -vE "^EMIT 0a 08 00( [0-9a-f]{2}){4} 01 00 ") || true
    if [ -n "$bad" ]; then echo "$bad"; exit 1; fi
    exit 0'

# The expander must stay ignorant of the ABI. This is the line between an
# expander and a compiler, and it is the same shape of check as the one
# guarding the lane definitions: if bfgen knew an op name or a length, the
# fixtures would be generated from the same knowledge the broker is built
# from, and a byte order bug would be invisible to the whole suite.
run "the expander knows no opcode and no op name" sh -c '
    body=$(grep -v "^[[:space:]]*#" tools/bfgen.sh)
    ! printf "%s" "$body" | grep -Eqi "hello|ABI\.md|opcode|0x0[1-9]|BSTM"'

# TIER 3b
echo
echo "== tier 3b: the header does not lie =="
# Declared at M2 and missing until M7, and the gap was a real one: tier 3
# proves a .bf is the expansion of its skeleton and tier 2 proves it is
# brainfuck, but NOTHING could see whether the skeleton's prose described its
# hex. A header saying "open" over a frame that stats would have passed every
# check in this file, and the prose is the only artifact a reviewer has.
#
# Six rules, and tools/bspoke.sh names each one it checked. Two of them (P5
# and P6) are pins on this TREE rather than on a platform, because the honest
# alternative was an exemption, and an exemption keeps passing after the thing
# it excused has changed.
run "every fixture comment describes the frame beneath it" sh tools/bspoke.sh

# TIER 5
echo
echo "== tier 5: a standard brainfuck program completes a round trip =="
# THE milestone. A file containing nothing but the eight instructions, run
# under a general purpose interpreter, reaching an operating system.
run "the handshake round trips and the program exits 0" sh -c '
    ./build/brainstem -- ./build/bfi bf/ctl/hello.bf >/dev/null </dev/null 2>&1'
run "the trace shows exactly the four frames" sh -c '
    got=$(./build/brainstem --trace -- ./build/bfi bf/ctl/hello.bf </dev/null 2>&1 | grep -c "brainstem: [<>]")
    test "$got" = 4'
run "the program chooses the exit status" sh -c '
    ./build/brainstem -- ./build/bfi bf/ctl/exitcode.bf >/dev/null </dev/null 2>&1; test $? -eq 1'

# The two ops that arrived with the seam. clock_now and random_bytes were
# chosen to be first because arc4random_buf against getrandom is a REAL
# divergence -- so the seam took its shape from one rather than from a guess
# about what might diverge later.
run "clock_now answers on both clocks" sh -c '
    ./build/brainstem --clock frozen=1700000000 -- ./build/bfi bf/time/clock.bf >/dev/null </dev/null 2>&1'
run "random_bytes returns as many bytes as it was asked for" sh -c '
    ./build/brainstem --trace -- ./build/bfi bf/rand/bytes.bf </dev/null 2>&1 >/dev/null | grep -q "< 00 len=16 "'
run "random_bytes of zero is a legal empty reply, not an error" sh -c '
    got=$(./build/brainstem --trace -- ./build/bfi bf/rand/bytes.bf </dev/null 2>&1 >/dev/null | grep -c "^brainstem: < 00 len=0$")
    test "$got" = 2'


# The eleven M4 ops, in one conversation each. Both fixtures run against a
# directory created fresh here, which is what makes their traces a fixed
# string of bytes: readdir would otherwise depend on whatever the last run
# left behind, and a fixture that depends on its own history is one that
# passes until somebody runs it twice.
run "the whole filesystem round trip completes" sh -c '
    rm -rf "$BS_TMP/work" && mkdir -p "$BS_TMP/work"
    (cd "$BS_TMP/work" && "$BS_R/build/brainstem" -- "$BS_R/build/bfi" "$BS_R/bf/fs/roundtrip.bf" </dev/null) >/dev/null 2>&1'
run "it really wrote the bytes, and really moved and removed the file" sh -c '
    rm -rf "$BS_TMP/work" && mkdir -p "$BS_TMP/work"
    (cd "$BS_TMP/work" && "$BS_R/build/brainstem" -- "$BS_R/build/bfi" "$BS_R/bf/fs/roundtrip.bf" </dev/null) >/dev/null 2>&1
    test -d "$BS_TMP/work/sub" || { echo "mkdir did not happen"; exit 1; }
    test ! -e "$BS_TMP/work/f" || { echo "rename left the old name behind"; exit 1; }
    test ! -e "$BS_TMP/work/g" || { echo "unlink did not happen"; exit 1; }
    exit 0'
# The reply to read must be the bytes write was given, not merely the right
# LENGTH of bytes -- a broker that echoed zeros would pass a length check.
run "read returns what write was given" sh -c '
    rm -rf "$BS_TMP/work" && mkdir -p "$BS_TMP/work"
    (cd "$BS_TMP/work" && "$BS_R/build/brainstem" --trace -- "$BS_R/build/bfi" "$BS_R/bf/fs/roundtrip.bf" </dev/null) 2>&1 >/dev/null \
        | grep -q "< 00 len=2 6869"'
run "a reused slot comes back with a new generation each time" sh -c '
    rm -rf "$BS_TMP/work" && mkdir -p "$BS_TMP/work"
    got=$( (cd "$BS_TMP/work" && "$BS_R/build/brainstem" --trace -- "$BS_R/build/bfi" "$BS_R/bf/fs/roundtrip.bf" </dev/null) 2>&1 >/dev/null \
          | sed -n "s/^brainstem: < 00 len=4 //p" | tr "\n" " ")
    test "$got" = "04000000 04000100 04000200 "'
run "readdir yields one entry then ends" sh -c '
    rm -rf "$BS_TMP/work" && mkdir -p "$BS_TMP/work"
    out=$( (cd "$BS_TMP/work" && "$BS_R/build/brainstem" --trace -- "$BS_R/build/bfi" "$BS_R/bf/fs/roundtrip.bf" </dev/null) 2>&1 >/dev/null)
    printf "%s\n" "$out" | grep -q "< 00 len=7 02030000737562" || { echo "the sub entry is wrong"; exit 1; }
    printf "%s\n" "$out" | grep -q "^brainstem: < 01 len=0$"  || { echo "the walk did not end"; exit 1; }
    exit 0'
# THE DIRECTORY WALK, which is the reason --sort-readdir exists.
#
# A directory has no order. ext4 returns entries in hash order, ufs in
# roughly creation order, and neither is a property of the program -- so
# until now every fixture here walked a directory with exactly ONE entry in
# it, the only size at which "whatever order the filesystem likes" and "a
# fixed order" are the same thing.
#
# The entries are created d, b, c, a, which is deliberate: on a filesystem
# that enumerates in creation order -- which is the PRIMARY platform -- an
# unsorted walk returns them in that order and every check below goes red.
# BS_WALK is a command rather than a shell function ON PURPOSE: every check
# below runs under `sh -c`, which is a child shell, and a child shell does not
# inherit functions. Exporting them is a bashism. So the setup and the run are
# each one line of text, expanded inside the child.
BS_MKWALK='rm -rf "$BS_TMP/walk" && mkdir -p "$BS_TMP/walk" &&
    cd "$BS_TMP/walk" && : > d && : > b && mkdir c && : > a'
BS_WALK='"$BS_R/build/brainstem" --sort-readdir --trace \
    -- "$BS_R/build/bfi" "$BS_R/bf/fs/walk.bf" </dev/null 2>&1 >/dev/null'
export BS_MKWALK BS_WALK

run "a sorted directory walk yields every entry, with its kind" sh -c '
    eval "$BS_MKWALK"
    got=$(eval "$BS_WALK" | sed -n "s/^brainstem: < 00 len=5 \(..\)..0000\(..\)$/\1\2/p" \
          | tr "\n" " ")
    test "$got" = "0161 0162 0263 0164 " || { echo "got [$got]"; exit 1; }
    exit 0'
run "and then it ends, rather than repeating the last entry" sh -c '
    eval "$BS_MKWALK"
    eval "$BS_WALK" | grep -q "^brainstem: < 01 len=0$"'

# The three handles every program starts with, named in the hello table so a
# program can check rather than assume. The hex is "stdin" "stdout" "stderr"
# with their length bytes, which is the tail of the record.
run "the hello table describes the three standard handles" sh -c '
    out=$(./build/brainstem --trace -- ./build/bfi bf/ctl/hello.bf </dev/null 2>&1 >/dev/null)
    printf "%s" "$out" | grep -q "05737464696e067374646f757406737464657272$" \
        || { echo "the names are not stdin, stdout, stderr"; exit 1; }
    printf "%s" "$out" | grep -q "03001400010000000" \
        || { echo "not three handles starting at 1"; exit 1; }
    printf "%s" "$out" | grep -q "02000000" \
        || { echo "handle 2 is missing"; exit 1; }
    printf "%s" "$out" | grep -q "03000000" \
        || { echo "handle 3 is missing"; exit 1; }
    exit 0'


# The five net ops, in one conversation, with no second process involved.
# bind replies with the address actually bound, the program reads the
# ephemeral port out of that reply in raw brainfuck, and emits it back in the
# connect frame -- so the fixture needs no fixed port, no helper binary and
# no coordination outside the protocol.
run "a brainfuck program connects to itself over TCP" sh -c '
    ./build/brainstem --op-timeout 5000 -- ./build/bfi bf/net/loopback.bf >/dev/null </dev/null 2>&1'
run "the two bytes arrive through the socket" sh -c '
    got=$(./build/brainstem --op-timeout 5000 --trace -- ./build/bfi bf/net/loopback.bf </dev/null 2>&1 >/dev/null \
        | sed -n "s/^brainstem: < 00 len=1 \(..\)$/\1/p" | tr -d "\n")
    test "$got" = "6869"'
# THE CHECK THAT MATTERS. bind was asked for port 0 and had to answer with a
# real one, and the program had to carry those two bytes from a reply into a
# request. If the port in the connect frame did not match the port bind
# returned, the fixture would still connect to SOMETHING or fail -- so this
# compares them rather than trusting that it worked.
run "the port the program connects to is the port bind gave it" sh -c '
    out=$(./build/brainstem --op-timeout 5000 --trace -- ./build/bfi bf/net/loopback.bf </dev/null 2>&1 >/dev/null)
    bound=$(printf "%s\n" "$out" | sed -n "s/^brainstem: < 00 len=32 ....\(....\).*/\1/p")
    used=$(printf "%s\n" "$out"  | sed -n "s/^brainstem: > 06 len=38 ................\(....\).*/\1/p")
    if [ -z "$bound" ] || [ "$bound" = "0000" ]; then echo "bind did not report a port: [$bound]"; exit 1; fi
    if [ "$bound" != "$used" ]; then echo "bind gave $bound, connect used $used"; exit 1; fi
    exit 0'
run "accept reports a handle and a 32 byte peer address" sh -c '
    ./build/brainstem --op-timeout 5000 --trace -- ./build/bfi bf/net/loopback.bf </dev/null 2>&1 >/dev/null \
        | grep -q "^brainstem: < 00 len=36 06000000"'


# THE PAYOFF. A file containing nothing but the eight brainfuck instructions
# creates two pipes, starts an interpreter on a SECOND brainfuck program with
# those pipes as its stdin and stdout, sends it two bytes, reads its answer,
# and collects its exit status.
#
# The directory is prepared here rather than by the fixture, because a
# brainfuck program cannot copy a binary -- and it is prepared FRESH each
# time, so nothing depends on what the last run left.
bs_procdir() {
    rm -rf "$BS_TMP/proc" && mkdir -p "$BS_TMP/proc"
    cp build/bfi "$BS_TMP/proc/bfi"
    cp bf/proc/echo.bf "$BS_TMP/proc/echo.bf"
}
run "brainfuck drives brainfuck" sh -c '
    rm -rf "$BS_TMP/proc" && mkdir -p "$BS_TMP/proc"
    cp build/bfi "$BS_TMP/proc/bfi" && cp bf/proc/echo.bf "$BS_TMP/proc/echo.bf"
    (cd "$BS_TMP/proc" && "$BS_R/build/brainstem" --op-timeout 5000 -- "$BS_R/build/bfi" "$BS_R/bf/proc/drive.bf" </dev/null) >/dev/null 2>&1'
# The bytes went out through one pipe, through a second brainfuck program
# running under its own interpreter, and back through another. A length check
# alone would pass on a broker that echoed zeros.
run "the bytes come back through the child" sh -c '
    rm -rf "$BS_TMP/proc" && mkdir -p "$BS_TMP/proc"
    cp build/bfi "$BS_TMP/proc/bfi" && cp bf/proc/echo.bf "$BS_TMP/proc/echo.bf"
    got=$( (cd "$BS_TMP/proc" && "$BS_R/build/brainstem" --op-timeout 5000 --trace -- "$BS_R/build/bfi" "$BS_R/bf/proc/drive.bf" </dev/null) 2>&1 >/dev/null \
        | sed -n "s/^brainstem: < 00 len=1 \(..\)$/\1/p" | tr -d "\n")
    test "$got" = "6869"'
run "the child exits 0 and wait reports it" sh -c '
    rm -rf "$BS_TMP/proc" && mkdir -p "$BS_TMP/proc"
    cp build/bfi "$BS_TMP/proc/bfi" && cp bf/proc/echo.bf "$BS_TMP/proc/echo.bf"
    (cd "$BS_TMP/proc" && "$BS_R/build/brainstem" --op-timeout 5000 --trace -- "$BS_R/build/bfi" "$BS_R/bf/proc/drive.bf" </dev/null) 2>&1 >/dev/null \
        | grep -q "^brainstem: < 00 len=4 01000000$"'
# wait is idempotent after reaping: the kernel will only report a status once,
# so the handle caches it. Two identical replies, from two identical requests.
run "wait repeats itself after the child is reaped" sh -c '
    rm -rf "$BS_TMP/proc" && mkdir -p "$BS_TMP/proc"
    cp build/bfi "$BS_TMP/proc/bfi" && cp bf/proc/echo.bf "$BS_TMP/proc/echo.bf"
    got=$( (cd "$BS_TMP/proc" && "$BS_R/build/brainstem" --op-timeout 5000 --trace -- "$BS_R/build/bfi" "$BS_R/bf/proc/drive.bf" </dev/null) 2>&1 >/dev/null \
          | grep -c "^brainstem: < 00 len=4 01000000$")
    test "$got" = 2'
# POLL, which had no fixture at all until tier 11 asked for one. Tier 5 has
# claimed "one fixture per op" since M4 and it was true of twenty two of the
# twenty three; poll was built and nothing in this tree ever sent one. A
# mutation sweep found it by having no check to name.
#
# The two replies asserted here are the whole property: a pipe with nothing
# in it is not readable, and the same pipe with a byte in it is. A poll that
# always said "ready", or always said "not ready", fails exactly one of them.
run "poll answers not-readable, then readable, on the same handle" sh -c '
    got=$(./build/brainstem --trace -- ./build/bfi bf/io/poll.bf </dev/null 2>&1 >/dev/null \
          | sed -n "s/^brainstem: < 00 len=6 //p" | head -2 | tr "\n" " ")
    test "$got" = "000000000000 010001000000 " || { echo "got [$got]"; exit 1; }
    exit 0'
# The third poll is on a pipe whose only writer is gone, and its bytes are NOT
# asserted: POLLHUP arrives on both platforms and the two kernels disagree
# about whether POLLIN comes with it. That is a fact about kernels rather than
# about the program, and a parity tier may only pin what the program
# determines. What must hold either way is that the reply is the same SIZE, so
# the conversation stays in step -- which is what the fixture running to
# completion proves.
run "and a poll on a hung-up pipe keeps the conversation in step" sh -c '
    out=$(./build/brainstem --trace -- ./build/bfi bf/io/poll.bf </dev/null 2>&1 >/dev/null)
    test "$(printf "%s\n" "$out" | grep -c "^brainstem: < 00 len=6 ")" = 3 \
        || { echo "$out"; exit 1; }
    printf "%s\n" "$out" | tail -1 | grep -q "^brainstem: < 00 len=0$" \
        || { echo "the program did not reach its exit frame"; exit 1; }
    exit 0'
run "pipe yields a read end and a write end, in that order" sh -c '
    rm -rf "$BS_TMP/proc" && mkdir -p "$BS_TMP/proc"
    cp build/bfi "$BS_TMP/proc/bfi" && cp bf/proc/echo.bf "$BS_TMP/proc/echo.bf"
    (cd "$BS_TMP/proc" && "$BS_R/build/brainstem" --op-timeout 5000 --trace -- "$BS_R/build/bfi" "$BS_R/bf/proc/drive.bf" </dev/null) 2>&1 >/dev/null \
        | grep -q "^brainstem: < 00 len=8 0400000005000000$"'

# TIER 5a
echo
echo "== tier 5a: metamorphic, across ops and across knobs =="
# Everything above asks whether one conversation produced the bytes it was
# supposed to. These ask a different kind of question: change ONE thing and
# require the rest to be unchanged.
#
# That catches a class the pinned traces cannot. A pin says "this run gives
# these bytes"; it cannot say WHY, so a value that leaked from one field into
# another -- a clock epoch reaching a length, a seed reaching a handle, a
# directory name reaching a reply -- is pinned right along with everything
# else and never looks wrong. These say what a knob is allowed to touch, and
# the interesting half of each is the "and nothing else".
#
# The relations were MEASURED before they were written down. Every one of
# them was run both ways first, and the "nothing else" in each is the diff
# that actually came back, not the diff that ought to have.
run "a frozen epoch changes the clock replies and nothing else" sh -c '
    ./build/brainstem --trace --clock frozen=1700000000 -- ./build/bfi bf/time/clock.bf \
        </dev/null 2>"$BS_TMP/m1" >/dev/null
    ./build/brainstem --trace --clock frozen=1800000000 -- ./build/bfi bf/time/clock.bf \
        </dev/null 2>"$BS_TMP/m2" >/dev/null
    got=$(diff "$BS_TMP/m1" "$BS_TMP/m2" | grep "^[<>]" | grep -cv "^[<>] brainstem: < 00 len=12 ")
    test "$got" = 0 || { diff "$BS_TMP/m1" "$BS_TMP/m2"; exit 1; }
    # and it must change SOMETHING, or the relation is satisfied by a knob
    # that does nothing at all
    cmp -s "$BS_TMP/m1" "$BS_TMP/m2" && { echo "two epochs gave one trace"; exit 1; }
    exit 0'
run "a seed changes its own echo and the random bytes, and nothing else" sh -c '
    ./build/brainstem --trace --seed 000102030405060708090a0b0c0d0e0f -- ./build/bfi \
        bf/rand/bytes.bf </dev/null 2>"$BS_TMP/m1" >/dev/null
    ./build/brainstem --trace --seed 0f0e0d0c0b0a09080706050403020100 -- ./build/bfi \
        bf/rand/bytes.bf </dev/null 2>"$BS_TMP/m2" >/dev/null
    got=$(diff "$BS_TMP/m1" "$BS_TMP/m2" | grep "^[<>]" \
          | grep -cv "^[<>] brainstem: < 00 len=\(104\|16\) ")
    test "$got" = 0 || { diff "$BS_TMP/m1" "$BS_TMP/m2"; exit 1; }
    cmp -s "$BS_TMP/m1" "$BS_TMP/m2" && { echo "two seeds gave one trace"; exit 1; }
    exit 0'
# THE ONE THE SORTED WALK EXISTS TO MAKE TRUE. Two directories with the same
# four names, built in opposite orders, must produce the same conversation.
#
# THE cd IS INSIDE THE SUBSHELL WITH THE RUN, and the first version of this
# check had it in a subshell of its own -- so both halves ran in the
# repository root instead, walked the same tree, desynced identically, and
# COMPARED EQUAL. It passed for two commits having asked nothing at all. A
# metamorphic check compares two runs, so it is the one shape that passes
# perfectly when both runs are wrong in the same way; the guard is that at
# least one of the two must also be pinned somewhere, and the walk is.
run "a sorted walk does not depend on the order the entries were made in" sh -c '
    rm -rf "$BS_TMP/walk" && mkdir -p "$BS_TMP/walk"
    (cd "$BS_TMP/walk" && : > d && : > b && mkdir c && : > a && eval "$BS_WALK") \
        | sed "$BS_NORM" > "$BS_TMP/m1"
    rm -rf "$BS_TMP/walk" && mkdir -p "$BS_TMP/walk"
    (cd "$BS_TMP/walk" && : > a && mkdir c && : > b && : > d && eval "$BS_WALK") \
        | sed "$BS_NORM" > "$BS_TMP/m2"
    diff -u "$BS_TMP/m1" "$BS_TMP/m2"
    # and it must be the walk that was compared, not a desync that matched
    grep -q "< 00 len=5 0201000063" "$BS_TMP/m1"'
# And the flag must not change a walk that has nothing to sort. roundtrip
# enumerates a directory of exactly one entry, where every order is the same
# order, so the two traces have to agree byte for byte.
run "--sort-readdir changes nothing when there is nothing to sort" sh -c '
    rm -rf "$BS_TMP/work" && mkdir -p "$BS_TMP/work"
    (cd "$BS_TMP/work" && "$BS_R/build/brainstem" --trace -- "$BS_R/build/bfi" \
        "$BS_R/bf/fs/roundtrip.bf" </dev/null) 2>&1 >/dev/null | sed "$BS_NORM" > "$BS_TMP/m1"
    rm -rf "$BS_TMP/work" && mkdir -p "$BS_TMP/work"
    (cd "$BS_TMP/work" && "$BS_R/build/brainstem" --trace --sort-readdir -- "$BS_R/build/bfi" \
        "$BS_R/bf/fs/roundtrip.bf" </dev/null) 2>&1 >/dev/null | sed "$BS_NORM" > "$BS_TMP/m2"
    diff -u "$BS_TMP/m1" "$BS_TMP/m2"'
# Nothing about where the broker is standing may reach a reply. The name is
# deliberately a different LENGTH as well as different content, because a
# path leaking into a reply would most likely arrive as a length.
run "the conversation does not depend on what the working directory is called" sh -c '
    rm -rf "$BS_TMP/w" && mkdir -p "$BS_TMP/w"
    (cd "$BS_TMP/w" && "$BS_R/build/brainstem" --trace -- "$BS_R/build/bfi" \
        "$BS_R/bf/fs/roundtrip.bf" </dev/null) 2>&1 >/dev/null | sed "$BS_NORM" > "$BS_TMP/m1"
    rm -rf "$BS_TMP/a-considerably-longer-directory-name"
    mkdir -p "$BS_TMP/a-considerably-longer-directory-name"
    (cd "$BS_TMP/a-considerably-longer-directory-name" && "$BS_R/build/brainstem" --trace \
        -- "$BS_R/build/bfi" "$BS_R/bf/fs/roundtrip.bf" </dev/null) 2>&1 >/dev/null \
        | sed "$BS_NORM" > "$BS_TMP/m2"
    diff -u "$BS_TMP/m1" "$BS_TMP/m2"'
# A read of a stream returns UP TO n bytes, so a program that asks for one
# byte at a time and one that asks for several must agree about the CONTENT
# even when they disagree about the grouping. bf/net/loopback.bf reads its
# two bytes singly and bf/fs/roundtrip.bf reads its two in one call; the
# bytes are "hi" in both, and that is a relation between two fixtures rather
# than a property of either.
run "one-byte reads and a single read agree about the bytes" sh -c '
    rm -rf "$BS_TMP/work" && mkdir -p "$BS_TMP/work"
    whole=$( (cd "$BS_TMP/work" && "$BS_R/build/brainstem" --trace -- "$BS_R/build/bfi" \
             "$BS_R/bf/fs/roundtrip.bf" </dev/null) 2>&1 >/dev/null \
             | sed -n "s/^brainstem: < 00 len=2 \(6869\)$/\1/p")
    singly=$(./build/brainstem --trace -- ./build/bfi bf/net/loopback.bf </dev/null 2>&1 >/dev/null \
             | sed -n "s/^brainstem: < 00 len=1 //p" | tr -d "\n")
    test "$whole" = "$singly" || { echo "whole [$whole] singly [$singly]"; exit 1; }
    test "$whole" = "6869" || { echo "neither read the expected bytes: [$whole]"; exit 1; }
    exit 0'

# TIER 6
echo
echo "== tier 6: error paths =="
# Each declared failure, reached by a real program rather than asserted.
run "an op before hello is fatal" sh -c '
    ./build/brainstem -- ./build/bfi bf/ctl/nohello.bf >/dev/null </dev/null 2>&1; test $? -eq 70'
run "a bad magic is fatal" sh -c '
    ./build/brainstem -- ./build/bfi bf/ctl/badmagic.bf >/dev/null </dev/null 2>&1; test $? -eq 70'
run "a wrong major version is fatal" sh -c '
    ./build/brainstem -- ./build/bfi bf/ctl/badversion.bf >/dev/null </dev/null 2>&1; test $? -eq 70'
run "a wrong arity is refused before the handler runs" sh -c '
    ./build/brainstem -- ./build/bfi bf/ctl/badarity.bf >/dev/null </dev/null 2>&1; test $? -eq 70'
run "an unknown opcode is recoverable" sh -c '
    ./build/brainstem -- ./build/bfi bf/ctl/unknownop.bf >/dev/null </dev/null 2>&1'
run "an unknown opcode answers NOSUCHOP and the program carries on" sh -c '
    ./build/brainstem --trace -- ./build/bfi bf/ctl/unknownop.bf </dev/null 2>&1 | grep -q "< 19 len=0"'

# ABI.md invariant I6: an error reply carries no payload, ever. A program that
# got a bad status reads exactly two more bytes, both zero, and is done.
#
# No current handler writes to the reply buffer and then fails, so the reset
# in broker.c that enforces this is defence in depth rather than something
# these cases can distinguish -- mutation checking says so plainly. It is
# pinned anyway, because the invariant is what lets every fixture drain a
# failure without knowing which failure it was, and the first handler that
# builds a reply incrementally will make it load bearing.
run "an error reply carries no payload" sh -c '
    rc=0
    ./build/brainstem --trace -- ./build/bfi bf/ctl/badmagic.bf </dev/null 2>&1 | grep -q "< e0 len=0" || rc=1
    ./build/brainstem --trace -- ./build/bfi bf/ctl/badarity.bf </dev/null 2>&1 | grep -q "< e2 len=0" || rc=1
    ./build/brainstem --trace -- ./build/bfi bf/ctl/nohello.bf  </dev/null 2>&1 | grep -q "< e4 len=0" || rc=1
    exit $rc'

run "an unknown clock id is INVAL rather than a plausible answer" sh -c '
    ./build/brainstem --trace -- ./build/bfi bf/time/badclock.bf </dev/null 2>&1 >/dev/null | grep -q "< 06 len=0"'
run "and INVAL is recoverable: the conversation continues past it" sh -c '
    ./build/brainstem --clock frozen=1700000000 -- ./build/bfi bf/time/badclock.bf >/dev/null </dev/null 2>&1'


# Nine refusals in one conversation, and the conversation continues through
# all of them. Every status here is recoverable: the error reply carries no
# payload, the program reads exactly three bytes, and the stream is still in
# step for the next request. An ABI where a refusal desynced the conversation
# would be one where a program could not afford to try anything.
run "every filesystem refusal lands on its own status, in order" sh -c '
    rm -rf "$BS_TMP/work" && mkdir -p "$BS_TMP/work"
    got=$( (cd "$BS_TMP/work" && "$BS_R/build/brainstem" --trace -- "$BS_R/build/bfi" "$BS_R/bf/fs/refused.bf" </dev/null) 2>&1 >/dev/null \
          | sed -n "s/^brainstem: < \(..\) len=.*/\1/p" | tr "\n" " ")
    want="00 05 06 06 06 00 04 09 00 03 03 00 00 00 00 "
    if [ "$got" != "$want" ]; then
        echo "wanted: $want"
        echo "got:    $got"
        exit 1
    fi
    exit 0'
run "and the program survives all nine and exits cleanly" sh -c '
    rm -rf "$BS_TMP/work" && mkdir -p "$BS_TMP/work"
    (cd "$BS_TMP/work" && "$BS_R/build/brainstem" -- "$BS_R/build/bfi" "$BS_R/bf/fs/refused.bf" </dev/null) >/dev/null 2>&1'
# ".." AND ABSOLUTE PATHS ARE NOT REFUSED ANY MORE, and this is the case that
# says so. The preopen model used to refuse both; it was removed because
# brainstem exists to make the system visible to a brainfuck program, and a
# literal path is the cheapest thing such a program can emit. Refusing one
# only ever cost reachability -- it never bought containment, because this was
# never a sandbox. The fixture opens "/" as a directory and expects a handle
# back -- an open rather than a stat, because a stat of the root reports a
# size and a mode belonging to the HOST, and a parity tier cannot pin those.
run "an absolute path reaches the system it names" sh -c '
    rm -rf "$BS_TMP/work" && mkdir -p "$BS_TMP/work"
    (cd "$BS_TMP/work" && "$BS_R/build/brainstem" --trace -- "$BS_R/build/bfi" "$BS_R/bf/fs/refused.bf" </dev/null 2>&1 >/dev/null) \
        | grep -q "^brainstem: < 00 len=4 04000100$"'


# The socket refusals, in order. Two are worth naming. Family 3 Unix is
# NOTSUP rather than INVAL because it is DECLARED by the ABI and not built by
# this broker, which is a different answer from "no such family" -- the first
# version collapsed the two and would have sent someone looking at their own
# encoder. And connect on a listener is ISCONN from the handle's kind rather
# than DENIED from its rights, because listen() strips CONNECT and a
# rights-first check would blame the capability when the handle was wrong.
run "every socket refusal lands on its own status, in order" sh -c '
    got=$(./build/brainstem --op-timeout 5000 --trace -- ./build/bfi bf/net/refused.bf </dev/null 2>&1 >/dev/null \
          | sed -n "s/^brainstem: < \(..\) .*/\1/p" | tr "\n" " ")
    want="00 06 17 06 06 00 20 03 00 00 00 26 04 00 00 "
    if [ "$got" != "$want" ]; then
        echo "wanted: $want"
        echo "got:    $got"
        exit 1
    fi
    exit 0'
# ECONNREFUSED is 61 on FreeBSD and 111 on Linux. This is the case that found
# the net errnos missing from sys_errmap entirely -- they arrived on the wire
# as IO, which is the "errno that escaped the map" that CONVENTIONS names as
# the reason the platform parity tier exists.
run "a refused connection is CONNREFUSED and not a raw errno" sh -c '
    ./build/brainstem --op-timeout 5000 --trace -- ./build/bfi bf/net/refused.bf </dev/null 2>&1 >/dev/null \
        | grep -q "^brainstem: < 20 len=0$"'
run "and the program survives all of them and exits cleanly" sh -c '
    ./build/brainstem --op-timeout 5000 -- ./build/bfi bf/net/refused.bf >/dev/null </dev/null 2>&1'


# The process refusals. The last two are the interesting pair: spawning a
# program that does not exist SUCCEEDS, and the failure arrives as the child's
# exit code 127 -- which is POSIX and what every shell reports. A broker that
# hid it would have to wait for the child before answering, making every spawn
# synchronous to make one error tidier.
run "every process refusal lands on its own status, in order" sh -c '
    rm -rf "$BS_TMP/proc" && mkdir -p "$BS_TMP/proc"
    cp build/bfi "$BS_TMP/proc/bfi" && cp bf/proc/echo.bf "$BS_TMP/proc/echo.bf"
    got=$( (cd "$BS_TMP/proc" && "$BS_R/build/brainstem" --op-timeout 5000 --trace -- "$BS_R/build/bfi" "$BS_R/bf/proc/refused.bf" </dev/null) 2>&1 >/dev/null \
          | sed -n "s/^brainstem: < \(..\) .*/\1/p" | tr "\n" " ")
    want="00 06 06 06 03 00 00 03 00 00 "
    if [ "$got" != "$want" ]; then
        echo "wanted: $want"
        echo "got:    $got"
        exit 1
    fi
    exit 0'
# PIPE 07, which was declared at M1 and produced by nothing until M7. Tier 6
# has claimed "every status reachable" that whole time, and a mutation sweep
# is what asked which check was standing behind this one.
#
# The fixture holds BOTH ends of the pipe and closes the read end, so the
# write that follows cannot succeed and cannot race. A program that merely
# stopped reading and exited would be a race: the reply fits in the pipe
# buffer, so whether the write fails depends on whether the interpreter has
# finished exiting.
run "writing into a pipe with no reader is PIPE, and the program carries on" sh -c '
    got=$(./build/brainstem --trace -- ./build/bfi bf/io/pipe.bf </dev/null 2>&1 >/dev/null \
          | sed -n "s/^brainstem: < \(..\) .*/\1/p" | tr "\n" " ")
    test "$got" = "00 00 00 07 00 00 " || { echo "got [$got]"; exit 1; }
    exit 0'
# AND THE BROKER SURVIVES IT. main.c ignores SIGPIPE in one line, and without
# that line this run does not report an error -- it dies, of a signal, halfway
# through answering, with no diagnosis at all. That is the worst failure shape
# this program has and it was one line away from M2 onward, with nothing able
# to see it.
run "and the broker survives it rather than dying of SIGPIPE" sh -c '
    ./build/brainstem -- ./build/bfi bf/io/pipe.bf </dev/null >/dev/null 2>&1
    rc=$?
    test $rc -eq 0 || { echo "the broker exited $rc (141 means SIGPIPE killed it)"; exit 1; }
    exit 0'
run "a child that could not exec is reported as exit 127" sh -c '
    rm -rf "$BS_TMP/proc" && mkdir -p "$BS_TMP/proc"
    cp build/bfi "$BS_TMP/proc/bfi" && cp bf/proc/echo.bf "$BS_TMP/proc/echo.bf"
    (cd "$BS_TMP/proc" && "$BS_R/build/brainstem" --op-timeout 5000 --trace -- "$BS_R/build/bfi" "$BS_R/bf/proc/refused.bf" </dev/null) 2>&1 >/dev/null \
        | grep -q "^brainstem: < 00 len=4 017f0000$"'

# TIER 7
echo
echo "== tier 7: determinism =="
# brainstem introduces the two things the sibling library was built to avoid:
# a clock and a random number generator. bfsodium keeps every run replayable by
# having neither -- "randomness supplied as input, never generated" -- and a
# syscall broker cannot keep that rule. So it keeps the PROPERTY instead: both
# sources stay, and both become steerable.
#
# Every case here is a PAIR. One reading proves a number; only the pair proves
# the knob is connected to anything. A generator that ignored its seed
# entirely would pass "the same seed repeats" perfectly.
run "the same seed gives byte identical output" sh -c '
    a=$(./build/brainstem --trace --seed 000102030405060708090a0b0c0d0e0f -- ./build/bfi bf/rand/bytes.bf </dev/null 2>&1 >/dev/null)
    b=$(./build/brainstem --trace --seed 000102030405060708090a0b0c0d0e0f -- ./build/bfi bf/rand/bytes.bf </dev/null 2>&1 >/dev/null)
    test "$a" = "$b"'
run "a different seed gives different output" sh -c '
    a=$(./build/brainstem --trace --seed 000102030405060708090a0b0c0d0e0f -- ./build/bfi bf/rand/bytes.bf </dev/null 2>&1 >/dev/null)
    b=$(./build/brainstem --trace --seed 000102030405060708090a0b0c0d0e10 -- ./build/bfi bf/rand/bytes.bf </dev/null 2>&1 >/dev/null)
    test "$a" != "$b"'
run "an unseeded generator does not repeat itself" sh -c '
    a=$(./build/brainstem --trace -- ./build/bfi bf/rand/bytes.bf </dev/null 2>&1 >/dev/null)
    b=$(./build/brainstem --trace -- ./build/bfi bf/rand/bytes.bf </dev/null 2>&1 >/dev/null)
    test "$a" != "$b"'
run "a frozen clock reads the same in two runs" sh -c '
    a=$(./build/brainstem --trace --clock frozen=1700000000 -- ./build/bfi bf/time/clock.bf </dev/null 2>&1 >/dev/null)
    b=$(./build/brainstem --trace --clock frozen=1700000000 -- ./build/bfi bf/time/clock.bf </dev/null 2>&1 >/dev/null)
    test "$a" = "$b"'
run "a live clock does not" sh -c '
    a=$(./build/brainstem --trace -- ./build/bfi bf/time/clock.bf </dev/null 2>&1 >/dev/null)
    b=$(./build/brainstem --trace -- ./build/bfi bf/time/clock.bf </dev/null 2>&1 >/dev/null)
    test "$a" != "$b"'
run "a virtual clock reads the same in two runs" sh -c '
    a=$(./build/brainstem --trace --clock virtual=100,step=1000000 -- ./build/bfi bf/time/clock.bf </dev/null 2>&1 >/dev/null)
    b=$(./build/brainstem --trace --clock virtual=100,step=1000000 -- ./build/bfi bf/time/clock.bf </dev/null 2>&1 >/dev/null)
    test "$a" = "$b"'

# The virtual clock is driven by the REQUEST COUNT, which is a property of the
# program and of nothing else -- not by wall time, and not by how often the
# clock was read. A step of one whole second makes that legible on the page:
# the handshake is request one, so the realtime read on request two is epoch
# plus two seconds (1000 + 2 = 0x3EA) and the monotonic read on request three
# is three seconds since a start that is defined to be zero.
run "the virtual clock advances one step per request, not per read" sh -c '
    got=$(./build/brainstem --trace --clock virtual=1000,step=1000000000 -- ./build/bfi bf/time/clock.bf </dev/null 2>&1 >/dev/null \
          | sed -n "s/^brainstem: < 00 len=12 //p" | tr "\n" " ")
    test "$got" = "ea0300000000000000000000 030000000000000000000000 "'

# A seed is refused rather than padded, and a clock spec is refused rather
# than guessed at. A silently truncated seed would make two runs differ for a
# reason nothing in the output could show, which is the one failure this
# entire tier exists to make impossible.
run "a short seed is refused" sh -c '
    ./build/brainstem --seed 0011 -- ./build/bfi bf/ctl/hello.bf >/dev/null </dev/null 2>&1; test $? -eq 2'
run "a seed that is not hex is refused" sh -c '
    ./build/brainstem --seed 000102030405060708090a0b0c0d0e0g -- ./build/bfi bf/ctl/hello.bf >/dev/null </dev/null 2>&1; test $? -eq 2'
run "a malformed clock spec is refused" sh -c '
    ./build/brainstem --clock fixed -- ./build/bfi bf/ctl/hello.bf >/dev/null </dev/null 2>&1; test $? -eq 2'
# "frozen=" with nothing after it meant "frozen=0" in the first draft, because
# strtol("") is 0 and says so only through errno. The self-test caught it; the
# case is kept here so the suite says out loud which grammar is meant.
run "an empty epoch is refused rather than read as zero" sh -c '
    ./build/brainstem --clock frozen= -- ./build/bfi bf/ctl/hello.bf >/dev/null </dev/null 2>&1; test $? -eq 2'
run "a zero virtual step is refused" sh -c '
    ./build/brainstem --clock virtual,step=0 -- ./build/bfi bf/ctl/hello.bf >/dev/null </dev/null 2>&1; test $? -eq 2'

echo
echo "== tier 7: --replay, and the trace as a complete record =="
# WHAT REPLAY IS FOR. Tier 10 pins traces and tier 7 requires two runs to
# produce the same one; all of that treats a trace as a RECORD. Nothing here
# checked it was a COMPLETE record -- a trace that dropped a frame or
# truncated a payload would still compare equal to a pinned copy of itself.
# A recording that cannot drive the program that produced it is not a record.
#
# AND NO SYSCALL IS MADE ON THE ABI PATH, which is checked BEHAVIOURALLY
# rather than with a tracer, and that is the stronger check of the two here.
# A syscall count proves nothing was asked; replaying a frozen clock under
# --clock live, or a seeded keystream with no seed, proves the VALUE came out
# of the recording. Those are the two ops that cross the seam most visibly,
# and the filesystem gets the same treatment below.
run "a recorded conversation replays, frame for frame" sh -c '
    rm -rf "$BS_TMP/work" && mkdir -p "$BS_TMP/work"
    (cd "$BS_TMP/work" && "$BS_R/build/brainstem" --trace -- "$BS_R/build/bfi" \
        "$BS_R/bf/fs/roundtrip.bf" </dev/null) 2>"$BS_TMP/rec" >/dev/null
    "$BS_R/build/brainstem" --replay "$BS_TMP/rec" -- "$BS_R/build/bfi" \
        "$BS_R/bf/fs/roundtrip.bf" </dev/null'
# The one that proves the filesystem was never touched: the recorded
# conversation makes a directory, writes a file, renames it and removes it.
# Replayed, it must leave the directory it runs in exactly as empty as it
# found it.
run "and it touches nothing -- an empty directory is still empty afterwards" sh -c '
    rm -rf "$BS_TMP/work" && mkdir -p "$BS_TMP/work"
    (cd "$BS_TMP/work" && "$BS_R/build/brainstem" --trace -- "$BS_R/build/bfi" \
        "$BS_R/bf/fs/roundtrip.bf" </dev/null) 2>"$BS_TMP/rec" >/dev/null
    rm -rf "$BS_TMP/empty" && mkdir -p "$BS_TMP/empty"
    (cd "$BS_TMP/empty" && "$BS_R/build/brainstem" --replay "$BS_TMP/rec" \
        -- "$BS_R/build/bfi" "$BS_R/bf/fs/roundtrip.bf" </dev/null) || exit 1
    left=$(ls -A "$BS_TMP/empty")
    test -z "$left" || { echo "the replay created: $left"; exit 1; }
    exit 0'
run "a frozen clock replayed under --clock live still reports the frozen instant" sh -c '
    ./build/brainstem --trace --clock frozen=1700000000 -- ./build/bfi bf/time/clock.bf \
        </dev/null 2>"$BS_TMP/rec" >/dev/null
    got=$(./build/brainstem --trace --clock live --replay "$BS_TMP/rec" -- ./build/bfi \
          bf/time/clock.bf </dev/null 2>&1 >/dev/null | grep -c "< 00 len=12 00f15365")
    test "$got" = 1'
run "a seeded keystream replayed with no seed still comes back" sh -c '
    ./build/brainstem --trace --seed 000102030405060708090a0b0c0d0e0f -- ./build/bfi \
        bf/rand/bytes.bf </dev/null 2>"$BS_TMP/rec" >/dev/null
    ./build/brainstem --trace --replay "$BS_TMP/rec" -- ./build/bfi bf/rand/bytes.bf \
        </dev/null 2>&1 >/dev/null \
        | grep -q "< 00 len=16 82233aa0ca0a14573efd34e9a85da697"'
# Four ways a replay must refuse, because a replay that accepted anything
# would be a check that never fails -- and this suite has met that shape
# before.
run "a divergence names the frame and both payloads" sh -c '
    ./build/brainstem --trace -- ./build/bfi bf/ctl/hello.bf </dev/null 2>"$BS_TMP/rec" >/dev/null
    out=$(./build/brainstem --replay "$BS_TMP/rec" -- ./build/bfi bf/time/clock.bf \
          </dev/null 2>&1); rc=$?
    test $rc -ne 0 || { echo "a divergent program replayed cleanly"; exit 1; }
    printf "%s\n" "$out" | grep -q "frame 2 diverges" || { echo "$out"; exit 1; }
    exit 0'
run "a program that outruns the recording is refused" sh -c '
    ./build/brainstem --trace -- ./build/bfi bf/ctl/hello.bf </dev/null 2>"$BS_TMP/rec" >/dev/null
    head -2 "$BS_TMP/rec" > "$BS_TMP/short"
    out=$(./build/brainstem --replay "$BS_TMP/short" -- ./build/bfi bf/ctl/hello.bf \
          </dev/null 2>&1); rc=$?
    test $rc -ne 0 || { echo "the program sent a frame the recording did not have"; exit 1; }
    printf "%s\n" "$out" | grep -q "the recording has 1" || { echo "$out"; exit 1; }
    exit 0'
run "a program that stops short of the recording is refused" sh -c '
    ./build/brainstem --trace -- ./build/bfi bf/ctl/hello.bf </dev/null 2>"$BS_TMP/rec" >/dev/null
    cat "$BS_TMP/rec" > "$BS_TMP/long"
    tail -2 "$BS_TMP/rec" >> "$BS_TMP/long"
    out=$(./build/brainstem --replay "$BS_TMP/long" -- ./build/bfi bf/ctl/hello.bf \
          </dev/null 2>&1); rc=$?
    test $rc -ne 0 || { echo "the program ended with the recording unfinished"; exit 1; }
    printf "%s\n" "$out" | grep -q "stopped after 2 of 3 frames" || { echo "$out"; exit 1; }
    exit 0'
# A NORMALISED trace is the trap worth a named error. tests/trace/ holds files
# with %% and H where the masked bytes were, and they are the traces a person
# has to hand. Skipping their unparseable lines would produce an empty
# recording and a replay that matched nothing at all -- passing, loudly,
# having checked nothing.
run "a normalised trace from tests/trace is refused, by name" sh -c '
    out=$(./build/brainstem --replay tests/trace/fs.roundtrip.txt -- ./build/bfi \
          bf/fs/roundtrip.bf </dev/null 2>&1); rc=$?
    test $rc -ne 0 || { echo "a normalised trace replayed"; exit 1; }
    printf "%s\n" "$out" | grep -q "NORMALISED" || { echo "$out"; exit 1; }
    exit 0'
run "a recording that breaks the request-reply alternation is refused" sh -c '
    printf "brainstem: < 00 len=0\n" > "$BS_TMP/bad"
    ./build/brainstem --replay "$BS_TMP/bad" -- ./build/bfi bf/ctl/hello.bf \
        </dev/null >/dev/null 2>&1; test $? -eq 2'

# TIER 8
echo
echo "== tier 8: the interpreter semantics matrix =="
# Every fixture under each end of input convention. The fixture job is
# unchanged and the interpreter varies within what brainfuck leaves
# unspecified; a fixture that only works under one convention is one that
# only works under our interpreter, and that is not brainfuck.
run "the round trip holds under all three EOF conventions" sh -c '
    rc=0
    for m in unchanged zero minus1; do
        BFI_EOF=$m ./build/brainstem -- ./build/bfi bf/ctl/hello.bf >/dev/null </dev/null 2>&1             || { echo "failed under BFI_EOF=$m"; rc=1; }
    done
    exit $rc'

# TIER 9
echo
echo "== tier 9: deadlock and timeout =="
# The defect this tier exists for is the project's worst: two processes each
# waiting for the other produce no output, no core, and a CI line reading
# "timed out", which points at everything except the cause. A suite that can
# hang is a suite nobody will run, so every case here is bounded.
run "a buffering interpreter is diagnosed, not hung" sh -c '
    BFI_FLUSH=block ./build/brainstem --hello-timeout 1500 -- ./build/bfi bf/ctl/hello.bf         >/dev/null </dev/null 2>&1; test $? -eq 71'
run "the diagnosis names buffering as the cause" sh -c '
    BFI_FLUSH=block ./build/brainstem --hello-timeout 1500 -- ./build/bfi bf/ctl/hello.bf </dev/null 2>&1         | grep -q "buffering its stdout"'
run "--check-interpreter accepts the vendored interpreter"     ./build/brainstem --check-interpreter ./build/bfi
run "--check-interpreter rejects a buffering one" sh -c '
    BFI_FLUSH=block ./build/brainstem --check-interpreter ./build/bfi >/dev/null </dev/null 2>&1; test $? -eq 72'

# TIER 10
echo
echo "== tier 10: platform parity =="
# The same fixtures must produce the same bytes on freebsd-15.1 and on
# ubuntu-26.04, and the expectation lives in this repository rather than being
# whatever this machine happened to produce. That distinction is the whole
# tier: a parity check that records what it sees passes on both guests while
# they disagree.
#
# Exactly ONE byte of the conversation is allowed to differ, and it is
# normalised to %% before the comparison: the platform byte in the hello
# reply, which exists precisely to be different. Everything else -- the frame
# layout, the field widths, the little-endian order, the status codes, the
# keystream -- is pinned identically for both. This is the tier that catches
# AF_INET6 being 28 on one and 10 on the other, O_CREAT being 0x0200 and
# 0x0040, a stat field that is 32 bits in one place, and an errno that escaped
# the map. At M3 none of those exist yet, which is exactly when to pin the
# ones that do.
# The two masks this tier depends on are defined at the top of this file,
# beside BS_TMP, because tier 5a needs them too and a definition that sits
# inside the tier that happened to want it first is one that quietly does not
# exist yet for the tier that runs earlier. That is not hypothetical: it cost
# two failing metamorphic checks and an unmasked mtime.


run "clock.bf under a frozen clock matches the pinned trace" sh -c '
    ./build/brainstem --trace --clock frozen=1700000000 -- ./build/bfi bf/time/clock.bf </dev/null 2>&1 >/dev/null \
        | sed "$BS_NORM" > "$BS_TMP/obs"
    diff -u tests/trace/clock.frozen.txt "$BS_TMP/obs"'
run "clock.bf under a virtual clock matches the pinned trace" sh -c '
    ./build/brainstem --trace --clock virtual=100,step=1000000 -- ./build/bfi bf/time/clock.bf </dev/null 2>&1 >/dev/null \
        | sed "$BS_NORM" > "$BS_TMP/obs"
    diff -u tests/trace/clock.virtual.txt "$BS_TMP/obs"'
run "badclock.bf matches the pinned trace" sh -c '
    ./build/brainstem --trace --clock frozen=1700000000 -- ./build/bfi bf/time/badclock.bf </dev/null 2>&1 >/dev/null \
        | sed "$BS_NORM" > "$BS_TMP/obs"
    diff -u tests/trace/badclock.frozen.txt "$BS_TMP/obs"'
run "bytes.bf under a seed matches the pinned trace" sh -c '
    ./build/brainstem --trace --seed 000102030405060708090a0b0c0d0e0f -- ./build/bfi bf/rand/bytes.bf </dev/null 2>&1 >/dev/null \
        | sed "$BS_NORM" > "$BS_TMP/obs"
    diff -u tests/trace/rand.seeded.txt "$BS_TMP/obs"'
run "hello.bf matches the pinned trace" sh -c '
    ./build/brainstem --trace --clock frozen=0 --seed 00000000000000000000000000000000 -- ./build/bfi bf/ctl/hello.bf </dev/null 2>&1 >/dev/null \
        | sed "$BS_NORM" > "$BS_TMP/obs"
    diff -u tests/trace/ctl.hello.txt "$BS_TMP/obs"'

run "roundtrip.bf matches the pinned trace" sh -c '
    rm -rf "$BS_TMP/work" && mkdir -p "$BS_TMP/work"
    (cd "$BS_TMP/work" && "$BS_R/build/brainstem" --trace -- "$BS_R/build/bfi" "$BS_R/bf/fs/roundtrip.bf" </dev/null) 2>&1 >/dev/null \
        | sed "$BS_NORM" > "$BS_TMP/obs"
    diff -u tests/trace/fs.roundtrip.txt "$BS_TMP/obs"'
run "walk.bf under --sort-readdir matches the pinned trace" sh -c '
    eval "$BS_MKWALK"
    eval "$BS_WALK" | sed "$BS_NORM" > "$BS_TMP/obs"
    diff -u "$BS_R/tests/trace/fs.walk.txt" "$BS_TMP/obs"'
# THE PIN ABOVE IS THE CHECK THAT --sort-readdir WORKS, and this is the check
# that the pin is reading the walk rather than the frames around it. Rename
# one entry and the trace has to change; if it does not, the four readdir
# replies are not being compared at all.
run "and the walk pin sees the entries, not just the frames around them" sh -c '
    eval "$BS_MKWALK"
    eval "$BS_WALK" | sed "$BS_NORM" > "$BS_TMP/obs"
    mv "$BS_TMP/walk/d" "$BS_TMP/walk/e"
    (cd "$BS_TMP/walk" && eval "$BS_WALK") | sed "$BS_NORM" > "$BS_TMP/obs2"
    if cmp -s "$BS_TMP/obs" "$BS_TMP/obs2"; then
        echo "renaming an entry did not change the trace"
        exit 1
    fi
    exit 0'
# SORTEDNESS ITSELF, checked against no pin at all. The entries are created
# d, b, c, a, so a filesystem enumerating in creation order -- which is the
# PRIMARY platform -- fails this the moment the flag stops working.
#
# What it cannot rule out is a filesystem that already returns names in byte
# order, where an inert flag would pass. That is why the flag is ALSO pinned
# above: between the two, an inert flag has nowhere left to hide except a
# host where both checks are vacuous for the same reason.
run "--sort-readdir returns names in byte order, whatever order they were made in" sh -c '
    eval "$BS_MKWALK"
    got=$(eval "$BS_WALK" | sed -n "s/^brainstem: < 00 len=5 ....0000\(..\)$/\1/p")
    test -n "$got" || { echo "no entries came back at all"; exit 1; }
    want=$(printf "%s\n" "$got" | sort)
    test "$got" = "$want" || { echo "got [$got] wanted [$want]"; exit 1; }
    test "$(printf "%s\n" "$got" | wc -l)" = "$(printf "%s\n" "$got" | sort -u | wc -l)" \
        || { echo "an entry came back twice"; exit 1; }
    exit 0'
run "refused.bf matches the pinned trace" sh -c '
    rm -rf "$BS_TMP/work" && mkdir -p "$BS_TMP/work"
    (cd "$BS_TMP/work" && "$BS_R/build/brainstem" --trace -- "$BS_R/build/bfi" "$BS_R/bf/fs/refused.bf" </dev/null) 2>&1 >/dev/null \
        | sed "$BS_NORM" > "$BS_TMP/obs"
    diff -u tests/trace/fs.refused.txt "$BS_TMP/obs"'
# The mtime mask must not eat the rest of the stat record. A file of a
# different SIZE has to produce a different trace, or the mask is covering
# more than it says it does.
run "the stat mask does not swallow the record around it" sh -c '
    rm -rf "$BS_TMP/work" && mkdir -p "$BS_TMP/work"
    (cd "$BS_TMP/work" && "$BS_R/build/brainstem" --trace -- "$BS_R/build/bfi" "$BS_R/bf/fs/roundtrip.bf" </dev/null) 2>&1 >/dev/null \
        | sed "$BS_NORM" | grep -q "^brainstem: < 00 len=32 01010100020000000000000" || exit 1
    exit 0'

# The one byte the traces above hide, checked here on its own -- and checked
# against a mapping written HERE rather than read out of the broker, so that
# the suite is a second opinion about it instead of an echo. sys.h numbers the
# platforms 1 FreeBSD, 2 Linux, and a build that reported the wrong one would
# otherwise sail through every case above.
run "the platform byte is the one this kernel should report" sh -c '
    case "$(uname -s)" in
        FreeBSD) want=01 ;;
        Linux)   want=02 ;;
        *)       echo "no expectation for $(uname -s)"; exit 1 ;;
    esac
    got=$(./build/brainstem --trace -- ./build/bfi bf/ctl/hello.bf </dev/null 2>&1 >/dev/null \
          | sed -n "s/^brainstem: < 00 len=[0-9]* 4253544d.\{32\}\(..\).*/\1/p")
    test "$got" = "$want"'

# The pinned traces must actually be able to fail. A normalisation that ate
# too much would make every one of them pass against anything, and nothing
# above would notice.
run "the pinned traces are not vacuous" sh -c '
    ./build/brainstem --trace --clock frozen=1700000001 -- ./build/bfi bf/time/clock.bf </dev/null 2>&1 >/dev/null \
        | sed "$BS_NORM" > "$BS_TMP/obs"
    if cmp -s "$BS_TMP/obs" tests/trace/clock.frozen.txt; then
        echo "a different epoch produced an identical trace: the mask eats too much"
        exit 1
    fi
    exit 0'

# TIER 10a
echo
echo "== tier 10a: the per-op syscall surface =="
# The measured half of what syscall-lean was going to buy. See tools/bscalls.sh
# for the window and tests/syscalls/README for what is pinned and what is
# still inferred on the primary platform.
run "every op asks the kernel for exactly what is pinned" sh tools/bscalls.sh

# The filesystem ops are measured here and NOT yet compared against anything,
# and this is a report rather than a check -- it prints and always passes,
# which is why it is not a run line and is not counted.
#
# The reason is a lesson that cost two round trips. The first freebsd/
# expectations in this tree were written from what the platform documents
# rather than from a run, and two of the five were wrong. M4 landed on a host
# that cannot reach the primary platform, so writing them by hand again would
# be the same guess a third time. The run that prints these IS the run that
# produces the expectation: paste them in, and the cases move from a report
# into the pinned list.
sh tools/bscalls.sh --report 2>&1 || true


# TIER 10b
echo
echo "== tier 10b: the seam is narrow =="
# The other measured half, and this one reads the objects the compiler
# emitted rather than the source it was given.
run "the external surface is the one tests/audit/allow.txt declares" sh tools/bsaudit.sh

# TIER 11
echo
echo "== tier 11: would these checks catch the bug they claim to? =="
# Declared at M0 as "a discipline rather than a check" and automated at M7.
# Every tier above asks whether this program is right; this one asks whether
# the CHECKS are worth anything, and it is the only tier that can.
#
# tools/bsmut.sh copies the tree, breaks one thing with sed, relinks the
# broker alone, and runs THIS FILE with BS_ONLY set to the check that
# mutation is supposed to break -- which must then fail. Thirty three
# mutations: one per built op, plus the invariants the ABI rests on.
#
# Naming the check is the point. The table in that file is a coverage map,
# machine-verified, and it has already earned its keep three times: `poll`
# had no fixture at all, PIPE 07 was a status nothing produced, and the
# SIGPIPE ignore in main.c had nothing standing behind it.
#
# IT IS SKIPPED INSIDE A FILTERED RUN, which is what bsmut's own children are.
# Without that this file would invoke the tool that invokes this file.
#
# It is also the most expensive tier here by a wide margin -- around seventy
# seconds against ten for everything else -- and that is the right trade
# exactly once per gate.
if [ -z "${BS_ONLY:-}" ]; then
    run "every deliberate defect is caught by the check named for it" sh tools/bsmut.sh
fi

echo
echo "== summary =="
if [ -n "${BS_ONLY:-}" ]; then echo "(BS_ONLY was set: this is a FILTERED run, not the suite)"; fi
echo "passed $pass, failed $fail on $(uname -srm)"
[ "$fail" -eq 0 ] || exit 1
