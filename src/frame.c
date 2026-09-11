/* frame.c — the wire codec. See frame.h for why it has no I/O in it.
 *
 * Every integer is assembled and disassembled a byte at a time, little end
 * first. Not a memcpy of a native integer, and not a cast through a pointer:
 * the first would be wrong on a big-endian host and the second is undefined
 * behaviour on any host whose alignment requirements we happen to violate.
 * A byte loop is correct everywhere, and at these sizes the compiler emits
 * the same thing anyway.
 */
#include "frame.h"

/* ---- header ------------------------------------------------------------ */

void bs_hdr_put(unsigned char *dst, unsigned char kind, unsigned int len) {
    dst[0] = kind;
    dst[1] = (unsigned char)( len       & 0xFF);
    dst[2] = (unsigned char)((len >> 8) & 0xFF);
}

void bs_hdr_get(const unsigned char *src, unsigned char *kind, unsigned int *len) {
    *kind = src[0];
    *len  = (unsigned int)src[1] | ((unsigned int)src[2] << 8);
}

/* ---- reading ----------------------------------------------------------- */

void bs_cur_init(struct bs_cur *c, const unsigned char *p, size_t n) {
    c->p = p; c->n = n; c->i = 0; c->bad = 0;
}

/* The one place an underrun is detected. Every get goes through it, so the
 * sticky flag cannot be set in one path and missed in another. */
static int take(struct bs_cur *c, size_t n) {
    if (c->bad) return 0;
    if (n > c->n - c->i) { c->bad = 1; return 0; }
    return 1;
}

unsigned char bs_get_u8(struct bs_cur *c) {
    if (!take(c, 1)) return 0;
    return c->p[c->i++];
}

unsigned int bs_get_u16(struct bs_cur *c) {
    if (!take(c, 2)) return 0;
    unsigned int v = (unsigned int)c->p[c->i] | ((unsigned int)c->p[c->i + 1] << 8);
    c->i += 2;
    return v;
}

unsigned long bs_get_u32(struct bs_cur *c) {
    if (!take(c, 4)) return 0;
    unsigned long v = 0;
    for (int k = 3; k >= 0; k--) v = (v << 8) | c->p[c->i + (size_t)k];
    c->i += 4;
    return v;
}

unsigned long long bs_get_u64(struct bs_cur *c) {
    if (!take(c, 8)) return 0;
    unsigned long long v = 0;
    for (int k = 7; k >= 0; k--) v = (v << 8) | c->p[c->i + (size_t)k];
    c->i += 8;
    return v;
}

const unsigned char *bs_get_bytes(struct bs_cur *c, size_t n) {
    if (!take(c, n)) return 0;
    const unsigned char *r = c->p + c->i;
    c->i += n;
    return r;
}

int    bs_cur_ok  (const struct bs_cur *c) { return !c->bad; }
size_t bs_cur_left(const struct bs_cur *c) { return c->bad ? 0 : c->n - c->i; }
int    bs_cur_done(const struct bs_cur *c) { return !c->bad && c->i == c->n; }

/* ---- writing ----------------------------------------------------------- */

void bs_buf_init(struct bs_buf *b, unsigned char *p, size_t cap) {
    b->p = p; b->cap = cap; b->n = 0; b->over = 0;
}

static int room(struct bs_buf *b, size_t n) {
    if (b->over) return 0;
    if (n > b->cap - b->n) { b->over = 1; return 0; }
    return 1;
}

void bs_put_u8(struct bs_buf *b, unsigned int v) {
    if (!room(b, 1)) return;
    b->p[b->n++] = (unsigned char)(v & 0xFF);
}

void bs_put_u16(struct bs_buf *b, unsigned int v) {
    if (!room(b, 2)) return;
    b->p[b->n++] = (unsigned char)( v       & 0xFF);
    b->p[b->n++] = (unsigned char)((v >> 8) & 0xFF);
}

void bs_put_u32(struct bs_buf *b, unsigned long v) {
    if (!room(b, 4)) return;
    for (int k = 0; k < 4; k++) b->p[b->n++] = (unsigned char)((v >> (8 * k)) & 0xFF);
}

void bs_put_u64(struct bs_buf *b, unsigned long long v) {
    if (!room(b, 8)) return;
    for (int k = 0; k < 8; k++) b->p[b->n++] = (unsigned char)((v >> (8 * k)) & 0xFF);
}

void bs_put_bytes(struct bs_buf *b, const unsigned char *s, size_t n) {
    if (!room(b, n)) return;
    for (size_t k = 0; k < n; k++) b->p[b->n++] = s[k];
}

void bs_put_zero(struct bs_buf *b, size_t n) {
    if (!room(b, n)) return;
    for (size_t k = 0; k < n; k++) b->p[b->n++] = 0;
}

int    bs_buf_ok (const struct bs_buf *b) { return !b->over; }
size_t bs_buf_len(const struct bs_buf *b) { return b->n; }
