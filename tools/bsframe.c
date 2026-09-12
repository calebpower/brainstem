/* bsframe — a SECOND, INDEPENDENT implementation of the frame codec.
 *
 * It includes nothing from src/. Not frame.h, not err.h, not a shared hex
 * helper. Every line of it was written from ABI.md section 2 rather than
 * from the other implementation, and that independence is the entire reason
 * the file exists: the suite requires bsframe and bscodec to agree over every
 * pinned vector, and two implementations that shared an encoder would agree
 * about a byte order bug as readily as about anything else.
 *
 * This is the sibling project's two-oracle rule with the oracles swapped in.
 * bfsodium checks brainfuck against a Cryptol specification; brainstem has no
 * Cryptol, so the second opinion has to be a second reading of the document.
 *
 * If you are tempted to factor out the duplication between this file and
 * tools/bscodec.c: don't. The duplication IS the test.
 *
 * Usage, deliberately identical to bscodec:
 *   bsframe encode KINDHEX PAYLOADHEX
 *   bsframe decode FRAMEHEX
 *   bsframe --selftest
 *
 * And one mode bscodec does not have, because it is about skeletons rather
 * than about bytes already on a wire:
 *   bsframe skeleton FILE.poke
 *
 * Exit: 0 ok; 1 not a well formed frame; 2 usage.
 */
#include <stdio.h>
#include <string.h>

/* ABI.md section 2: "Request, program to broker: off 0 u8 op; off 1 u16 LE
 * len; off 3 len payload." And: "Max payload 65535." Written out here as
 * literals rather than shared constants, for the same reason as everything
 * else in this file. */
#define HDR  3
#define MAXP 65535

static unsigned char buf[HDR + MAXP];
static unsigned char pay[MAXP];

static int nyb(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static long tobytes(const char *s, unsigned char *dst, long cap) {
    long got = 0;
    int half = -1;
    while (*s) {
        int c = (unsigned char)*s++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        int v = nyb(c);
        if (v < 0) return -1;
        if (half < 0) { half = v; continue; }
        if (got >= cap) return -1;
        dst[got++] = (unsigned char)(half * 16 + v);
        half = -1;
    }
    return half < 0 ? got : -1;
}

static void show(const unsigned char *p, long n) {
    long i;
    for (i = 0; i < n; i++) printf("%02x", p[i]);
}

static int encode(const char *kh, const char *ph) {
    unsigned char kind[1];
    long n;
    if (tobytes(kh, kind, 1) != 1) { fprintf(stderr, "bsframe: KINDHEX is one hex byte\n"); return 2; }
    n = tobytes(ph, pay, MAXP);
    if (n < 0) { fprintf(stderr, "bsframe: PAYLOADHEX is not hex\n"); return 2; }

    /* the length is little endian: low byte first, then high */
    buf[0] = kind[0];
    buf[1] = (unsigned char)(n % 256);
    buf[2] = (unsigned char)((n / 256) % 256);
    {
        long i;
        for (i = 0; i < n; i++) buf[HDR + i] = pay[i];
    }
    show(buf, HDR + n);
    printf("\n");
    return 0;
}

static int decode(const char *fh) {
    long n = tobytes(fh, buf, HDR + MAXP);
    long declared, actual;
    if (n < 0) { fprintf(stderr, "bsframe: FRAMEHEX is not hex\n"); return 2; }
    if (n < HDR) { printf("ERR SHORTHDR\n"); return 1; }

    declared = (long)buf[1] + 256L * (long)buf[2];
    actual   = n - HDR;
    if (actual < declared) { printf("ERR SHORTBODY\n"); return 1; }
    if (actual > declared) { printf("ERR TRAILING\n"); return 1; }

    printf("kind=%02x len=%ld payload=", buf[0], declared);
    show(buf + HDR, declared);
    printf("\n");
    return 0;
}

/* ---- skeleton mode: the frames a .poke ACTUALLY emits ------------------- */
/*
 * TIER 3b, which was declared at M2 and has been missing ever since.
 *
 * Tier 3 proves a committed .bf is the expansion of its skeleton, and tier 2
 * proves it is brainfuck. Neither can see whether the skeleton's PROSE
 * describes what its hex does -- and the prose is the only review artifact
 * there is, because a .bf carries no comments by design. If a header lies,
 * nothing else in this tree disagrees with it.
 *
 * What this mode does is mechanical and deliberately ignorant. It groups
 * consecutive EMIT bytes into frames, reads the first three as an opcode and
 * a little-endian length, and prints what it found beside the comment lines
 * attached to that frame. IT KNOWS NO OP NAMES, no arities and no ABI. The
 * knowledge stays in tests/run.sh, which reads the op table out of
 * `brainstem --dump-abi` and the statuses out of src/errs.def and asks
 * whether the prose agrees with them. Same split as bfgen: mechanise the
 * drudgery, never the knowledge.
 *
 * Raw brainfuck inside a frame is COUNTED rather than refused. A '.' emits
 * exactly one byte whose value this tool cannot know but whose existence it
 * can count, which is what keeps bf/net/loopback.poke checkable -- there the
 * ephemeral port is carried in raw brainfuck through the middle of a connect
 * frame, because it is a value rather than a literal. A '[' or ']' makes the
 * count unknowable, and the frame is reported as unknowable rather than
 * guessed at.
 *
 * A comment line is attached to the frame that follows it only when it is
 * INDENTED: "#" then two or more spaces. That is the convention every fixture
 * already follows -- indented comments annotate frames, flush ones are the
 * file's prose. Without the distinction, a file header that happens to
 * mention six op names would satisfy the name check for the first frame
 * vacuously, which is the kind of check that passes without asking anything.
 */

#define SK_LINE 4096
#define SK_NOTE 8192

struct sk {
    /* the run being accumulated */
    int  open, startline, unknowable;
    long total;
    long firstunk;               /* offset of the first byte of unknown value */
    unsigned char head[HDR];
    /* the comment lines attached to it */
    char note[SK_NOTE];
    size_t notelen;
    /* what the last closed frame was, which is what the self test reads */
    int  count, last_ok, last_read;
    unsigned char last_op;
    long last_declared, last_actual;
    int  quiet;
};

static struct sk SK;

static void sk_reset(int quiet) {
    memset(&SK, 0, sizeof SK);
    SK.quiet = quiet;
    SK.firstunk = -1;
}

static void sk_num(long v, char *out) {
    if (v < 0) { out[0] = '?'; out[1] = 0; return; }
    sprintf(out, "%ld", v);
}

static void sk_close(int readn) {
    char d[24], a[24], r[24], o[8];
    if (!SK.open) return;
    SK.count++;
    SK.last_ok = (!SK.unknowable && SK.total >= HDR &&
                  (SK.firstunk < 0 || SK.firstunk >= HDR));
    if (SK.last_ok) {
        SK.last_op       = SK.head[0];
        SK.last_declared = (long)SK.head[1] + 256L * (long)SK.head[2];
    } else {
        SK.last_op = 0;
        SK.last_declared = -1;
    }
    SK.last_actual = SK.unknowable ? -1 : SK.total - HDR;
    SK.last_read   = readn;

    if (!SK.quiet) {
        sk_num(SK.last_declared, d);
        sk_num(SK.last_actual, a);
        sk_num(readn, r);
        if (SK.last_ok) sprintf(o, "%02x", SK.last_op);
        else            strcpy(o, "??");
        /* Fixed fields first and the free text last, so an awk reading this
         * can take $4 through $7 and treat everything after as prose. */
        printf("FRAME %d %d %s %s %s %s %s\n",
               SK.count, SK.startline, o, d, a, r,
               SK.notelen ? SK.note : "-");
    }

    SK.open = 0;
    SK.total = 0;
    SK.unknowable = 0;
    SK.firstunk = -1;
    SK.notelen = 0;
}

static void sk_byte(unsigned char v, int known, int lineno) {
    if (!SK.open) {
        SK.open = 1;
        SK.startline = lineno;
        SK.total = 0;
        SK.unknowable = 0;
        SK.firstunk = -1;
    }
    if (!known && SK.firstunk < 0) SK.firstunk = SK.total;
    if (SK.total < HDR) SK.head[SK.total] = v;
    SK.total++;
}

static void sk_addnote(const char *s) {
    if (SK.notelen && SK.notelen + 1 < SK_NOTE) SK.note[SK.notelen++] = ' ';
    while (*s && SK.notelen + 1 < SK_NOTE) SK.note[SK.notelen++] = *s++;
    SK.note[SK.notelen] = 0;
}

static const char *sk_tok(const char *p, char *out, size_t cap) {
    size_t n = 0;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        if (n + 1 < cap) out[n++] = *p;
        p++;
    }
    out[n] = 0;
    return p;
}

/* 0 accepted, 1 the line is neither a directive nor brainfuck. bfgen refuses
 * the same line, and the two readers of a .poke agreeing about what a .poke
 * IS is the precondition for either of them proving anything about one. */
static int sk_feed(const char *line, int lineno) {
    char w[64];
    const char *p = line;
    const char *q;

    while (*p == ' ' || *p == '\t') p++;
    if (*p == 0 || *p == '\n' || *p == '\r') return 0;

    if (*p == '#') {
        /* indented annotates the next frame; flush is the file's own prose */
        if (p[1] == ' ' && p[2] == ' ') {
            char t[SK_LINE];
            size_t n = 0;
            q = p + 1;
            while (*q == ' ' || *q == '\t') q++;
            while (*q && *q != '\n' && *q != '\r' && n + 1 < sizeof t) t[n++] = *q++;
            t[n] = 0;
            if (n) sk_addnote(t);
        }
        return 0;
    }

    p = sk_tok(p, w, sizeof w);
    if (strcmp(w, "EMIT") == 0) {
        for (;;) {
            p = sk_tok(p, w, sizeof w);
            if (!w[0]) break;
            if (strlen(w) != 2 || nyb(w[0]) < 0 || nyb(w[1]) < 0) return 1;
            sk_byte((unsigned char)(nyb(w[0]) * 16 + nyb(w[1])), 1, lineno);
        }
        return 0;
    }
    if (strcmp(w, "READ") == 0) {
        long n = 0;
        int i, any = 0;
        p = sk_tok(p, w, sizeof w);
        for (i = 0; w[i]; i++) {
            if (w[i] < '0' || w[i] > '9') return 1;
            n = n * 10 + (w[i] - '0');
            any = 1;
        }
        if (!any) return 1;
        sk_close((int)n);
        return 0;
    }
    if (strcmp(w, "LOOP") == 0 || strcmp(w, "END") == 0) { sk_close(-1); return 0; }

    /* raw brainfuck, which bfgen passes through untouched */
    {
        int dots = 0, comma = 0, loopy = 0;
        for (q = line; *q; q++) {
            switch (*q) {
            case ' ': case '\t': case '\n': case '\r':      break;
            case '.':                            dots++;    break;
            case ',':                            comma = 1; break;
            case '[': case ']':                  loopy = 1; break;
            case '>': case '<': case '+': case '-':         break;
            default: return 1;
            }
        }
        /* The order matters: a '.' inside a loop may open the run, and the
         * run it opens is the unknowable one. Marking before the bytes
         * arrive would mark whatever ran BEFORE this line instead, which is
         * how bf/proc/echo.poke -- ",[.,]", a program that speaks no
         * protocol at all -- first came back looking like a readable frame. */
        while (dots-- > 0) sk_byte(0, 0, lineno);
        if (loopy && SK.open) SK.unknowable = 1;
        /* a ',' is a read, and a read is where a frame ends */
        if (comma) sk_close(-1);
    }
    return 0;
}

static int skeleton(const char *path) {
    char line[SK_LINE];
    FILE *f = fopen(path, "r");
    int lineno = 0;
    if (!f) { fprintf(stderr, "bsframe: cannot read %s\n", path); return 2; }
    sk_reset(0);
    while (fgets(line, sizeof line, f)) {
        lineno++;
        if (sk_feed(line, lineno) != 0) {
            fprintf(stderr, "bsframe: %s:%d is neither a directive nor brainfuck\n",
                    path, lineno);
            fclose(f);
            return 2;
        }
    }
    sk_close(-1);
    fclose(f);
    return 0;
}

/* Both polarities, over the cases ABI.md calls out by name. Deliberately not
 * the same cases bscodec tests: that one exercises the cursor and the buffer,
 * this one exercises the frame SHAPE, and between them the edges are covered
 * from two directions. */
static int selftest(void) {
    int bad = 0;
    unsigned char h[HDR];
    long d;

    /* the length is little endian and the header is three bytes */
    h[0] = 0x01; h[1] = 0x0a; h[2] = 0x00;
    d = (long)h[1] + 256L * (long)h[2];
    if (d != 10) { printf("SELFTEST FAIL: little endian length\n"); bad = 1; }

    h[1] = 0x00; h[2] = 0x01;
    d = (long)h[1] + 256L * (long)h[2];
    if (d != 256) { printf("SELFTEST FAIL: length crossing the byte boundary\n"); bad = 1; }

    h[1] = 0xff; h[2] = 0xff;
    d = (long)h[1] + 256L * (long)h[2];
    if (d != MAXP) { printf("SELFTEST FAIL: maximum length\n"); bad = 1; }
    if (!bad) printf("selftest ok: the length is little endian at three widths\n");

    /* a big endian reading must NOT agree, or the test above proves nothing */
    h[1] = 0x0a; h[2] = 0x00;
    if (((long)h[2] + 256L * (long)h[1]) == 10) {
        printf("SELFTEST FAIL: big endian reads the same, so the check is inert\n");
        bad = 1;
    } else {
        printf("selftest ok: a big endian reading disagrees, so the check has teeth\n");
    }

    /* ---- the skeleton reader, both polarities -------------------------- */
    {   /* a whole frame, comments in the middle of it, closed by a READ */
        sk_reset(1);
        sk_feed("#   hello  op 01, len 10", 1);
        sk_feed("EMIT 01 0a 00 42 53 54 4d 01 00", 2);
        sk_feed("#     and the rest of it", 3);
        sk_feed("EMIT 00 00 00 00", 4);
        sk_feed("READ 107", 5);
        if (SK.count != 1 || !SK.last_ok || SK.last_op != 0x01 ||
            SK.last_declared != 10 || SK.last_actual != 10 || SK.last_read != 107) {
            printf("SELFTEST FAIL: a frame split across EMIT lines did not add up\n");
            bad = 1;
        } else {
            printf("selftest ok: consecutive EMIT lines are one frame\n");
        }
    }
    {   /* the check must be able to FAIL: one byte short is one byte short */
        sk_reset(1);
        sk_feed("EMIT 01 0a 00 42 53 54 4d 01 00 00 00 00", 1);
        sk_feed("READ 3", 2);
        if (SK.last_declared != 10 || SK.last_actual != 9) {
            printf("SELFTEST FAIL: a short payload was not seen as short\n");
            bad = 1;
        } else {
            printf("selftest ok: a declared length longer than the payload is visible\n");
        }
    }
    {   /* raw brainfuck in the middle of a frame: a '.' is a byte this tool
         * cannot value but can count, which is exactly what keeps
         * bf/net/loopback.poke checkable at all */
        sk_reset(1);
        sk_feed("EMIT 06 26 00 05 00 00 00 00 00 01 00", 1);
        sk_feed(">.>.<<", 2);
        sk_feed("EMIT 7f 00 00 01", 3);
        sk_feed("EMIT 00 00 00 00 00 00 00 00 00 00 00 00", 4);
        sk_feed("EMIT 00 00 00 00 00 00 00 00 00 00 00 00", 5);
        sk_feed("READ 3", 6);
        if (SK.last_declared != 38 || SK.last_actual != 38) {
            printf("SELFTEST FAIL: raw brainfuck bytes were not counted (%ld against %ld)\n",
                   SK.last_declared, SK.last_actual);
            bad = 1;
        } else {
            printf("selftest ok: a '.' in raw brainfuck counts as one emitted byte\n");
        }
    }
    {   /* a loop makes the count unknowable, and saying so beats guessing */
        sk_reset(1);
        sk_feed("EMIT 0b 08 00 05 00 00 00 00 00", 1);
        sk_feed("+[.-]", 2);
        sk_feed("READ 5", 3);
        if (SK.last_actual != -1) {
            printf("SELFTEST FAIL: a loop was counted rather than refused\n");
            bad = 1;
        } else {
            printf("selftest ok: a loop makes the byte count unknowable, not wrong\n");
        }
    }
    {   /* a flush comment is the file's prose and must not reach a frame */
        sk_reset(1);
        sk_feed("# this prose mentions spawn and readdir and connect", 1);
        sk_feed("EMIT 02 01 00 00", 2);
        sk_feed("READ 3", 3);
        if (SK.notelen != 0 || SK.count != 1) {
            printf("SELFTEST FAIL: flush prose was attached to a frame\n");
            bad = 1;
        } else {
            printf("selftest ok: only an indented comment annotates a frame\n");
        }
        /* and an indented one must be, or the tier has nothing to read */
        sk_reset(1);
        sk_feed("#   exit  op 02", 1);
        sk_feed("EMIT 02 01 00 00", 2);
        if (SK.notelen == 0) {
            printf("SELFTEST FAIL: an indented comment was dropped\n");
            bad = 1;
        }
    }
    {   /* a line that is neither a directive nor brainfuck is refused, the
         * same way bfgen refuses it */
        sk_reset(1);
        if (sk_feed("PEEK 4", 1) == 0) {
            printf("SELFTEST FAIL: an unknown directive was accepted\n");
            bad = 1;
        } else {
            printf("selftest ok: an unknown directive is refused, as bfgen refuses it\n");
        }
    }

    if (bad) { printf("SELFTEST FAILED\n"); return 1; }
    printf("bsframe --selftest: ok\n");
    return 0;
}

static void usage(const char *me) {
    fprintf(stderr,
        "usage: %s encode KINDHEX PAYLOADHEX | decode FRAMEHEX\n"
        "       %s skeleton FILE.poke\n"
        "       %s --selftest\n", me, me, me);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 2; }
    if (strcmp(argv[1], "--selftest") == 0) return selftest();
    if (strcmp(argv[1], "encode") == 0 && argc == 4) return encode(argv[2], argv[3]);
    if (strcmp(argv[1], "decode") == 0 && argc == 3) return decode(argv[2]);
    if (strcmp(argv[1], "skeleton") == 0 && argc == 3) return skeleton(argv[2]);
    usage(argv[0]);
    return 2;
}
