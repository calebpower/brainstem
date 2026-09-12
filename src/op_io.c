/* op_io.c — read, write, close and poll.
 *
 * These four work on anything the handle table holds: a file, a directory, a
 * pipe end, a socket. That is deliberate and is why they are one file rather
 * than three -- a program that has a handle should not have to know what is
 * behind it to move bytes through it, and the places where it does matter
 * (you cannot read a directory, you cannot seek a pipe) are refusals rather
 * than separate opcodes.
 */
#include "ops.h"
#include "fdtab.h"

/* Every op here begins the same way: decode a handle, refuse it if the
 * generation is stale or the slot was never issued, and refuse it again if
 * the rights the handle carries do not cover what is being asked.
 *
 * RIGHTS ARE CHECKED BEFORE THE SEAM IS TOUCHED, so a refusal costs no
 * syscall and, more importantly, cannot half-happen. */
static bs_err want(bs_u32 h, bs_u16 rights, struct bs_slot **out) {
    struct bs_slot *s = bs_fdtab_get(h);
    if (!s) return BS_BADF;
    if ((s->rights & rights) != rights) return BS_DENIED;
    *out = s;
    return BS_OK;
}

/* ABI.md section 7.10. Request: handle{u32} n{u16} flags{u16}, eight bytes.
 * Reply: up to n bytes.
 *
 * END and AGAIN are DIFFERENT CODES and that is exactly what a poll loop
 * needs: "there will never be more" and "there is none right now" call for
 * opposite actions, and a program that could not tell them apart would either
 * spin forever or stop early. */
bs_err op_io_read(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    bs_u32 h;
    unsigned int n, flags;
    struct bs_slot *s;
    bs_err e;
    size_t got = 0;
    (void)ctx;

    h     = (bs_u32)bs_get_u32(req);
    n     = bs_get_u16(req);
    flags = bs_get_u16(req);
    if (!bs_cur_ok(req) || !bs_cur_done(req)) return BS_BADLEN;

    e = want(h, BS_R_READ, &s);
    if (e != BS_OK) return e;

    /* A directory is enumerated with readdir, never read as bytes. Both
     * kernels would give an error here anyway, but they do not agree on
     * WHICH error, and an errno that differs by platform is the one thing
     * this ABI refuses to put on the wire. */
    if (s->kind == BS_HK_DIR) return BS_ISDIR;

    if (n == 0) return BS_OK;

    /* n is a u16 and the reply arena is sized for the largest u16, so the
     * read goes straight into the reply buffer with no scratch copy and no
     * possibility of arithmetic overrunning it. */
    e = sys_read(s->fd, rep->p + rep->n, n, (flags & 1u) ? 1 : 0, &got);
    if (e != BS_OK) return e;
    if (got == 0) return BS_END;
    rep->n += got;
    return bs_buf_ok(rep) ? BS_OK : BS_IO;
}

/* ABI.md section 7.11. Request: handle{u32} flags{u16} data. Reply:
 * nwritten{u16}.
 *
 * Without NOWAIT the broker retries short writes internally, so nwritten
 * always equals the data length on OK. See sys_write for why that is the
 * single biggest kindness in this ABI. */
bs_err op_io_write(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    bs_u32 h;
    unsigned int flags;
    const unsigned char *data;
    size_t len;
    struct bs_slot *s;
    bs_err e;
    size_t put = 0;
    (void)ctx;

    h     = (bs_u32)bs_get_u32(req);
    flags = bs_get_u16(req);
    if (!bs_cur_ok(req)) return BS_BADLEN;

    len  = bs_cur_left(req);
    data = bs_get_bytes(req, len);
    if (!bs_cur_ok(req)) return BS_BADLEN;

    e = want(h, BS_R_WRITE, &s);
    if (e != BS_OK) return e;
    if (s->kind == BS_HK_DIR) return BS_ISDIR;

    e = sys_write(s->fd, data, len, (flags & 1u) ? 1 : 0, &put);
    if (e != BS_OK) return e;

    bs_put_u16(rep, (unsigned int)put);
    return bs_buf_ok(rep) ? BS_OK : BS_IO;
}

/* ABI.md section 7.12. Request: handle{u32}. Reply: empty.
 *
 * Closing one of the three standard handles is permitted and permanent --
 * the program was given it and may give it up. Closing a closed handle is BADF. The slot returns to
 * the pool immediately and its generation increments, which is required for
 * determinism rather than an implementation detail: it is what makes the
 * next handle predictable AND makes the old one unusable. */
bs_err op_io_close(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    bs_u32 h;
    (void)ctx; (void)rep;

    h = (bs_u32)bs_get_u32(req);
    if (!bs_cur_ok(req) || !bs_cur_done(req)) return BS_BADLEN;
    if (!bs_fdtab_get(h)) return BS_BADF;
    return bs_fdtab_free(h);
}

/* ABI.md section 7.13. Request: timeout_ms{u32} nfds{u8} then nfds triples of
 * handle{u32} want_read{u8} want_write{u8}. Reply: nready{u8} reserved{u8}
 * then nfds quads of readable, writable, hup, err.
 *
 * THE FLAGSHIP PARSEABILITY DECISION. Brainfuck has no bitwise instruction:
 * bfsodium had to build XOR8 out of eight halvings, and its rotl32 at twenty
 * million instructions a call was that library's real bottleneck. A POSIX
 * revents bitmask would cost a bit decomposition per descriptor per poll.
 * One byte per condition costs a bare loop and nothing else, and four bytes
 * instead of one is a trade the measured cost table says is not even visible.
 *
 * The reply is POSITIONAL, in the request's order, so a program never
 * searches for a handle in it -- it walks its own request and the reply in
 * lockstep. nready exists so the common "nothing happened" case costs one
 * test rather than nfds. */
bs_err op_io_poll(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    bs_pollfd set[BS_POLL_MAX];
    unsigned long timeout;
    unsigned int nfds, i;
    size_t nready = 0;
    bs_err e;
    (void)ctx;

    timeout = bs_get_u32(req);
    nfds    = bs_get_u8(req);
    if (!bs_cur_ok(req)) return BS_BADLEN;
    if (nfds > BS_POLL_MAX) return BS_INVAL;

    /* The length is checked against nfds HERE rather than by the table,
     * because it is the only op whose arity depends on a field inside its own
     * payload. Getting this wrong is the desync this ABI fears most, so it is
     * exact: not "at least", exactly. */
    if (bs_cur_left(req) != (size_t)nfds * 6u) return BS_BADLEN;

    for (i = 0; i < nfds; i++) {
        bs_u32 h = (bs_u32)bs_get_u32(req);
        struct bs_slot *s = bs_fdtab_get(h);
        if (!s) return BS_BADF;
        set[i].fd         = s->fd;
        set[i].want_read  = bs_get_u8(req) ? 1 : 0;
        set[i].want_write = bs_get_u8(req) ? 1 : 0;
        set[i].readable = set[i].writable = set[i].hup = set[i].err = 0;
    }
    if (!bs_cur_ok(req) || !bs_cur_done(req)) return BS_BADLEN;

    e = sys_poll(set, nfds, (bs_u32)timeout, &nready);
    if (e != BS_OK) return e;

    bs_put_u8(rep, nready > 255 ? 255 : (unsigned int)nready);
    bs_put_u8(rep, 0);
    for (i = 0; i < nfds; i++) {
        bs_put_u8(rep, set[i].readable);
        bs_put_u8(rep, set[i].writable);
        bs_put_u8(rep, set[i].hup);
        bs_put_u8(rep, set[i].err);
    }
    return bs_buf_ok(rep) ? BS_OK : BS_IO;
}
