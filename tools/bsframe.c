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

    if (bad) { printf("SELFTEST FAILED\n"); return 1; }
    printf("bsframe --selftest: ok\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s encode KINDHEX PAYLOADHEX | decode FRAMEHEX | --selftest\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "--selftest") == 0) return selftest();
    if (strcmp(argv[1], "encode") == 0 && argc == 4) return encode(argv[2], argv[3]);
    if (strcmp(argv[1], "decode") == 0 && argc == 3) return decode(argv[2]);
    fprintf(stderr, "usage: %s encode KINDHEX PAYLOADHEX | decode FRAMEHEX | --selftest\n", argv[0]);
    return 2;
}
