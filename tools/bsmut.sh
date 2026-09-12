#!/bin/sh
# bsmut.sh -- TIER 11: would the suite catch the bug it claims to?
#
# Every other tier asks whether this program is right. This one asks whether
# the CHECKS are worth anything, and it is the only tier that can: a check
# that has never been observed failing is indistinguishable from a check that
# cannot fail. The project has had three of those already -- a mask that ate
# the byte it was meant to spare, a BS_ONLY that matched on the word "the",
# and a metamorphic comparison of two runs that were wrong in the same way.
#
# HOW IT WORKS. The tree is copied, one deliberate defect is applied with sed,
# the broker alone is relinked, and tests/run.sh is run with BS_ONLY set to
# THE CHECK THAT MUTATION IS SUPPOSED TO BREAK. That check must fail. Then the
# copy is thrown away.
#
# NAMING THE CHECK IS THE POINT, and it is why this is not "mutate, run
# everything, require any failure". A table of op against check is a COVERAGE
# MAP, machine-verified: it says out loud which check is standing behind each
# of the twenty three ops. When a row stops holding, the answer is not to
# widen the row -- it is that the op has lost its cover.
#
# It has already earned that. `poll` was built at M4 and had no fixture at
# all; tier 5 has claimed "one fixture per op" ever since and was wrong about
# one of twenty three. There was no check to put in its row, which is how the
# gap surfaced. bf/io/poll.poke exists because of this table.
#
# THE ORACLES ARE NEVER REBUILT. `build.sh --relink` recompiles one unit and
# relinks build/brainstem; build/bscodec, build/bsframe, build/bsbf and
# build/bfi stay as they were. A mutation tester that rebuilt its own
# interpreter would be marking its own homework.
#
# Cost: about seventy seconds for thirty three mutations on a quiet Linux
# container, of which most is compiling and the rest is fifteen deliberate
# desyncs waiting out a timeout. A full build and a full suite per mutation
# would be twenty minutes, and nobody runs a twenty minute tier. Three things
# buy that down and each is load bearing: build.sh --relink, tests/run.sh's
# BS_ONLY, and the lowered timeouts in the copy.
#
# It is still by far the most expensive tier in the suite, and that is the
# right trade exactly once per gate.
#
# Usage:  sh tools/bsmut.sh [--selftest] [--list]
# Exit:   0 every mutation was caught; 1 one survived; 2 the tool could not run.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

bm_die() { echo "bsmut: $*" >&2; exit 2; }

# ---- the mutations ---------------------------------------------------------
#
# Four fields: NAME | UNIT | SED | CHECK. UNIT is a file under src/ without
# the .c, SED is applied to it, and CHECK is the BS_ONLY pattern -- a
# substring of the label of the check that must go red.
#
# The op sweep is GENERATED from the op table rather than typed, because
# twenty three near-identical rows is exactly the sort of list that acquires a
# typo nobody sees. Each one makes the dispatcher answer OK with an empty
# reply for one opcode, which is the most general defect an op can have: it
# does nothing and says it worked.

# The dispatch line the op sweep rewrites. Held in one place because it is the
# single thing in this file that knows what broker.c looks like.
BM_DISPATCH='st = op->fn(&ctx, &req, &rep);'

op_checks() {
    cat <<'EOT'
01|the handshake round trips and the program exits 0
02|the program chooses the exit status
03|clock_now answers on both clocks
04|random_bytes returns as many bytes as it was asked for
05|a brainfuck program connects to itself over TCP
06|a refused connection is CONNREFUSED and not a raw errno
07|the port the program connects to is the port bind gave it
08|every socket refusal lands on its own status, in order
09|accept reports a handle and a 32 byte peer address
0a|roundtrip.bf matches the pinned trace
0b|read returns what write was given
0c|a reused slot comes back with a new generation each time
0d|poll answers not-readable, then readable, on the same handle
0e|pipe yields a read end and a write end, in that order
0f|brainfuck drives brainfuck
10|the child exits 0 and wait reports it
11|the whole filesystem round trip completes
12|roundtrip.bf matches the pinned trace
13|the stat mask does not swallow the record around it
14|readdir yields one entry then ends
15|it really wrote the bytes, and really moved and removed the file
16|it really wrote the bytes, and really moved and removed the file
17|it really wrote the bytes, and really moved and removed the file
EOT
}

# The rules, which are not ops. These are the invariants the ABI rests on, and
# each is broken the way a tired person would break it rather than the way a
# saboteur would.
rule_mutations() {
    cat <<'EOT'
little-endian-length|frame|s/= (unsigned char)( v[^)]*);/= (unsigned char)((v >> 8) \& 0xFF);/|hello.bf matches the pinned trace
handle-generation|fdtab|s/tab\[i\].gen = (bs_u16)(tab\[i\].gen + 1);//|a reused slot comes back with a new generation each time
raw-errno-on-the-wire|sys_posix|s/case ECONNREFUSED:  return BS_CONNREFUSED;/case ECONNREFUSED:  return (bs_err)e;/|a refused connection is CONNREFUSED and not a raw errno
strict-arity|ops|s/^int bs_op_arity_ok(const struct bs_op \*o, unsigned int len) {/int bs_op_arity_ok(const struct bs_op *o, unsigned int len) { (void)o; (void)len; return 1;/|brainstem self-test
the-hello-gate|broker|s/st = BS_NOHELLO;/st = BS_OK;/|an op before hello is fatal
an-empty-path-to-an-fs-op|op_fs|s/if (!allow_empty) return BS_INVAL;//|every filesystem refusal lands on its own status, in order
an-empty-path-to-spawn|path|s/if (n == 0) return BS_INVAL;//|every process refusal lands on its own status, in order
dot-and-dotdot|sys_posix|s/if (e->d_name\[0\] == /if (0 \&\& e->d_name[0] == /|readdir yields one entry then ends
rights-only-narrow|op_io|s/if ((s->rights \& rights) != rights) return BS_DENIED;//|every filesystem refusal lands on its own status, in order
the-sigpipe-ignore|main|s/signal(SIGPIPE, SIG_IGN);//|the broker survives it rather than dying of SIGPIPE
EOT
}

# ---- one mutation ----------------------------------------------------------

bm_apply() {  # bm_apply TREE UNIT SED -- 0 applied, 1 the sed changed nothing
    _f=$1/src/$2.c
    [ -f "$_f" ] || bm_die "no such unit src/$2.c"
    cp "$_f" "$_f.orig"
    sed "$3" "$_f.orig" > "$_f"
    if cmp -s "$_f" "$_f.orig"; then return 1; fi
    rm -f "$_f.orig"
    return 0
}

# A MUTATION THAT DID NOT APPLY IS THE WORST OUTCOME THIS TOOL HAS, because it
# looks exactly like a mutation that survived: the suite passes, and the
# report says the check did not notice. It would send somebody after a gap in
# the tests that is really a typo in this file. So it is checked, separately,
# and reported as a tool failure rather than as a finding.

bm_run() {  # bm_run NAME UNIT SED CHECK -- prints a verdict, sets bm_bad
    _name=$1; _unit=$2; _sed=$3; _check=$4

    rm -rf "$bm_tree/src"
    cp -R "$bm_pristine" "$bm_tree/src"

    # THE OBJECT OF THE PREVIOUS MUTATION IS STILL IN build/obj. Restoring the
    # source is not enough, because --relink only recompiles the unit it is
    # given -- so a run that mutated broker.c and then mutates fdtab.c would be
    # testing two defects at once and crediting the second with the first's
    # catch. Relinking the previous unit from the restored source puts it back.
    if [ -n "$bm_last" ] && [ "$bm_last" != "$_unit" ]; then
        (cd "$bm_tree" && sh tools/build.sh --relink "$bm_last") >"$bm_out" 2>&1 \
            || bm_die "could not restore src/$bm_last.c"
    fi
    bm_last=$_unit

    if ! bm_apply "$bm_tree" "$_unit" "$_sed"; then
        echo "bsmut: TOOL FAIL $_name: the sed changed nothing in src/$_unit.c"
        bm_bad=1
        return 0
    fi

    if ! (cd "$bm_tree" && sh tools/build.sh --relink "$_unit") >"$bm_out" 2>&1; then
        # A mutation that will not COMPILE is caught, and by the strictest
        # check there is. It is reported as such rather than counted as a pass,
        # because it means this row is not testing what it says it is.
        echo "bsmut: caught  $_name -- it does not compile"
        return 0
    fi

    if (cd "$bm_tree" && BS_NOBUILD=1 BS_ONLY="$_check" sh tests/run.sh) >"$bm_out" 2>&1; then
        ran=$(sed -n 's/^passed \([0-9]*\), failed.*/\1/p' "$bm_out")
        if [ "${ran:-0}" = 0 ]; then
            echo "bsmut: TOOL FAIL $_name: no check matched \"$_check\""
        else
            echo "bsmut: SURVIVED $_name -- \"$_check\" passed anyway"
        fi
        bm_bad=1
    else
        echo "bsmut: caught  $_name"
    fi
    return 0
}

# ---- both polarities -------------------------------------------------------

bm_selftest() {
    _d=$(mktemp -d)
    bm_tree=$_d/tree
    bm_out=$_d/out
    bm_bad=0
    bm_last=
    bm_pristine=$_d/pristine
    mkdir -p "$bm_tree"

    # A sed that matches nothing must be a TOOL failure and not a finding.
    cp -R "$repo/src" "$bm_tree/src"
    if bm_apply "$bm_tree" frame 's/this string is not in frame.c/x/'; then
        echo "SELFTEST FAIL: a sed that changed nothing was accepted"
        rm -rf "$_d"; return 1
    fi
    echo "selftest ok: a mutation that does not apply is refused, not counted"

    # And one that does match must be applied. The sed has to change BYTES,
    # not merely match: applying is measured by comparing the file with itself
    # before and after, so a substitution of a string for itself is correctly
    # reported as having done nothing.
    rm -rf "$bm_tree/src"; cp -R "$repo/src" "$bm_tree/src"
    if ! bm_apply "$bm_tree" frame 's/unsigned int/unsigned  int/'; then
        echo "SELFTEST FAIL: a sed that matched was reported as not applying"
        rm -rf "$_d"; return 1
    fi
    echo "selftest ok: a mutation that applies is reported as applying"

    # Every row must name a unit that exists and a check the suite has. The
    # second half is the one that rots: a check gets reworded, the row stops
    # matching, and every mutation in it silently becomes a TOOL FAIL.
    _rc=0
    op_checks | while IFS='|' read -r code check; do
        [ -n "$code" ] || continue
        grep -qF "$check" "$repo/tests/run.sh" || {
            echo "SELFTEST FAIL: op $code names a check tests/run.sh does not have:"
            echo "  $check"
            exit 1
        }
    done || _rc=1
    rule_mutations | while IFS='|' read -r name unit sed_ check; do
        [ -n "$name" ] || continue
        [ -f "$repo/src/$unit.c" ] || { echo "SELFTEST FAIL: $name names src/$unit.c"; exit 1; }
        grep -qF "$check" "$repo/tests/run.sh" || {
            echo "SELFTEST FAIL: $name names a check tests/run.sh does not have:"
            echo "  $check"
            exit 1
        }
    done || _rc=1
    [ "$_rc" = 0 ] || { rm -rf "$_d"; return 1; }
    echo "selftest ok: every row names a real unit and a real check"

    # And every BUILT op has a row, which is what makes this a sweep rather
    # than a selection.
    _ops=$("$repo/build/brainstem" --dump-abi | awk '$4 == "built" { print $2 }' | sort)
    _rows=$(op_checks | cut -d'|' -f1 | sort)
    if [ "$_ops" != "$_rows" ]; then
        echo "SELFTEST FAIL: the op sweep does not cover every built op"
        echo "  built: $(echo $_ops)"
        echo "  rows:  $(echo $_rows)"
        rm -rf "$_d"; return 1
    fi
    echo "selftest ok: every built op has a row, so this is a sweep"

    rm -rf "$_d"
    echo "bsmut --selftest: ok"
    return 0
}

# ---- entry -----------------------------------------------------------------

case "${1:-}" in
    --selftest) bm_selftest; exit $? ;;
    --list)     op_checks; rule_mutations; exit 0 ;;
    "")         ;;
    *)          bm_die "usage: sh tools/bsmut.sh [--selftest] [--list]" ;;
esac

[ -x "$repo/build/brainstem" ] || bm_die "build/brainstem is not built"

d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
bm_tree=$d/tree
bm_out=$d/out
bm_bad=0
bm_last=
bm_pristine=$d/pristine

mkdir -p "$bm_tree"
for x in src tools tests bf; do cp -R "$repo/$x" "$bm_tree/$x"; done
for x in ABI.md CONVENTIONS.md GUIDE.md HANDOFF.md README.md .reaper.toml Containerfile; do
    [ -e "$repo/$x" ] && cp "$repo/$x" "$bm_tree/$x"
done

# THE COPY'S TIMEOUTS ARE LOWERED, AND THIS IS WHAT MAKES THE TIER AFFORDABLE.
#
# A mutation that shortens a reply does not produce a wrong answer -- it
# produces a DESYNC. The program reads fewer bytes than it was sent, then
# blocks waiting for the rest, the broker blocks waiting for the next request,
# and the conversation sits there until the op timeout fires. That is exactly
# the failure being detected, and it is caught correctly; it just costs thirty
# seconds of wall clock per mutation to notice.
#
# Fifteen of the thirty three mutations here are of that shape. The first run
# of this tool took 517 seconds, of which about 450 were spent waiting for
# timeouts to expire on defects that were already decided.
#
# Two seconds is plenty for every fixture in this tree -- the whole suite runs
# in ten -- and it changes nothing about what is measured, because a timeout
# is not in any reply. The checks that are ABOUT timeouts pass their own on the
# command line, so they are untouched by this.
bm_t=$bm_tree/src/brainstem.h
sed -e 's/#define BS_OP_TIMEOUT_MS 30000/#define BS_OP_TIMEOUT_MS 2000/' \
    -e 's/#define BS_HELLO_TIMEOUT_MS 5000/#define BS_HELLO_TIMEOUT_MS 2000/' \
    "$bm_t" > "$bm_t.new" && mv "$bm_t.new" "$bm_t"
grep -q "BS_OP_TIMEOUT_MS 2000" "$bm_t" || bm_die "could not lower the copy's op timeout"

# The pristine source, which every mutation is restored from. It is the COPY's
# source rather than the repository's, so the lowered timeouts survive the
# restore -- restoring from $repo would quietly put the thirty seconds back.
cp -R "$bm_tree/src" "$bm_pristine"

# One full build, here and nowhere else. THE ORACLES ARE BUILT ONCE, FROM
# UNMUTATED SOURCE, AND NEVER REBUILT: build.sh --relink touches one unit and
# the broker, so build/bscodec, build/bsframe, build/bsbf and build/bfi stay as
# they are for the whole run. A mutation tester that rebuilt its own
# interpreter would be marking its own homework.
(cd "$bm_tree" && sh tools/build.sh) >"$bm_out" 2>&1 || {
    echo "bsmut: the copy does not build" >&2; cat "$bm_out" >&2; exit 2; }

# THE COPY MUST BE GREEN BEFORE ANYTHING IS BROKEN IN IT. Without this, a
# mutation tester that had merely copied the tree wrongly would report every
# mutation as caught, and be believed.
if ! (cd "$bm_tree" && BS_NOBUILD=1 BS_ONLY="the handshake round trips" sh tests/run.sh) \
        >"$bm_out" 2>&1; then
    echo "bsmut: the unmutated copy does not pass; nothing below would mean anything" >&2
    cat "$bm_out" >&2
    exit 2
fi
echo "bsmut: the unmutated copy passes, so a failure below is the mutation"

op_checks > "$d/ops"
rule_mutations > "$d/rules"

while IFS='|' read -r code check; do
    [ -n "$code" ] || continue
    bm_run "op-$code" broker \
        "s/$BM_DISPATCH/st = (op->code == 0x$code) ? BS_OK : op->fn(\&ctx, \&req, \&rep);/" \
        "$check"
done < "$d/ops"

while IFS='|' read -r name unit sed_ check; do
    [ -n "$name" ] || continue
    bm_run "$name" "$unit" "$sed_" "$check"
done < "$d/rules"

if [ "$bm_bad" = 0 ]; then
    echo "bsmut: every mutation was caught by the check named for it"
    exit 0
fi
echo "bsmut: a mutation was not caught -- see the lines above"
exit 1
