#!/bin/sh
# bspoke.sh -- TIER 3b: the header does not lie.
#
# A committed .bf carries no comments, by design: tier 2 requires it to be
# nothing but the eight instructions, so the skeleton is the only artifact a
# reviewer can read. Tier 3 proves the .bf is the expansion of the skeleton
# and tier 2 proves it is brainfuck. NEITHER CAN SEE WHETHER THE SKELETON'S
# PROSE DESCRIBES ITS HEX. If a header says "open" above a frame that stats,
# or claims a length the payload does not have, every tier in this suite
# passes and the one thing a human would have caught is the thing nobody can.
#
# That gap was declared at M2 and left open until M7. This closes it.
#
# THE SPLIT, which is the same one bfgen has. tools/bsframe reads a skeleton
# and reports what its frames ARE -- opcode, declared length, bytes actually
# emitted, how many bytes are read back, and the comment attached. It knows
# no op names and no ABI. This script holds the KNOWLEDGE: it reads the op
# table out of `brainstem --dump-abi` and the statuses out of src/errs.def,
# and asks whether the prose agrees with them. Mechanise the drudgery, never
# the knowledge.
#
# Six rules:
#
#   P1  a frame declares the number of bytes it actually emits
#   P2  every frame is annotated, unless it repeats the opcode above it
#   P3  the annotation names the op the frame emits
#   P4  an annotation naming a status names that status's real code, and the
#       frame then reads exactly three bytes
#   P5  the frames this tier cannot read are exactly the ones it should not
#   P6  exactly one frame disagrees with the op table's arity
#
# P5 and P6 are PINS ON THE TREE rather than on a platform, and both exist
# because the alternative was an exemption. An exemption rots quietly: it
# keeps passing after the thing it excused has changed. A pin fails the day
# somebody adds a second one, which is the day to think about it.
#
# Usage:  sh tools/bspoke.sh [--selftest]
# Exit:   0 every rule holds; 1 a rule does not; 2 the tool could not run.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# P5. bf/proc/echo.poke is the one fixture in the tree that speaks no
# protocol at all -- it is ",[.,]", the classic brainfuck cat, and it exists
# to be run BY another brainfuck program through spawn. It emits no frames,
# so there is nothing here for this tier to read, and bsframe reports that
# honestly rather than pretending it found one.
UNREADABLE="bf/proc/echo.poke"

# P6. bf/ctl/badarity.poke declares a hello one byte short ON PURPOSE: it is
# the fixture that proves the dispatcher checks the length against the op
# table BEFORE the handler runs. It is the only frame in the tree the table
# would refuse, and pinning that it is the only one is what stops a second,
# accidental one hiding behind the first.
BADARITY="bf/ctl/badarity.poke"

bp_die() { echo "bspoke: $*" >&2; exit 2; }

# ---- the three inputs ------------------------------------------------------
#
# Written to files rather than passed in variables because awk reads them as
# tables, and because a failure to produce one must be loud rather than
# arriving as an empty map that makes every rule vacuously true.

bp_gather() {  # bp_gather DIR  -- opnames, errnames and frames into DIR
    _bp_d=$1

    [ -x "$repo/build/brainstem" ] || bp_die "build/brainstem is not built"
    [ -x "$repo/build/bsframe" ]   || bp_die "build/bsframe is not built"

    # code, lowercase name, exact request length or "var".
    #
    # 65535 is BS_VAR in ops.def -- the same u16 that is the maximum payload,
    # which is why a variable-length op cannot be told from a maximal one by
    # its number alone and is spelled out here instead.
    "$repo/build/brainstem" --dump-abi \
        | awk '{
            n = $2; nm = tolower($3);
            q = $5; sub(/^req=/, "", q);
            if (q + 0 == 65535) q = "var";
            print n, nm, q;
        }' > "$_bp_d/opnames"
    [ -s "$_bp_d/opnames" ] || bp_die "--dump-abi produced no op table"

    # NAME and code, from the X-macro table itself rather than from a copy.
    sed -n 's/^X(0x\([0-9A-Fa-f][0-9A-Fa-f]\), *\([A-Z0-9_]*\),.*/\2 \1/p' \
        "$repo/src/errs.def" | tr 'A-F' 'a-f' > "$_bp_d/errnames"
    [ -s "$_bp_d/errnames" ] || bp_die "src/errs.def produced no status table"

    : > "$_bp_d/frames"
    for _bp_s in "$repo"/bf/*/*.poke; do
        _bp_rel=${_bp_s#"$repo"/}
        "$repo/build/bsframe" skeleton "$_bp_s" > "$_bp_d/one" \
            || bp_die "bsframe could not read $_bp_rel"
        sed "s|^FRAME |$_bp_rel |" "$_bp_d/one" >> "$_bp_d/frames"
    done
    [ -s "$_bp_d/frames" ] || bp_die "no skeletons found under bf/"
}

# ---- the rules -------------------------------------------------------------
#
# One awk over the frame table, because every rule is a question about the
# same seven fields and a trailing run of prose. Fields: file, frame index,
# first line, opcode, declared length, bytes emitted, bytes read, then the
# comment attached to the frame.

bp_rules() {  # bp_rules DIR UNREADABLE BADARITY -- 0 all hold, 1 one does not
    awk -v opf="$1/opnames" -v errf="$1/errnames" \
        -v unreadable="$2" -v badarity="$3" '
    FILENAME == opf  { opname[$1] = $2; arity[$1] = $3; next }
    FILENAME == errf { errcode[$1] = $2; next }

    {
        file = $1; idx = $2; line = $3; op = $4;
        dec = $5; act = $6; rd = $7;
        where = file " frame " idx " (line " line ")";

        note = "";
        for (j = 8; j <= NF; j++) note = note (j > 8 ? " " : "") $j;
        annotated = (note != "-" && note != "");

        # P5 first: a frame this tool could not read cannot be asked
        # anything else, so it is counted and skipped rather than reported
        # against five rules it has no answers for.
        if (op == "??" || dec == "?" || act == "?") {
            if (!(file in unreadable_seen)) {
                unreadable_seen[file] = 1;
                unreadable_list = unreadable_list (unreadable_list ? " " : "") file;
            }
            next;
        }

        # P1
        if (dec + 0 != act + 0)
            p1 = p1 where ": declares " dec " bytes and emits " act "\n";

        # P2
        if (!annotated && prevkey != file " " op)
            p2 = p2 where ": no comment, and it does not repeat op " op "\n";
        prevkey = file " " op;

        # P3. Matched by index over a flattened copy rather than by regex:
        # the name has to be a WHOLE word, or readdir would satisfy read and
        # the rule would be half blind, and an anchor inside an alternation
        # is the kind of regex the three awks in this project disagree
        # about. Punctuation becomes a space, so "open" is found in
        # `open "f".` and "hello" is not found in "helloworld".
        if (annotated && (op in opname)) {
            nm = opname[op];
            flat = " " note " ";
            gsub(/[^A-Za-z0-9_]/, " ", flat);
            if (index(flat, " " nm " ") == 0)
                p3 = p3 where ": emits op " op " (" nm ") and its comment does not say so\n";
        }

        # P4. Scanning by FIELD rather than by regex capture: a status is
        # written as a name then its code, so the pair is two adjacent
        # words, and adjacency is something every awk agrees about.
        if (annotated) {
            n = split(note, w, /[ \t]+/);
            for (j = 1; j < n; j++) {
                t = w[j];
                gsub(/[^A-Za-z0-9_]/, "", t);
                if (!(t in errcode)) continue;
                if (w[j+1] !~ /^[0-9a-f][0-9a-f][,.]?$/) continue;
                c = w[j+1]; gsub(/[^0-9a-f]/, "", c);
                if (c != errcode[t])
                    p4 = p4 where ": says " t " " c ", and errs.def says " errcode[t] "\n";
                else if (rd != "3")
                    p4 = p4 where ": expects " t " but reads " rd " bytes, not 3\n";
            }
        }

        # P6
        if ((op in arity) && arity[op] != "var" && dec + 0 != arity[op] + 0) {
            arity_list = arity_list (arity_list ? " " : "") file;
            p6detail = p6detail where ": declares " dec " and the table says " arity[op] "\n";
        }
    }

    END {
        bad = 0;
        if (p1) { printf "bspoke: FAIL P1 a frame declares a length it does not emit\n%s", p1; bad = 1 }
        else printf "bspoke: ok P1 every frame declares the bytes it emits\n";

        if (p2) { printf "bspoke: FAIL P2 a frame has no comment\n%s", p2; bad = 1 }
        else printf "bspoke: ok P2 every frame is annotated, or repeats the op above it\n";

        if (p3) { printf "bspoke: FAIL P3 a comment names the wrong op\n%s", p3; bad = 1 }
        else printf "bspoke: ok P3 every comment names the op its frame emits\n";

        if (p4) { printf "bspoke: FAIL P4 a comment disagrees with errs.def\n%s", p4; bad = 1 }
        else printf "bspoke: ok P4 every status named is real, and read as three bytes\n";

        if (unreadable_list != unreadable) {
            printf "bspoke: FAIL P5 unreadable frames are in [%s], pinned as [%s]\n",
                   unreadable_list, unreadable;
            bad = 1;
        } else printf "bspoke: ok P5 the only skeleton with no readable frame is %s\n", unreadable;

        if (arity_list != badarity) {
            printf "bspoke: FAIL P6 frames disagreeing with the op table are in [%s], pinned as [%s]\n%s",
                   arity_list, badarity, p6detail;
            bad = 1;
        } else printf "bspoke: ok P6 the only frame the op table would refuse is in %s\n", badarity;

        exit bad;
    }
    ' "$1/opnames" "$1/errnames" "$1/frames"
}

# ---- both polarities -------------------------------------------------------
#
# Every rule above is a claim that something is NOT wrong, and a claim of that
# shape passes just as happily when the check is broken as when the tree is
# clean. So each rule is shown failing against a skeleton written to break it,
# once, before the real ones are read.

bp_selftest() {
    _d=$(mktemp -d)
    trap 'rm -rf "$_d"' EXIT
    mkdir -p "$_d/bf/bad"

    printf '%s\n' \
        '01 hello 10' '02 exit 1' '03 clock_now 1' '11 open var' > "$_d/opnames"
    printf '%s\n' 'NOENT 05' 'INVAL 06' > "$_d/errnames"

    _rc=0

    # P1: nine bytes under a declared ten
    printf '%s\n' 'bad.poke 1 1 01 10 9 3 hello  op 01' > "$_d/frames"
    bp_rules "$_d" "" "" > "$_d/out" 2>&1 || _rc=$?
    grep -q "FAIL P1" "$_d/out" || { echo "SELFTEST FAIL: P1 did not fire"; return 1; }

    # P2: a second op with no comment, not a repeat of the one above it
    printf '%s\n' 'bad.poke 1 1 01 10 10 3 hello  op 01' \
                  'bad.poke 2 3 02 1 1 3 -' > "$_d/frames"
    bp_rules "$_d" "" "" > "$_d/out" 2>&1 || true
    grep -q "FAIL P2" "$_d/out" || { echo "SELFTEST FAIL: P2 did not fire"; return 1; }

    # and a repeat of the op above it is NOT a violation, or the rule would
    # force three identical comments onto three consecutive closes
    printf '%s\n' 'bad.poke 1 1 02 1 1 3 exit  op 02' \
                  'bad.poke 2 3 02 1 1 3 -' > "$_d/frames"
    bp_rules "$_d" "" "" > "$_d/out" 2>&1 || true
    if grep -q "FAIL P2" "$_d/out"; then
        echo "SELFTEST FAIL: P2 refused a repeated op"; return 1
    fi

    # P3: the comment says open and the frame says hello
    printf '%s\n' 'bad.poke 1 1 01 10 10 3 open "f" -- the wrong op entirely' > "$_d/frames"
    bp_rules "$_d" "" "" > "$_d/out" 2>&1 || true
    grep -q "FAIL P3" "$_d/out" || { echo "SELFTEST FAIL: P3 did not fire"; return 1; }

    # and a name must not match inside a longer word, or readdir would
    # satisfy read and the rule would be half blind
    printf '%s\n' 'bad.poke 1 1 01 10 10 3 helloworld is not hello' > "$_d/frames"
    bp_rules "$_d" "" "" > "$_d/out" 2>&1 || true
    if grep -q "FAIL P3" "$_d/out"; then
        echo "SELFTEST FAIL: P3 matched inside a word"; return 1
    fi

    # P4, both halves: the wrong code, and a reply read as more than a status
    printf '%s\n' 'bad.poke 1 1 11 4 4 3 open "nope" -- NOENT 09' > "$_d/frames"
    bp_rules "$_d" "" "" > "$_d/out" 2>&1 || true
    grep -q "FAIL P4" "$_d/out" || { echo "SELFTEST FAIL: P4 did not fire on a wrong code"; return 1; }

    printf '%s\n' 'bad.poke 1 1 11 4 4 7 open "nope" -- NOENT 05' > "$_d/frames"
    bp_rules "$_d" "" "" > "$_d/out" 2>&1 || true
    grep -q "FAIL P4" "$_d/out" || { echo "SELFTEST FAIL: P4 did not fire on a long read"; return 1; }

    # P5: a frame nobody pinned as unreadable
    printf '%s\n' 'bad.poke 1 1 ?? ? ? ? -' > "$_d/frames"
    bp_rules "$_d" "" "" > "$_d/out" 2>&1 || true
    grep -q "FAIL P5" "$_d/out" || { echo "SELFTEST FAIL: P5 did not fire"; return 1; }

    # P6: a hello declaring nine against a table that says ten
    printf '%s\n' 'bad.poke 1 1 01 9 9 3 hello  op 01' > "$_d/frames"
    bp_rules "$_d" "" "" > "$_d/out" 2>&1 || true
    grep -q "FAIL P6" "$_d/out" || { echo "SELFTEST FAIL: P6 did not fire"; return 1; }

    # and a clean table must pass all six, or every rule above proves only
    # that this tool can say the word FAIL
    printf '%s\n' 'bad.poke 1 1 01 10 10 3 hello  op 01' \
                  'bad.poke 2 3 11 4 4 3 open "nope" -- NOENT 05' > "$_d/frames"
    if ! bp_rules "$_d" "" "" > "$_d/out" 2>&1; then
        echo "SELFTEST FAIL: a clean frame table did not pass"
        cat "$_d/out"
        return 1
    fi

    echo "selftest ok: all six rules fire on a skeleton written to break them"
    echo "selftest ok: and a clean frame table passes all six"
    echo "bspoke --selftest: ok"
    return 0
}

# ---- entry -----------------------------------------------------------------

case "${1:-}" in
    --selftest) bp_selftest; exit $? ;;
    "")         ;;
    *)          bp_die "usage: sh tools/bspoke.sh [--selftest]" ;;
esac

d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
bp_gather "$d"
bp_rules "$d" "$UNREADABLE" "$BADARITY"
