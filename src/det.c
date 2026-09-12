/* det.c — seeded randomness and a virtual clock.
 *
 * THE GENERATOR IS CHACHA20, and that is not an arbitrary choice of PRNG.
 * bfsodium implements ChaCha20 in brainfuck and verifies it against RFC 8439
 * with two independent oracles. So a test can ask brainstem for random bytes
 * under a known seed, compute the same keystream with the sibling's
 * chacha20/stream.bf, and require them to agree -- a dual oracle for this
 * broker's RNG, using the other project as the second opinion. Nothing else
 * available here has that property, and it is the reason ABI.md section 9
 * specifies the construction rather than saying "some PRNG".
 *
 * Stated exactly, so the oracle can be reproduced:
 *
 *   key     = the 16 seed bytes, then 16 zero bytes
 *   nonce   = 12 zero bytes
 *   counter = 0, incrementing per 64 byte block
 *
 * The keystream is consumed sequentially across every rand.bytes call for the
 * life of the broker, so the output depends only on the seed and on how many
 * bytes have been asked for so far.
 */
#include <string.h>
#include <stdlib.h>
#include "det.h"

static int    seeded;
static bs_u8  seed[16];
static int    clock_mode = BS_CLOCK_LIVE;
static bs_i64 clock_epoch = 1700000000;
static bs_u32 clock_step  = 1000000;      /* one millisecond per request */
static bs_u64 ticks;

/* ---- ChaCha20, RFC 8439 section 2.3 ------------------------------------ */

static bs_u32 rotl(bs_u32 v, int n) { return (v << n) | (v >> (32 - n)); }

static void quarter(bs_u32 *x, int a, int b, int c, int d) {
    x[a] += x[b]; x[d] ^= x[a]; x[d] = rotl(x[d], 16);
    x[c] += x[d]; x[b] ^= x[c]; x[b] = rotl(x[b], 12);
    x[a] += x[b]; x[d] ^= x[a]; x[d] = rotl(x[d], 8);
    x[c] += x[d]; x[b] ^= x[c]; x[b] = rotl(x[b], 7);
}

static void chacha_block(const bs_u32 in[16], bs_u8 out[64]) {
    bs_u32 x[16];
    int i;
    for (i = 0; i < 16; i++) x[i] = in[i];
    for (i = 0; i < 10; i++) {          /* ten double rounds is twenty rounds */
        quarter(x, 0, 4,  8, 12);
        quarter(x, 1, 5,  9, 13);
        quarter(x, 2, 6, 10, 14);
        quarter(x, 3, 7, 11, 15);
        quarter(x, 0, 5, 10, 15);
        quarter(x, 1, 6, 11, 12);
        quarter(x, 2, 7,  8, 13);
        quarter(x, 3, 4,  9, 14);
    }
    for (i = 0; i < 16; i++) {
        bs_u32 v = x[i] + in[i];
        out[4 * i + 0] = (bs_u8)( v        & 0xFF);
        out[4 * i + 1] = (bs_u8)((v >>  8) & 0xFF);
        out[4 * i + 2] = (bs_u8)((v >> 16) & 0xFF);
        out[4 * i + 3] = (bs_u8)((v >> 24) & 0xFF);
    }
}

static bs_u32 state[16];
static bs_u8  ks[64];
static int    ks_used;

static void rng_init(void) {
    bs_u8 key[32];
    int i;
    memcpy(key, seed, 16);
    memset(key + 16, 0, 16);
    /* "expand 32-byte k", the constant RFC 8439 section 2.3 specifies */
    state[0] = 0x61707865UL; state[1] = 0x3320646eUL;
    state[2] = 0x79622d32UL; state[3] = 0x6b206574UL;
    for (i = 0; i < 8; i++)
        state[4 + i] = (bs_u32)key[4 * i]
                     | ((bs_u32)key[4 * i + 1] <<  8)
                     | ((bs_u32)key[4 * i + 2] << 16)
                     | ((bs_u32)key[4 * i + 3] << 24);
    state[12] = 0;
    state[13] = state[14] = state[15] = 0;
    ks_used = 64;                        /* force a block on first use */
}

static void rng_fill(bs_u8 *buf, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n; i++) {
        if (ks_used == 64) {
            chacha_block(state, ks);
            state[12]++;
            ks_used = 0;
        }
        buf[i] = ks[ks_used++];
    }
}

/* ---- knobs ------------------------------------------------------------- */

static int hexnyb(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bs_err det_set_seed(const char *hex) {
    int i;
    if (!hex) return BS_INVAL;
    for (i = 0; i < 32; i++) if (hexnyb((unsigned char)hex[i]) < 0) return BS_INVAL;
    if (hex[32] != '\0') return BS_INVAL;
    for (i = 0; i < 16; i++)
        seed[i] = (bs_u8)((hexnyb((unsigned char)hex[2 * i]) << 4)
                        |  hexnyb((unsigned char)hex[2 * i + 1]));
    seeded = 1;
    rng_init();
    return BS_OK;
}

/* A strict decimal parser, because strtol is not one.
 *
 * strtol("") is 0 with no error a caller can see without clearing errno
 * first, which is how "--clock frozen=" came to mean "--clock frozen=0" in
 * the first draft -- caught by the self test rather than by review, which is
 * the whole argument for the self test. Here an empty run of digits, a
 * trailing character, or a stop short of `stop` is a refusal.
 *
 * `stop` is the character the number is allowed to end on: '\0' when the
 * number is the last thing in the spec, ',' when a step follows. */
static int decnum(const char **pp, int stop, bs_i64 *out) {
    const char *p = *pp;
    bs_i64 v = 0;
    int neg = 0, digits = 0;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
        if (++digits > 18) return 0;      /* refuse rather than wrap */
    }
    if (digits == 0) return 0;
    if (*p != stop) return 0;
    *out = neg ? -v : v;
    *pp = p;
    return 1;
}

bs_err det_set_clock(const char *spec) {
    const char *p;
    bs_i64 v;

    if (!spec) return BS_INVAL;

    if (strcmp(spec, "live") == 0) { clock_mode = BS_CLOCK_LIVE; return BS_OK; }

    if (strncmp(spec, "frozen", 6) == 0) {
        p = spec + 6;
        if (*p == '\0') { clock_mode = BS_CLOCK_FROZEN; return BS_OK; }
        if (*p != '=') return BS_INVAL;
        p++;
        if (!decnum(&p, '\0', &v)) return BS_INVAL;
        clock_mode  = BS_CLOCK_FROZEN;
        clock_epoch = v;
        return BS_OK;
    }

    if (strncmp(spec, "virtual", 7) == 0) {
        bs_i64 epoch = clock_epoch, step = clock_step;
        p = spec + 7;
        if (*p == '=') {
            p++;
            if (!decnum(&p, ',', &epoch) && !decnum(&p, '\0', &epoch)) return BS_INVAL;
        }
        if (*p == ',') {
            p++;
            if (strncmp(p, "step=", 5) != 0) return BS_INVAL;
            p += 5;
            if (!decnum(&p, '\0', &step)) return BS_INVAL;
            /* A zero step is a frozen clock spelled the long way, and it
             * would break the one thing the virtual clock is for: being
             * strictly monotonic, so "loop until the clock changes"
             * terminates. Refuse it rather than quietly hanging a program. */
            if (step <= 0 || step > 1000000000) return BS_INVAL;
        } else if (*p != '\0') {
            return BS_INVAL;
        }
        clock_mode  = BS_CLOCK_VIRTUAL;
        clock_epoch = epoch;
        clock_step  = (bs_u32)step;
        return BS_OK;
    }

    return BS_INVAL;
}

int    det_rng_seeded(void) { return seeded; }
int    det_clock_mode(void) { return clock_mode; }
bs_u32 det_clock_step(void) { return clock_mode == BS_CLOCK_VIRTUAL ? clock_step : 0; }
const bs_u8 *det_seed_bytes(void) { return seed; }

void det_tick(void) { ticks++; }

static void virtual_now(bs_time *out, int monotonic) {
    bs_u64 ns = (bs_u64)clock_step * ticks;
    bs_i64 base = monotonic ? 0 : clock_epoch;
    out->sec  = base + (bs_i64)(ns / 1000000000ULL);
    out->nsec = (bs_u32)(ns % 1000000000ULL);
}

bs_err det_clock_real(bs_time *out) {
    if (clock_mode == BS_CLOCK_FROZEN)  { out->sec = clock_epoch; out->nsec = 0; return BS_OK; }
    if (clock_mode == BS_CLOCK_VIRTUAL) { virtual_now(out, 0); return BS_OK; }
    return sys_clock_real(out);
}

bs_err det_clock_mono(bs_time *out) {
    if (clock_mode == BS_CLOCK_FROZEN)  { out->sec = 0; out->nsec = 0; return BS_OK; }
    if (clock_mode == BS_CLOCK_VIRTUAL) { virtual_now(out, 1); return BS_OK; }
    return sys_clock_mono(out);
}

bs_err det_random(bs_u8 *buf, unsigned int n) {
    /* Under a seed this issues NO SYSCALL AT ALL, which is why the per-op
     * syscall tier pins a different multiset for rand.bytes in deterministic
     * mode -- and why a replayed trace can be produced with the kernel out of
     * the picture entirely. */
    if (seeded) { rng_fill(buf, n); return BS_OK; }
    return sys_random(buf, n);
}
