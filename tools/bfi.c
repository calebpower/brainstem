/*
 * bfi.c — the pinned reference brainfuck interpreter.
 *
 * VENDORED from bfsodium at commit 8edf0f6, where tools/bfi.c last changed in
 * 59b45b0. It is a TEST FIXTURE
 * and nothing else: brainstem embeds no interpreter and hard depends on none.
 * The shipped broker takes --interp and defaults to `bfi` on PATH, never to a
 * path inside this repo. Any conforming interpreter will do, which is the
 * entire point of the portability claim.
 *
 * THERE USED TO BE THREE DELTAS AND THERE ARE NOW TWO.
 *
 * The one that went was the important one: setvbuf(stdout, NULL, _IONBF, 0).
 * Upstream called putchar() with no setvbuf and fflush()ed once, after the
 * program had ended -- so over a pipe not one byte of a request reached the
 * broker until the program terminated, by which time the program was already
 * blocked on ',' awaiting a reply that could not come. Silent, total deadlock
 * whose only symptom is a hang, and the entire reason brainstem states
 * requirement I1 and ships --check-interpreter.
 *
 * It was proposed upstream, accepted, and landed in bfsodium 59b45b0. Both
 * copies have the line now, so it is no longer a difference between them --
 * which is what a fix going home looks like, and is worth noticing rather than
 * quietly renumbering.
 *
 * TWO DELTAS FROM UPSTREAM REMAIN, both additive, both here, and neither has
 * any reason to go upstream: they exist to test a broker bfsodium does not
 * have.
 *
 *   1. BFI_EOF, which selects what ',' does at end of input.
 *      Brainfuck does not specify this and real interpreters disagree three
 *      ways. brainstem's claim is that its programs work under ANY conforming
 *      interpreter, so the suite runs every fixture under all three. Upstream
 *      has only the first, which is this file's default, so a run with the
 *      variable unset behaves exactly as bfsodium's does.
 *
 *   2. BFI_FLUSH=block, which puts the stdout buffering defect BACK.
 *      A deadlock the suite cannot reproduce is a deadlock that comes back.
 *      This knob is how the liveness tier proves the broker diagnoses a
 *      buffering interpreter instead of hanging behind one.
 *
 * Everything else is upstream's, including the frozen semantics below. Two
 * copies of an interpreter in two repositories can drift and no check inside
 * this repository can detect it; HANDOFF records that as a known soft spot and
 * the mitigation is a periodic manual diff against bfsodium.
 *
 * Frozen semantics (see bfsodium CONVENTIONS.md):
 *   - cells are unsigned 8-bit and wrap mod 256 (relied upon as byte arithmetic);
 *   - the tape is unbounded to the right, grown on demand and zero-filled;
 *   - moving left of cell 0 is a hard error, surfaced rather than hidden;
 *   - ',' at end-of-input leaves the current cell UNCHANGED, unless BFI_EOF
 *     says otherwise;
 *   - '.' and ',' are raw bytes — no newline translation, no encoding;
 *   - a ';' starts a comment that runs to end of line, command bytes included.
 *     Outside a comment, every byte that is not one of ><+-.,[] is ignored.
 *
 * Usage:  bfi program.bf < input > output
 * Env:    BFI_CONTRACTS=1   check ASSERT contracts (exit 4 on violation)
 *         BFI_COUNT=1       print the instruction count to stderr
 *         BFI_EOF=unchanged ',' at EOF leaves the cell alone (default)
 *         BFI_EOF=zero      ',' at EOF stores 0
 *         BFI_EOF=minus1    ',' at EOF stores 255
 *         BFI_FLUSH=byte    stdout unbuffered (default)
 *         BFI_FLUSH=block   stdout block buffered -- reproduces the deadlock
 * Exit:   0 ok; 2 usage/parse/OOM; 3 pointer moved left of cell 0;
 *         4 a contract assertion failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char *msg) { fprintf(stderr, "bfi: %s\n", msg); exit(2); }

/* What ',' does when there is no more input. */
enum { EOF_UNCHANGED = 0, EOF_ZERO = 1, EOF_MINUS1 = 2 };

static int eof_mode(void) {
    const char *m = getenv("BFI_EOF");
    if (!m || strcmp(m, "unchanged") == 0) return EOF_UNCHANGED;
    if (strcmp(m, "zero") == 0) return EOF_ZERO;
    if (strcmp(m, "minus1") == 0) return EOF_MINUS1;
    fprintf(stderr, "bfi: BFI_EOF must be unchanged, zero or minus1\n");
    exit(2);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s program.bf\n", argv[0]); return 2; }

    /* Upstream's setvbuf, wrapped by delta 2 so the suite can undo it. A
     * brokered conversation is request then response, so a buffered byte is a
     * byte the far end is already waiting for. */
    {
        const char *fl = getenv("BFI_FLUSH");
        if (fl && strcmp(fl, "block") == 0) {
            static char blockbuf[4096];
            setvbuf(stdout, blockbuf, _IOFBF, sizeof blockbuf);
        } else if (!fl || strcmp(fl, "byte") == 0) {
            setvbuf(stdout, NULL, _IONBF, 0);
        } else {
            fprintf(stderr, "bfi: BFI_FLUSH must be byte or block\n");
            return 2;
        }
    }
    int at_eof = eof_mode();

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("bfi: open program"); return 2; }

    /* Contract assertions. A routine states where the pointer must be and
     * which cells must be clear at a given point; those statements live in
     * comments, so a plain interpreter ignores them, and BFI_CONTRACTS makes
     * this one check them EVERY time execution reaches that point, which is
     * what turns an interface from a comment that might lie into a fact. */
    struct Assertion { int kind; size_t a, b; size_t line; };
    struct Assertion *asr = NULL; size_t nasr = 0, casr = 0;
    size_t *astart = NULL, *acount = NULL;
    size_t pend_first = 0, pend_n = 0;
    int contracts = (getenv("BFI_CONTRACTS") != NULL);

    /* Load the program, keeping only command bytes (everything else is comment). */
    size_t cap = 1u << 16, n = 0;
    char *prog = malloc(cap);
    size_t *srcline = malloc(cap * sizeof *srcline);   /* for diagnostics */
    astart = malloc(cap * sizeof *astart);
    acount = malloc(cap * sizeof *acount);
    if (!prog || !srcline || !astart || !acount) die("out of memory");
    int ch, in_comment = 0;
    size_t line = 1;
    char cbuf[512]; size_t clen = 0;
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '\n') {
            if (in_comment) {
                cbuf[clen] = 0;
                /* "; ASSERT ptr=N" and "; ASSERT zero A:B" attach to the NEXT
                 * instruction, so they are checked wherever execution reaches
                 * it, including on every pass through a loop. */
                char *s = cbuf; while (*s == ' ') s++;
                if (strncmp(s, "ASSERT ", 7) == 0) {
                    s += 7; while (*s == ' ') s++;
                    struct Assertion na; na.line = line; na.a = na.b = 0; na.kind = -1;
                    if (strncmp(s, "ptr=", 4) == 0) { na.kind = 0; na.a = (size_t)strtoul(s + 4, NULL, 10); }
                    else if (strncmp(s, "zero ", 5) == 0) {
                        char *colon = strchr(s + 5, ':');
                        if (colon) { na.kind = 1; na.a = (size_t)strtoul(s + 5, NULL, 10);
                                     na.b = (size_t)strtoul(colon + 1, NULL, 10); }
                    }
                    if (na.kind >= 0) {
                        if (nasr == casr) { casr = casr ? casr * 2 : 64;
                                            asr = realloc(asr, casr * sizeof *asr);
                                            if (!asr) die("out of memory"); }
                        if (pend_n == 0) pend_first = nasr;
                        asr[nasr++] = na; pend_n++;
                    }
                }
            }
            clen = 0; line++; in_comment = 0; continue;
        }
        if (in_comment) { if (clen + 1 < sizeof cbuf) cbuf[clen++] = (char)ch; continue; }
        if (ch == ';') { in_comment = 1; clen = 0; continue; }
        if (ch=='>'||ch=='<'||ch=='+'||ch=='-'||ch=='.'||ch==','||ch=='['||ch==']') {
            if (n == cap) {
                cap <<= 1;
                prog = realloc(prog, cap);
                srcline = realloc(srcline, cap * sizeof *srcline);
                astart = realloc(astart, cap * sizeof *astart);
                acount = realloc(acount, cap * sizeof *acount);
                if (!prog || !srcline || !astart || !acount) die("out of memory");
            }
            srcline[n] = line;
            astart[n] = pend_first; acount[n] = pend_n; pend_first = 0; pend_n = 0;
            prog[n++] = (char)ch;
        }
    }
    fclose(f);

    /* Precompute matching-bracket jumps so loops are O(1) to skip. */
    size_t *jump = malloc(n * sizeof *jump);
    size_t *stk  = malloc(n * sizeof *stk);
    if ((n && !jump) || (n && !stk)) die("out of memory");
    size_t sp = 0;
    for (size_t i = 0; i < n; i++) {
        if (prog[i] == '[') stk[sp++] = i;
        else if (prog[i] == ']') {
            if (sp == 0) die("unmatched ]");
            size_t o = stk[--sp];
            jump[o] = i; jump[i] = o;
        }
    }
    if (sp != 0) die("unmatched [");
    free(stk);

    /* Tape: unbounded to the right, grown on demand and zero-filled. */
    size_t tcap = 1u << 16;
    unsigned char *tape = calloc(tcap, 1);
    if (!tape) die("out of memory");
    size_t p = 0;
    unsigned long long steps = 0;

    for (size_t ip = 0; ip < n; ip++) {
        steps++;
        if (contracts && acount[ip]) {
            for (size_t k = 0; k < acount[ip]; k++) {
                struct Assertion *A = &asr[astart[ip] + k];
                if (A->kind == 0 && p != A->a) {
                    fprintf(stderr, "bfi: %s:%zu: CONTRACT pointer is at %zu  expected %zu\n",
                            argv[1], A->line, p, A->a);
                    return 4;
                }
                if (A->kind == 1) {
                    for (size_t q = A->a; q <= A->b && q < tcap; q++)
                        if (tape[q]) {
                            fprintf(stderr, "bfi: %s:%zu: CONTRACT cell %zu should be clear  holds %u\n",
                                    argv[1], A->line, q, (unsigned)tape[q]);
                            return 4;
                        }
                }
            }
        }
        switch (prog[ip]) {
            case '>':
                if (++p == tcap) {
                    size_t old = tcap; tcap <<= 1;
                    unsigned char *t = realloc(tape, tcap);
                    if (!t) die("out of memory (tape)");
                    tape = t; memset(tape + old, 0, tcap - old);
                }
                break;
            case '<':
                if (p == 0) { fprintf(stderr, "bfi: %s:%zu: pointer moved left of cell 0\n", argv[1], srcline[ip]); return 3; }
                --p;
                break;
            case '+': tape[p]++; break;            /* wraps mod 256 */
            case '-': tape[p]--; break;            /* wraps mod 256 */
            case '.': putchar(tape[p]); break;
            case ',': {                            /* delta 1 is the else arm */
                int in = getchar();
                if (in != EOF) tape[p] = (unsigned char)in;
                else if (at_eof == EOF_ZERO) tape[p] = 0;
                else if (at_eof == EOF_MINUS1) tape[p] = 255;
            } break;
            case '[': if (tape[p] == 0) ip = jump[ip]; break;
            case ']': if (tape[p] != 0) ip = jump[ip]; break;
        }
    }

    if (getenv("BFI_COUNT")) fprintf(stderr, "bfi: %llu instructions executed\n", (unsigned long long)steps);
    fflush(stdout);
    free(tape); free(jump); free(prog);
    return 0;
}
