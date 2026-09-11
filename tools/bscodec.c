/* bscodec — the real codec on the command line, so it can be tested with no
 * process, no descriptor and no kernel.
 *
 * This is a thin skin over src/frame.c and adds no encoding knowledge of its
 * own. Its twin, tools/bsframe.c, implements the same command line from
 * ABI.md with NO shared code, and the suite requires the two to agree over
 * every pinned vector. That is the whole point of having two: a bug in the
 * byte order or the header width would be invisible to a test that used one
 * encoder to check the other.
 *
 * Usage:
 *   bscodec encode KINDHEX PAYLOADHEX   print the frame as hex
 *   bscodec decode FRAMEHEX            print "kind=NN len=N payload=HEX"
 *   bscodec --selftest                 prove the cursor and buffer hold at their edges
 *
 * Exit: 0 ok; 1 the input was not a well formed frame; 2 usage.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/frame.h"

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* hex string to bytes; returns the count, or -1 if it is not hex or is odd */
static long unhex(const char *s, unsigned char *out, size_t cap) {
    size_t n = 0;
    int hi = -1;
    for (; *s; s++) {
        if (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r') continue;
        int v = hexval((unsigned char)*s);
        if (v < 0) return -1;
        if (hi < 0) { hi = v; continue; }
        if (n == cap) return -1;
        out[n++] = (unsigned char)((hi << 4) | v);
        hi = -1;
    }
    return hi < 0 ? (long)n : -1;
}

static void puthex(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
}

static unsigned char inbuf[BS_FRAME_MAX];
static unsigned char outbuf[BS_FRAME_MAX];

static int do_encode(const char *kindhex, const char *payhex) {
    unsigned char k[1];
    if (unhex(kindhex, k, 1) != 1) { fprintf(stderr, "bscodec: KINDHEX is one hex byte\n"); return 2; }
    long n = unhex(payhex, inbuf, sizeof inbuf);
    if (n < 0) { fprintf(stderr, "bscodec: PAYLOADHEX is not hex\n"); return 2; }
    if (n > BS_PAYLOAD_MAX) { fprintf(stderr, "bscodec: payload exceeds %d\n", BS_PAYLOAD_MAX); return 1; }

    bs_hdr_put(outbuf, k[0], (unsigned int)n);
    for (long i = 0; i < n; i++) outbuf[BS_HDR_LEN + i] = inbuf[i];
    puthex(outbuf, (size_t)(BS_HDR_LEN + n));
    printf("\n");
    return 0;
}

static int do_decode(const char *framehex) {
    long n = unhex(framehex, inbuf, sizeof inbuf);
    if (n < 0) { fprintf(stderr, "bscodec: FRAMEHEX is not hex\n"); return 2; }
    if (n < BS_HDR_LEN) { printf("ERR SHORTHDR\n"); return 1; }

    unsigned char kind; unsigned int len;
    bs_hdr_get(inbuf, &kind, &len);

    size_t body = (size_t)n - BS_HDR_LEN;
    if (body < len)  { printf("ERR SHORTBODY\n"); return 1; }
    if (body > len)  { printf("ERR TRAILING\n"); return 1; }

    printf("kind=%02x len=%u payload=", kind, len);
    puthex(inbuf + BS_HDR_LEN, len);
    printf("\n");
    return 0;
}

/* Self-test: the cursor and the buffer must hold at their edges, and must
 * FAIL where they are supposed to. A codec that never refuses anything is
 * indistinguishable from one with no checks in it. */
static int selftest(void) {
    int bad = 0;
    unsigned char b[32];

    /* a round trip through every width, little end first */
    struct bs_buf w; bs_buf_init(&w, b, sizeof b);
    bs_put_u8(&w, 0x11);
    bs_put_u16(&w, 0x2233);
    bs_put_u32(&w, 0x44556677UL);
    bs_put_u64(&w, 0x8899AABBCCDDEEFFULL);
    if (!bs_buf_ok(&w) || bs_buf_len(&w) != 15) { printf("SELFTEST FAIL: put widths\n"); bad = 1; }
    if (b[0] != 0x11 || b[1] != 0x33 || b[2] != 0x22) { printf("SELFTEST FAIL: u16 is not little endian\n"); bad = 1; }
    if (b[3] != 0x77 || b[6] != 0x44) { printf("SELFTEST FAIL: u32 is not little endian\n"); bad = 1; }
    if (b[7] != 0xFF || b[14] != 0x88) { printf("SELFTEST FAIL: u64 is not little endian\n"); bad = 1; }

    struct bs_cur r; bs_cur_init(&r, b, bs_buf_len(&w));
    if (bs_get_u8(&r) != 0x11)                     { printf("SELFTEST FAIL: u8 round trip\n"); bad = 1; }
    if (bs_get_u16(&r) != 0x2233)                  { printf("SELFTEST FAIL: u16 round trip\n"); bad = 1; }
    if (bs_get_u32(&r) != 0x44556677UL)            { printf("SELFTEST FAIL: u32 round trip\n"); bad = 1; }
    if (bs_get_u64(&r) != 0x8899AABBCCDDEEFFULL)   { printf("SELFTEST FAIL: u64 round trip\n"); bad = 1; }
    if (!bs_cur_done(&r)) { printf("SELFTEST FAIL: cursor not exactly consumed\n"); bad = 1; }
    else printf("selftest ok: every width round trips little end first\n");

    /* one byte short of a u16 must set the sticky flag, not read past */
    bs_cur_init(&r, b, 1);
    (void)bs_get_u16(&r);
    if (bs_cur_ok(&r)) { printf("SELFTEST FAIL: underrun not caught\n"); bad = 1; }
    else printf("selftest ok: caught a read past the end\n");

    /* and once bad, it stays bad -- a later get must not appear to succeed */
    if (bs_get_u8(&r) != 0 || bs_cur_ok(&r)) { printf("SELFTEST FAIL: failure is not sticky\n"); bad = 1; }
    else printf("selftest ok: the failure flag is sticky\n");

    /* a parse that leaves bytes behind has misread the frame */
    bs_cur_init(&r, b, 4);
    (void)bs_get_u8(&r);
    if (!bs_cur_ok(&r) || bs_cur_done(&r)) { printf("SELFTEST FAIL: trailing bytes reported as done\n"); bad = 1; }
    else printf("selftest ok: a partial parse is not done\n");

    /* the buffer must refuse rather than overflow */
    bs_buf_init(&w, b, 3);
    bs_put_u32(&w, 1);
    if (bs_buf_ok(&w) || bs_buf_len(&w) != 0) { printf("SELFTEST FAIL: overflow not caught\n"); bad = 1; }
    else printf("selftest ok: caught a write past the end\n");

    /* a header is three bytes and every bit pattern in them is legal */
    unsigned char h[BS_HDR_LEN]; unsigned char k; unsigned int L;
    bs_hdr_put(h, 0x17, BS_PAYLOAD_MAX);
    bs_hdr_get(h, &k, &L);
    if (k != 0x17 || L != BS_PAYLOAD_MAX) { printf("SELFTEST FAIL: header round trip at max\n"); bad = 1; }
    bs_hdr_put(h, 0x00, 0);
    bs_hdr_get(h, &k, &L);
    if (k != 0 || L != 0) { printf("SELFTEST FAIL: header round trip at zero\n"); bad = 1; }
    if (!bad) printf("selftest ok: the header round trips at both extremes\n");

    if (bad) { printf("SELFTEST FAILED\n"); return 1; }
    printf("bscodec --selftest: ok\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s encode KINDHEX PAYLOADHEX | decode FRAMEHEX | --selftest\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "--selftest") == 0) return selftest();
    if (strcmp(argv[1], "encode") == 0 && argc == 4) return do_encode(argv[2], argv[3]);
    if (strcmp(argv[1], "decode") == 0 && argc == 3) return do_decode(argv[2]);
    fprintf(stderr, "usage: %s encode KINDHEX PAYLOADHEX | decode FRAMEHEX | --selftest\n", argv[0]);
    return 2;
}
