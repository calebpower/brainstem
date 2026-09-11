#!/bin/sh
# bstier.sh — HANDOFF.md's tier status table, checked against the suite.
#
# CONVENTIONS section 8 says which tiers the project REQUIRES and why. That is
# a slow moving, normative claim and it is a person's to write. HANDOFF.md says
# which of them are BUILT, and that is fast moving fact, so it is this script's
# to write instead.
#
# The two were one table in the sibling project until tier 6 sat in it in the
# same voice as the tiers that ran, and a reader seeing that beside a green
# suite concluded there was fuzz coverage. There was none. A table describing
# what you want and what you have in one column drifts the moment those
# differ, and brainstem declares eighteen tiers and has built eleven, so here
# they differ almost everywhere.
#
# WHY SH AND AWK RATHER THAN PERL. The first version of this was perl, ported
# from the sibling, and it failed on freebsd-15.1 -- twice, once for itself
# and once for the check it performs. FreeBSD has not shipped perl in base for
# years, reaper's guests are ephemeral and its templates are bare, and
# guest-setup's FreeBSD branch deliberately installs nothing. Adding
# "pkg install perl5" would have fixed the symptom and given the primary
# platform a dependency it does not need. Removing the dependency is better,
# and it keeps the FreeBSD branch's verify-only property true.
#
# Only POSIX awk is used: no strtonum, no gensub, no delete on a whole array,
# no length() of an array. The sibling's gawk habit already cost this project
# one bug in bfgen.
#
# HOW A TIER IS FOUND. tests/run.sh carries "# TIER <ids>" markers. A marker
# claims every assertion after it, until the next marker, for each id it
# names. One marker may name several tiers where the suite genuinely
# interleaves them. A grep pattern per tier was the alternative and was
# rejected: the pattern would live in the document, so the suite could stop
# testing something while the document went on matching a line that no longer
# ran. The marker lives where the assertions are.
#
# THREE STATES, because two were not enough:
#   yes     automated; at least one assertion under a marker naming it
#   manual  a practice rather than a check, so it has no marker and must not
#           have one; tier 11 (mutation) is the case
#   no      declared in CONVENTIONS and not built
#
# Usage:
#   sh tools/bstier.sh              check; exit 1 if the table disagrees
#   sh tools/bstier.sh --fix        rewrite the counts in HANDOFF.md
#   sh tools/bstier.sh --selftest   prove it complains at each divergence
set -eu

CONV=CONVENTIONS.md
DOC=HANDOFF.md
SUITE=tests/run.sh

# The shared awk program. Reads the three files in a fixed order and
# cross-references them at the end. Order matters and is set by the caller.
_bst_awk='
FILENAME == conv {
    if ($0 ~ /^## 8\. /) { in8 = 1; next }
    if (in8 && $0 ~ /^### Named non-goals/) { in8 = 0 }
    if (in8 && $0 ~ /^\| *[0-9]+[a-z]? +[^|]/) {
        line = $0
        sub(/^\| */, "", line)
        n = split(line, f, / /)
        id = f[1]
        if (!(id in want)) { want[id] = 1; order[++norder] = id }
    }
    next
}
FILENAME == suite {
    if ($0 ~ /^#[ \t]*TIER[ \t]/) {
        t = $0
        sub(/^#[ \t]*TIER[ \t]*/, "", t)
        sub(/[ \t]*$/, "", t)
        ncur = split(t, cur, /[ \t]+/)
        for (i = 1; i <= ncur; i++) if (!(cur[i] in seen)) { seen[cur[i]] = 1; count[cur[i]] = 0 }
        next
    }
    if (ncur == 0) next
    # an assertion is a run or dk invocation, at the start of a line or after
    # a semicolon or a do -- a for loop is ONE source line and many checks
    if ($0 ~ /(^|;|do )[ \t]*run[ \t]/ || $0 ~ /(^|;|do )[ \t]*dk[ \t]/) {
        for (i = 1; i <= ncur; i++) count[cur[i]]++
    }
    next
}
FILENAME == doc {
    if ($0 ~ /^\| tier \| built \| run\.sh lines \|/) { intab = 1; next }
    if (!intab) next
    if ($0 ~ /^\|[ \t]*-+/) next
    if ($0 !~ /^\|/) { intab = 0; next }
    line = $0
    nf = split(line, g, /\|/)
    id = g[2]; b = g[3]; l = g[4]
    gsub(/[ \t]/, "", id); gsub(/[ \t]/, "", b); gsub(/[ \t]/, "", l)
    if (id == "") next
    row[id] = b
    rowlines[id] = l
    if (!(id in rowseen)) { rowseen[id] = 1; roworder[++nrow] = id }
    next
}
END {
    bad = 0
    for (i = 1; i <= norder; i++) {
        id = order[i]
        if (!(id in rowseen)) {
            printf "FAIL %s: tier %s is required by %s and has no status row\n", doc, id, conv
            bad = 1
        }
    }
    for (i = 1; i <= nrow; i++) {
        id = roworder[i]
        if (!(id in want)) {
            printf "FAIL %s: tier %s has a status row and is required by nothing\n", doc, id
            bad = 1
            continue
        }
        b = row[id]
        c = (id in count) ? count[id] : 0
        if (b != "yes" && b != "no" && b != "manual") {
            printf "FAIL %s: tier %s has built=%s; it must be yes, no or manual\n", doc, id, b
            bad = 1
            continue
        }
        if (b == "yes") {
            if (c == 0) {
                printf "FAIL %s: tier %s says built and no assertion in %s claims it;\n", doc, id, suite
                printf "     add a \"# TIER %s\" marker above the checks that serve it\n", id
                bad = 1
            } else if (rowlines[id] + 0 != c) {
                printf "FAIL %s: tier %s says %s run.sh lines; the suite has %d\n", doc, id, rowlines[id], c
                bad = 1
            }
        } else {
            if (c > 0) {
                printf "FAIL %s: tier %s says %s and %s has %d assertion(s) under a\n", doc, id, b, suite, c
                printf "     marker claiming it; the tier got built and nobody said so\n"
                bad = 1
            }
            if (rowlines[id] != "0") {
                printf "FAIL %s: tier %s is not built, so its line count must be 0\n", doc, id
                bad = 1
            }
        }
    }
    for (id in seen) {
        if (!(id in rowseen)) {
            printf "FAIL %s: a \"# TIER %s\" marker names a tier with no status row\n", suite, id
            bad = 1
        }
    }
    if (!bad) printf "bstier: %s tier table matches the suite (%d tiers)\n", doc, nrow
    exit bad
}
'

check() {
    _bst_d=${1:-.}
    awk -v conv="$_bst_d/$CONV" -v suite="$_bst_d/$SUITE" -v doc="$_bst_d/$DOC" \
        "$_bst_awk" "$_bst_d/$CONV" "$_bst_d/$SUITE" "$_bst_d/$DOC"
}

fix() {
    # The -v options come BEFORE the program text. POSIX awk takes operands
    # after the program, so "awk 'prog' -v x=1 file" treats -v and x=1 as
    # FILENAMES and then fails to open them. The first draft of this function
    # had it the wrong way round and was never run, because the suite only
    # calls check -- which is exactly how an untested code path stays broken.
    awk -v suite="$SUITE" -v doc="$DOC" '
    FILENAME == suite {
        if ($0 ~ /^#[ \t]*TIER[ \t]/) {
            t = $0; sub(/^#[ \t]*TIER[ \t]*/, "", t); sub(/[ \t]*$/, "", t)
            ncur = split(t, cur, /[ \t]+/)
            for (i = 1; i <= ncur; i++) count[cur[i]] += 0
            next
        }
        if (ncur == 0) next
        if ($0 ~ /(^|;|do )[ \t]*run[ \t]/ || $0 ~ /(^|;|do )[ \t]*dk[ \t]/)
            for (i = 1; i <= ncur; i++) count[cur[i]]++
        next
    }
    FILENAME == doc {
        if ($0 ~ /^\| tier \| built \| run\.sh lines \|/) { intab = 1; print; next }
        if (!intab || $0 !~ /^\|/ || $0 ~ /^\|[ \t]*-+/) { if ($0 !~ /^\|/) intab = 0; print; next }
        nf = split($0, g, /\|/)
        id = g[2]; b = g[3]
        gsub(/[ \t]/, "", id); gsub(/[ \t]/, "", b)
        if (id == "") { print; next }
        c = (b == "yes" && (id in count)) ? count[id] : 0
        rest = ""
        for (i = 5; i <= nf; i++) rest = rest "|" g[i]
        printf "| %s | %s | %d %s\n", id, b, c, rest
        next
    }
    ' "$SUITE" "$DOC" > "$DOC.new"
    mv "$DOC.new" "$DOC"
    echo "bstier: $DOC rewritten"
}

# Self-test: both polarities, because a checker that has never been observed
# failing is indistinguishable from a clean corpus.
selftest() {
    _st_fails=0
    _st_n=0
    _st_here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
    _st_root=$(mktemp -d)
    trap 'rm -rf "$_st_root"' EXIT INT TERM

    _st_case() {   # _st_case NAME WANT CONVEXTRA ROWS SUITEBODY
        _c_name=$1; _c_want=$2; _c_conv=$3; _c_rows=$4; _c_suite=$5
        # a counter rather than a name derived with tr -c, whose treatment of
        # complemented ranges is one of the BSD against GNU differences this
        # script exists partly because of
        _st_n=$((_st_n + 1))
        _c_d="$_st_root/case$_st_n"
        mkdir -p "$_c_d/tests"
        {   printf '## 8. Testing protocol\n\n| Tier | Q | How |\n|---|---|---|\n'
            printf '| 1 interpreter self-test | a | b |\n'
            printf '| 6 differential fuzz | a | b |\n'
            printf '| 11 mutation | a | b |\n'
            printf '%s' "$_c_conv"
            printf '\n### Named non-goals\n'
        } > "$_c_d/$CONV"
        {   printf '#!/bin/sh\n'; printf '%s' "$_c_suite"; } > "$_c_d/$SUITE"
        {   printf '## State\n\n| tier | built | run.sh lines | note |\n|---|---|---|---|\n'
            printf '%s' "$_c_rows"
            printf '\ntail\n'
        } > "$_c_d/$DOC"
        if check "$_c_d" >/dev/null 2>&1; then _c_got=0; else _c_got=1; fi
        if [ "$_c_got" = "$_c_want" ]; then echo "selftest ok: $_c_name"
        else echo "SELFTEST FAIL: $_c_name (wanted $_c_want got $_c_got)"; _st_fails=$((_st_fails+1)); fi
    }

    _good_rows='| 1 | yes | 2 | x |
| 6 | no | 0 | x |
| 11 | manual | 0 | x |
'
    _good_suite='# TIER 1
run "a" true
run "b" true
'

    _st_case "a clean tree passes" 0 "" "$_good_rows" "$_good_suite"
    _st_case "a tier required with no status row" 1 '| 7 metamorphic | a | b |
' "$_good_rows" "$_good_suite"
    _st_case "a status row for a tier nobody requires" 1 "" '| 1 | yes | 2 | x |
| 6 | no | 0 | x |
| 11 | manual | 0 | x |
| 42 | yes | 1 | x |
' "$_good_suite"
    _st_case "a tier claiming built with nothing behind it" 1 "" '| 1 | yes | 2 | x |
| 6 | yes | 1 | x |
| 11 | manual | 0 | x |
' "$_good_suite"
    _st_case "a stale line count" 1 "" '| 1 | yes | 9 | x |
| 6 | no | 0 | x |
| 11 | manual | 0 | x |
' "$_good_suite"
    _st_case "a tier built while the table still said no" 1 "" "$_good_rows" '# TIER 1
run "a" true
run "b" true
# TIER 6
run "c" true
'
    _st_case "a marker naming a tier with no status row" 1 "" "$_good_rows" '# TIER 1
run "a" true
run "b" true
# TIER 99
run "c" true
'
    _st_case "an unknown built state" 1 "" '| 1 | maybe | 2 | x |
| 6 | no | 0 | x |
| 11 | manual | 0 | x |
' "$_good_suite"
    _st_case "a loop counts as one source line" 0 "" '| 1 | yes | 1 | x |
| 6 | no | 0 | x |
| 11 | manual | 0 | x |
' '# TIER 1
for f in a b c; do run "x $f" true; done
'

    # --fix has its own case, because it had a bug for its whole existence
    # and nothing noticed: the suite only ever calls check, so the repair path
    # was never executed. A stale count must become correct, and a correct
    # table must come out byte for byte unchanged.
    _st_n=$((_st_n + 1))
    _f_d="$_st_root/case$_st_n"
    mkdir -p "$_f_d/tests"
    {   printf '## 8. Testing protocol

| Tier | Q | How |
|---|---|---|
'
        printf '| 1 interpreter self-test | a | b |
'
        printf '| 6 differential fuzz | a | b |
'
        printf '| 11 mutation | a | b |

### Named non-goals
'
    } > "$_f_d/$CONV"
    printf '#!/bin/sh
%s' "$_good_suite" > "$_f_d/$SUITE"
    {   printf '## State

| tier | built | run.sh lines | note |
|---|---|---|---|
'
        printf '| 1 | yes | 99 | x |
| 6 | no | 0 | x |
| 11 | manual | 0 | x |

tail
'
    } > "$_f_d/$DOC"
    if check "$_f_d" >/dev/null 2>&1; then
        echo "SELFTEST FAIL: a stale count was not caught before --fix"; _st_fails=$((_st_fails+1))
    fi
    ( cd "$_f_d" && sh "$_st_here/bstier.sh" --fix >/dev/null 2>&1 ) || true
    if check "$_f_d" >/dev/null 2>&1; then echo "selftest ok: --fix repairs a stale count"
    else echo "SELFTEST FAIL: --fix did not repair a stale count"; _st_fails=$((_st_fails+1)); fi
    cp "$_f_d/$DOC" "$_f_d/doc.again"
    ( cd "$_f_d" && sh "$_st_here/bstier.sh" --fix >/dev/null 2>&1 ) || true
    if cmp -s "$_f_d/$DOC" "$_f_d/doc.again"; then echo "selftest ok: --fix is idempotent"
    else echo "SELFTEST FAIL: --fix is not idempotent"; _st_fails=$((_st_fails+1)); fi

    if [ "$_st_fails" -gt 0 ]; then echo "SELFTEST FAILED ($_st_fails)"; return 1; fi
    echo "bstier --selftest: ok"
    return 0
}

case "${1-}" in
    --selftest) selftest ;;
    --fix)      fix ;;
    '')         check . ;;
    *)          echo "usage: sh tools/bstier.sh [--fix|--selftest]" >&2; exit 2 ;;
esac
