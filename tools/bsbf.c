/* bsbf — is this still brainfuck?
 *
 * One question, and it is the project's central claim made mechanical: a
 * committed fixture must contain ONLY the eight instructions and whitespace.
 * No ';' comments, no '#' comments, no dialect anything.
 *
 * NO OTHER TIER CAN SEE THIS. Every other check asks what a fixture
 * ACHIEVES, and a fixture that achieved it using an extension would pass all
 * of them -- it would talk to the broker perfectly, and be unrunnable under
 * any interpreter but the one it was written against. The whole project is
 * the claim that this does not happen, so the claim gets a checker.
 *
 * Why the sibling's rule does not transfer. bfsodium permits ';' comments
 * and proves portability by comparing the instruction stream with comments
 * honoured against the stream with only command bytes kept. That works when
 * the FILE is the product and a human reads it. Here the file is generated
 * from a .poke that carries all the prose, so the committed brainfuck has no
 * reason to contain anything else -- and the stricter rule is both simpler
 * to check and impossible to get subtly wrong.
 *
 * Usage:
 *   bsbf FILE...      check; exit 1 if any file is not pure brainfuck
 *   bsbf --selftest   prove it complains at each divergence
 */
#include <stdio.h>
#include <string.h>

static int is_cmd(int c) {
    return c == '>' || c == '<' || c == '+' || c == '-'
        || c == '.' || c == ',' || c == '[' || c == ']';
}

static int is_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Returns 0 clean, 1 for a file that is not pure brainfuck. */
static int scan(const char *path, int quiet) {
    FILE *f = fopen(path, "rb");
    int c, bad = 0;
    long line = 1, col = 0;
    long depth = 0, cmds = 0;

    if (!f) { if (!quiet) printf("FAIL %s: cannot read\n", path); return 1; }

    while ((c = fgetc(f)) != EOF) {
        col++;
        if (c == '\n') { line++; col = 0; continue; }
        if (is_space(c)) continue;
        if (!is_cmd(c)) {
            if (!quiet)
                printf("FAIL %s:%ld:%ld: '%c' is not a brainfuck instruction;\n"
                       "     a committed fixture carries only the eight bytes and whitespace,\n"
                       "     because the prose belongs in the .poke it was generated from\n",
                       path, line, col, c);
            bad = 1;
            break;
        }
        cmds++;
        if (c == '[') depth++;
        if (c == ']') {
            depth--;
            if (depth < 0) {
                if (!quiet) printf("FAIL %s:%ld:%ld: a ']' with no '['\n", path, line, col);
                bad = 1;
                break;
            }
        }
    }
    fclose(f);

    if (!bad && depth != 0) {
        if (!quiet) printf("FAIL %s: %ld unclosed '['\n", path, depth);
        bad = 1;
    }
    /* An empty fixture is almost certainly a generation failure rather than
     * an intentional no-op, and it would pass every other check silently. */
    if (!bad && cmds == 0) {
        if (!quiet) printf("FAIL %s: no instructions at all\n", path);
        bad = 1;
    }
    if (!bad && !quiet) printf("PASS %s\n", path);
    return bad;
}

static int wr(const char *path, const char *s) {
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    fputs(s, f);
    fclose(f);
    return 0;
}

/* Both polarities: a clean fixture must PASS, so the rule is a position and
 * not a ban, and each defect must be caught. */
static int selftest(void) {
    const char *p = "/tmp/bsbf-selftest.bf";
    int fails = 0;

    struct { const char *name; const char *body; int want; } cases[] = {
        { "a clean fixture",                 "+++.,,[->+<]\n",        0 },
        { "a semicolon comment",             "+++. ; a remark\n",     1 },
        { "a hash comment",                  "+++.\n# a remark\n",    1 },
        { "a dialect instruction",           "+++.!,,\n",             1 },
        { "an unmatched open bracket",       "[+++.\n",               1 },
        { "an unmatched close bracket",      "+++.]\n",               1 },
        { "an empty file",                   "\n",                    1 },
        { "whitespace is fine",              "  +++ .\t,\n,\n",       0 },
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int got;
        if (wr(p, cases[i].body)) { printf("SELFTEST FAIL: cannot write a case\n"); return 1; }
        got = scan(p, 1);
        if (got == cases[i].want) printf("selftest ok: %s\n", cases[i].name);
        else { printf("SELFTEST FAIL: %s (wanted %d got %d)\n", cases[i].name, cases[i].want, got); fails++; }
    }
    remove(p);
    if (fails) { printf("SELFTEST FAILED (%d)\n", fails); return 1; }
    printf("bsbf --selftest: ok\n");
    return 0;
}

int main(int argc, char **argv) {
    int bad = 0, i;
    if (argc < 2) { fprintf(stderr, "usage: %s FILE... | --selftest\n", argv[0]); return 2; }
    if (strcmp(argv[1], "--selftest") == 0) return selftest();
    for (i = 1; i < argc; i++) bad |= scan(argv[i], 0);
    return bad;
}
