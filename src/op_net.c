/* op_net.c — socket, connect, bind, listen, accept.
 *
 * Five ops and one record. The record is the interesting part: thirty two
 * bytes for every family, so a program emits a constant length and reads a
 * constant length no matter what it is talking to. See ABI.md section 6, and
 * sys_net.c for where the platform numbers go to die.
 *
 * WHAT THIS FILE DOES NOT DO, stated because ABI.md section 8 used to imply
 * otherwise: it does not check a capability before creating a socket. At M5
 * `socket` and `connect` are ambient -- a program that can reach this broker
 * can reach the network. The preopen model bounds the FILESYSTEM and does not
 * bound the network, and section 8.1 now says so in those words. Whether it
 * should is an open design question recorded in HANDOFF rather than settled
 * quietly here.
 */
#include <string.h>

#include "ops.h"
#include "fdtab.h"

/* A socket or a listener, but never a file. Checked from the table before the
 * op runs, so connect on a regular file is BADF here rather than something
 * stranger further down. */
static bs_err want_sock(bs_u32 h, bs_u16 rights, struct bs_slot **out) {
    struct bs_slot *s = bs_fdtab_get(h);
    if (!s) return BS_BADF;
    if (s->kind != BS_HK_SOCKET && s->kind != BS_HK_LISTENER) return BS_BADF;
    if ((s->rights & rights) != rights) return BS_DENIED;
    *out = s;
    return BS_OK;
}

/* The 32 byte record, off the wire. family{u8} reserved{u8} port{u16 LE}
 * body{28}. The port is little-endian like every other integer in this
 * protocol; the body is a byte string in reading order. Mixing byte orders
 * inside one record is the likeliest bug in the whole design, so the only
 * swap happens at the seam and the only place either convention is written
 * down is here and in ABI.md section 6. */
static bs_err addr_get(struct bs_cur *c, bs_addr *a) {
    const unsigned char *body;
    a->family = bs_get_u8(c);
    (void)bs_get_u8(c);                 /* reserved */
    a->port   = (bs_u16)bs_get_u16(c);
    body = bs_get_bytes(c, 28);
    if (!bs_cur_ok(c)) return BS_BADLEN;
    memcpy(a->body, body, 28);
    return BS_OK;
}

static void addr_put(struct bs_buf *b, const bs_addr *a) {
    bs_put_u8(b, a->family);
    bs_put_u8(b, 0);
    bs_put_u16(b, a->port);
    bs_put_bytes(b, a->body, 28);
}

/* ABI.md section 7.5. Request: domain{u8} type{u8} protocol{u8}.
 * Reply: handle{u32}. */
bs_err op_net_socket(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    unsigned int domain, type, proto;
    bs_osfd fd;
    bs_u32 h;
    bs_err e;
    (void)ctx;

    domain = bs_get_u8(req);
    type   = bs_get_u8(req);
    proto  = bs_get_u8(req);
    if (!bs_cur_ok(req) || !bs_cur_done(req)) return BS_BADLEN;

    /* protocol must be 0. There is exactly one protocol per (domain, type)
     * this ABI can express, so a nonzero value is a program that thinks it is
     * talking to Berkeley sockets and should be told otherwise now rather
     * than by a connect that behaves oddly later. */
    if (proto != 0) return BS_INVAL;

    e = sys_socket(domain, type, &fd);
    if (e != BS_OK) return e;

    /* Everything a socket can do, granted at creation and narrowed by nothing
     * -- because at M5 there is no capability gate on socket creation to
     * narrow FROM. When there is one, this is the line that changes and the
     * rest of the file does not. */
    e = bs_fdtab_alloc(BS_HK_SOCKET,
                       (bs_u16)(BS_R_READ | BS_R_WRITE | BS_R_CONNECT | BS_R_ACCEPT),
                       fd, &h);
    if (e != BS_OK) { sys_close(fd); return e; }

    bs_put_u32(rep, h);
    return bs_buf_ok(rep) ? BS_OK : BS_IO;
}

/* ABI.md section 7.6. Request: handle{u32} flags{u16} addr{32}, thirty eight
 * bytes. Reply: empty.
 *
 * NON-BLOCKING CONNECT WITHOUT A GETSOCKOPT OP. Issue this with NOWAIT; on
 * AGAIN, poll for writable; then RE-ISSUE THE IDENTICAL FRAME. The broker
 * remembers that an attempt is in flight on this handle and reports its
 * result. That is why there is no getsockopt in the ABI and no socket state a
 * program has to track beyond the handle it already holds. */
bs_err op_net_connect(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    bs_u32 h;
    unsigned int flags;
    bs_addr a;
    struct bs_slot *s;
    bs_err e;
    (void)ctx; (void)rep;

    h     = (bs_u32)bs_get_u32(req);
    flags = bs_get_u16(req);
    e = addr_get(req, &a);
    if (e != BS_OK) return e;
    if (!bs_cur_done(req)) return BS_BADLEN;

    /* Kind BEFORE rights, as everywhere else in this broker. listen() strips
     * CONNECT from the handle, so a rights-first check would report DENIED
     * and tell the program its capability was wrong when its HANDLE was.
     * ISCONN is the specific true answer and the one it can act on. */
    e = want_sock(h, 0, &s);
    if (e != BS_OK) return e;
    if (s->kind == BS_HK_LISTENER) return BS_ISCONN;
    if ((s->rights & BS_R_CONNECT) != BS_R_CONNECT) return BS_DENIED;
    if (!sys_net_family_supported(a.family)) return BS_NOTSUP;

    return sys_connect(s->fd, &a, (flags & 1u) ? 1 : 0, &s->netstate);
}

/* ABI.md section 7.7. Request: handle{u32} flags{u16} addr{32}. Reply: the
 * address ACTUALLY bound, thirty two bytes.
 *
 * The reply is what makes port 0 usable. There is no getsockname op, so
 * without this a program asking for an ephemeral port could never find out
 * which one it got -- and asking for an ephemeral port is what a test does,
 * because a fixed port makes a suite that cannot run twice at once. */
bs_err op_net_bind(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    bs_u32 h;
    unsigned int flags;
    bs_addr a, bound;
    struct bs_slot *s;
    bs_err e;
    (void)ctx;

    h     = (bs_u32)bs_get_u32(req);
    flags = bs_get_u16(req);
    e = addr_get(req, &a);
    if (e != BS_OK) return e;
    if (!bs_cur_done(req)) return BS_BADLEN;

    e = want_sock(h, BS_R_WRITE, &s);
    if (e != BS_OK) return e;
    if (!sys_net_family_supported(a.family)) return BS_NOTSUP;

    memset(&bound, 0, sizeof bound);
    e = sys_bind(s->fd, &a, (flags & 1u) ? 1 : 0, &bound);
    if (e != BS_OK) return e;

    addr_put(rep, &bound);
    return bs_buf_ok(rep) ? BS_OK : BS_IO;
}

/* ABI.md section 7.8. Request: handle{u32} backlog{u16}, six bytes. A backlog
 * of 0 means 128, because zero is the byte a brainfuck program emits for free
 * and the sensible default is what it should get for free. */
bs_err op_net_listen(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    bs_u32 h;
    unsigned int backlog;
    struct bs_slot *s;
    bs_err e;
    (void)ctx; (void)rep;

    h       = (bs_u32)bs_get_u32(req);
    backlog = bs_get_u16(req);
    if (!bs_cur_ok(req) || !bs_cur_done(req)) return BS_BADLEN;

    e = want_sock(h, BS_R_ACCEPT, &s);
    if (e != BS_OK) return e;

    e = sys_listen(s->fd, backlog);
    if (e != BS_OK) return e;

    /* The kind changes, and that is the point of having kinds. A listener
     * cannot be connected and cannot be read; both are refused from the table
     * now, before the op runs, rather than by the kernel later with an errno
     * the two platforms spell differently. */
    s->kind   = BS_HK_LISTENER;
    s->rights = (bs_u16)(s->rights & ~(bs_u16)(BS_R_CONNECT | BS_R_READ | BS_R_WRITE));
    return BS_OK;
}

/* ABI.md section 7.9. Request: handle{u32} flags{u16}. Reply: handle{u32}
 * then the peer address, thirty six bytes. */
bs_err op_net_accept(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    bs_u32 h, nh;
    unsigned int flags;
    struct bs_slot *s;
    bs_addr peer;
    bs_osfd fd;
    bs_err e;
    (void)ctx;

    h     = (bs_u32)bs_get_u32(req);
    flags = bs_get_u16(req);
    if (!bs_cur_ok(req) || !bs_cur_done(req)) return BS_BADLEN;

    e = want_sock(h, BS_R_ACCEPT, &s);
    if (e != BS_OK) return e;
    /* accept on a socket that was never listened on is refused here, from the
     * kind, which is the example ABI.md section 5 uses when it says a handle
     * carries a kind at all. */
    if (s->kind != BS_HK_LISTENER) return BS_BADF;

    memset(&peer, 0, sizeof peer);
    e = sys_accept(s->fd, (flags & 1u) ? 1 : 0, &fd, &peer);
    if (e != BS_OK) return e;

    e = bs_fdtab_alloc(BS_HK_SOCKET, (bs_u16)(BS_R_READ | BS_R_WRITE), fd, &nh);
    if (e != BS_OK) { sys_close(fd); return e; }

    bs_put_u32(rep, nh);
    addr_put(rep, &peer);
    return bs_buf_ok(rep) ? BS_OK : BS_IO;
}
