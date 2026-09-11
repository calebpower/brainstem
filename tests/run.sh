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

# The vendored interpreter carries one intentional delta from upstream and the
# whole project rests on it. Someone tidying the vendored file back toward its
# source would reintroduce a deadlock whose only symptom is a hang, so the
# delta is pinned here rather than trusted to a comment.
run "the vendored interpreter still has the unbuffering fix" \
    grep -q "setvbuf(stdout, NULL, _IONBF, 0)" tools/bfi.c

echo
echo "== tier 1: interpreter self-test =="
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

printf '%s' '++++++++[>++++[>++>+++>+++>+<<<<-]>+>+>->>+[<]<-]>>.>---.+++++++..+++.>>.<-.<.+++.------.--------.>>+.>++.' > "$tmp/hello.bf"
run "hello world" sh -c "\"$bfi\" \"$tmp/hello.bf\" </dev/null | cmp -s - /dev/stdin <<'EOT'
Hello World!
EOT"

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

echo
echo "== summary =="
echo "passed $pass, failed $fail on $(uname -srm)"
[ "$fail" -eq 0 ] || exit 1
