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
run() {  # run LABEL CMD...
    label="$1"; shift
    if "$@" >/dev/null 2>&1; then echo "PASS $label"; pass=$((pass+1))
    else echo "FAIL $label"; fail=$((fail+1)); fi
}

echo "== build =="
# tools/build.sh is the only thing in this repository that invokes a compiler,
# and the check below proves it stays that way.
sh tools/build.sh >/dev/null
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

# The expander must stay ignorant of the ABI. This is the line between an
# expander and a compiler, and it is the same shape of check as the one
# guarding the lane definitions: if bfgen knew an op name or a length, the
# fixtures would be generated from the same knowledge the broker is built
# from, and a byte order bug would be invisible to the whole suite.
run "the expander knows no opcode and no op name" sh -c '
    body=$(grep -v "^[[:space:]]*#" tools/bfgen.sh)
    ! printf "%s" "$body" | grep -Eqi "hello|ABI\.md|opcode|0x0[1-9]|BSTM"'

# TIER 5
echo
echo "== tier 5: a standard brainfuck program completes a round trip =="
# THE milestone. A file containing nothing but the eight instructions, run
# under a general purpose interpreter, reaching an operating system.
run "the handshake round trips and the program exits 0" sh -c '
    ./build/brainstem -- ./build/bfi bf/ctl/hello.bf >/dev/null 2>&1'
run "the trace shows exactly the four frames" sh -c '
    got=$(./build/brainstem --trace -- ./build/bfi bf/ctl/hello.bf 2>&1 | grep -c "brainstem: [<>]")
    test "$got" = 4'
run "the program chooses the exit status" sh -c '
    ./build/brainstem -- ./build/bfi bf/ctl/exitcode.bf >/dev/null 2>&1; test $? -eq 1'

# TIER 6
echo
echo "== tier 6: error paths =="
# Each declared failure, reached by a real program rather than asserted.
run "an op before hello is fatal" sh -c '
    ./build/brainstem -- ./build/bfi bf/ctl/nohello.bf >/dev/null 2>&1; test $? -eq 70'
run "a bad magic is fatal" sh -c '
    ./build/brainstem -- ./build/bfi bf/ctl/badmagic.bf >/dev/null 2>&1; test $? -eq 70'
run "a wrong major version is fatal" sh -c '
    ./build/brainstem -- ./build/bfi bf/ctl/badversion.bf >/dev/null 2>&1; test $? -eq 70'
run "a wrong arity is refused before the handler runs" sh -c '
    ./build/brainstem -- ./build/bfi bf/ctl/badarity.bf >/dev/null 2>&1; test $? -eq 70'
run "an unknown opcode is recoverable" sh -c '
    ./build/brainstem -- ./build/bfi bf/ctl/unknownop.bf >/dev/null 2>&1'
run "an unknown opcode answers NOSUCHOP and the program carries on" sh -c '
    ./build/brainstem --trace -- ./build/bfi bf/ctl/unknownop.bf 2>&1 | grep -q "< 19 len=0"'

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
    ./build/brainstem --trace -- ./build/bfi bf/ctl/badmagic.bf 2>&1 | grep -q "< e0 len=0" || rc=1
    ./build/brainstem --trace -- ./build/bfi bf/ctl/badarity.bf 2>&1 | grep -q "< e2 len=0" || rc=1
    ./build/brainstem --trace -- ./build/bfi bf/ctl/nohello.bf  2>&1 | grep -q "< e4 len=0" || rc=1
    exit $rc'

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
        BFI_EOF=$m ./build/brainstem -- ./build/bfi bf/ctl/hello.bf >/dev/null 2>&1             || { echo "failed under BFI_EOF=$m"; rc=1; }
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
    BFI_FLUSH=block ./build/brainstem --hello-timeout 1500 -- ./build/bfi bf/ctl/hello.bf         >/dev/null 2>&1; test $? -eq 71'
run "the diagnosis names buffering as the cause" sh -c '
    BFI_FLUSH=block ./build/brainstem --hello-timeout 1500 -- ./build/bfi bf/ctl/hello.bf 2>&1         | grep -q "buffering its stdout"'
run "--check-interpreter accepts the vendored interpreter"     ./build/brainstem --check-interpreter ./build/bfi
run "--check-interpreter rejects a buffering one" sh -c '
    BFI_FLUSH=block ./build/brainstem --check-interpreter ./build/bfi >/dev/null 2>&1; test $? -eq 72'

echo
echo "== summary =="
echo "passed $pass, failed $fail on $(uname -srm)"
[ "$fail" -eq 0 ] || exit 1
