/* sys_net.c — the socket half of the seam, and the portable half of it.
 *
 * Split out of sys_posix.c rather than added to it because this is where the
 * two kernels disagree most and the file is worth reading on its own:
 * AF_INET6 is 28 on FreeBSD and 10 on Linux, and FreeBSD's sockaddr_in
 * carries a leading sin_len byte that Linux's does not. Every one of those
 * numbers dies in this file. Nothing above it can name one.
 *
 * NO NAMESPACE WIDENING MACRO, here as in sys_posix.c. Everything used below
 * is POSIX.1-2008, which is exactly why the address families and the socket
 * types are the only things that needed a mapping at all.
 */
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "sys.h"

bs_err sys_errmap(int e);

static int fdof(bs_osfd f) { return (int)f; }

/* ---- families and types ------------------------------------------------- */

static int af_of(bs_u32 d, int *ok) {
    *ok = 1;
    switch (d) {
    case BS_AF_INET:  return AF_INET;
    case BS_AF_INET6: return AF_INET6;
    default: *ok = 0; return 0;
    }
}

static int type_of(bs_u32 t, int *ok) {
    *ok = 1;
    switch (t) {
    case BS_SOCK_STREAM: return SOCK_STREAM;
    case BS_SOCK_DGRAM:  return SOCK_DGRAM;
    default: *ok = 0; return 0;
    }
}

/* ---- the address record, both directions -------------------------------- */

/* A sockaddr_storage is the only place in this program where a POSIX socket
 * type is named, and it never leaves this file. */
static bs_err to_sockaddr(const bs_addr *a, struct sockaddr_storage *ss, socklen_t *len) {
    memset(ss, 0, sizeof *ss);
    if (a->family == BS_AF_INET) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)ss;
        v4->sin_family = AF_INET;
        /* The port is host order on the wire and network order here, and the
         * swap is the broker's job precisely so that a brainfuck program
         * never has to think about byte order twice in one record. */
        v4->sin_port = htons((unsigned short)a->port);
        /* Bytes 0..3 of the body are the quad in READING ORDER, so they are
         * already exactly what s_addr holds on the network. No swap: this is
         * a byte string, not an integer. */
        memcpy(&v4->sin_addr, a->body, 4);
        *len = (socklen_t)sizeof *v4;
        return BS_OK;
    }
    if (a->family == BS_AF_INET6) {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)ss;
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons((unsigned short)a->port);
        memcpy(&v6->sin6_addr, a->body, 16);
        *len = (socklen_t)sizeof *v6;
        return BS_OK;
    }
    /* Family 3 Unix is declared by the ABI and not implemented in v1. See
     * sys_net_family_supported() for the reason, which is a platform
     * divergence rather than a shortage of time. */
    return BS_NOTSUP;
}

static void from_sockaddr(const struct sockaddr_storage *ss, bs_addr *a) {
    memset(a, 0, sizeof *a);
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)ss;
        a->family = BS_AF_INET;
        a->port   = (bs_u16)ntohs(v4->sin_port);
        memcpy(a->body, &v4->sin_addr, 4);
        return;
    }
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)ss;
        a->family = BS_AF_INET6;
        a->port   = (bs_u16)ntohs(v6->sin6_port);
        memcpy(a->body, &v6->sin6_addr, 16);
        return;
    }
    a->family = BS_AF_NONE;
}

/* UNIX SOCKETS ARE DECLARED AND NOT BUILT, and the reason is the same one
 * that keeps RENAME_NOREPLACE out of the ABI.
 *
 * Every filesystem path in this ABI resolves beneath a preopened directory
 * handle, so a Unix socket address is a directory handle plus a relative
 * path. FreeBSD has bindat(2) and connectat(2), which take exactly that.
 * Linux has neither, and the workarounds -- fchdir around the call, or
 * /proc/self/fd/N -- are respectively racy and Linux-only.
 *
 * An op that worked on the primary platform and needed a different mechanism
 * on the secondary one is the thing this project refuses to ship, so family 3
 * answers NOTSUP on both until there is a mechanism that is the same on both.
 * Declared rather than removed, because the record layout is already
 * specified and a later minor version may fill it in. */
bs_u8 sys_net_family_supported(bs_u32 family) {
    return (family == BS_AF_INET || family == BS_AF_INET6) ? 1 : 0;
}

/* DECLARED is not the same as BUILT, and the two statuses are not the same
 * answer. Family 3 is in the ABI and this broker does not implement it, which
 * is NOTSUP -- a program built against a later minor version should be told
 * "not here" rather than "no such thing". Family 9 is not in the ABI at all
 * and is INVAL. The first version of this collapsed the two and reported Unix
 * as a malformed value, which would have sent someone looking at their own
 * encoder. */
static bs_u8 family_declared(bs_u32 family) {
    return (family >= BS_AF_INET && family <= BS_AF_UNIX) ? 1 : 0;
}

/* ---- the ops ------------------------------------------------------------ */

bs_err sys_socket(bs_u32 domain, bs_u32 type, bs_osfd *out) {
    int af, ty, ok1, ok2, fd;
    af = af_of(domain, &ok1);
    ty = type_of(type, &ok2);
    if (!ok1) return family_declared(domain) ? BS_NOTSUP : BS_INVAL;
    if (!ok2) return BS_INVAL;

    fd = socket(af, ty, 0);
    if (fd < 0) return sys_errmap(errno);

    /* SOCK_CLOEXEC would do this in the same call and is on both platforms,
     * but it is not POSIX.1-2008 and this file is compiled without a
     * namespace widening macro. The extra fcntl is deterministic, portable,
     * and visible in the syscall tier, which is worth more than saving one
     * call. There is no race: the broker is single threaded and the only
     * fork it does is the interpreter's, from the same loop. */
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) { int e = errno; close(fd); return sys_errmap(e); }
    *out = (bs_osfd)fd;
    return BS_OK;
}

bs_err sys_connect(bs_osfd fd, const bs_addr *a, int nowait, int *state) {
    struct sockaddr_storage ss;
    socklen_t len = 0;
    bs_err e;
    int r;

    e = to_sockaddr(a, &ss, &len);
    if (e != BS_OK) return e;

    if (!nowait) {
        /* A blocking connect on a socket that was left non-blocking by an
         * abandoned NOWAIT attempt would return immediately and lie. Refuse
         * rather than guess: mixing the two on one handle is a program bug
         * and this is the only place it can be seen. */
        if (*state) return BS_ISCONN;
        do { r = connect(fdof(fd), (struct sockaddr *)&ss, len); } while (r < 0 && errno == EINTR);
        if (r < 0) return sys_errmap(errno);
        return BS_OK;
    }

    if (*state) {
        /* The re-issue. SO_ERROR is how a finished non-blocking connect
         * reports itself, and reading it here is what lets the ABI have no
         * getsockopt op at all -- the program re-sends the identical frame
         * and the broker knows what it means. */
        int err = 0;
        socklen_t elen = (socklen_t)sizeof err;
        if (getsockopt(fdof(fd), SOL_SOCKET, SO_ERROR, &err, &elen) < 0)
            return sys_errmap(errno);
        if (err == EINPROGRESS || err == EALREADY) return BS_AGAIN;
        *state = 0;
        if (err != 0) return sys_errmap(err);
        /* Back to blocking: the socket's mode is the broker's business and a
         * later read without NOWAIT must actually wait. */
        {
            int fl = fcntl(fdof(fd), F_GETFL, 0);
            if (fl >= 0) fcntl(fdof(fd), F_SETFL, fl & ~O_NONBLOCK);
        }
        return BS_OK;
    }

    {
        int fl = fcntl(fdof(fd), F_GETFL, 0);
        if (fl < 0) return sys_errmap(errno);
        if (fcntl(fdof(fd), F_SETFL, fl | O_NONBLOCK) < 0) return sys_errmap(errno);
        do { r = connect(fdof(fd), (struct sockaddr *)&ss, len); } while (r < 0 && errno == EINTR);
        if (r == 0) {
            fcntl(fdof(fd), F_SETFL, fl);
            return BS_OK;
        }
        if (errno == EINPROGRESS) { *state = 1; return BS_AGAIN; }
        {
            int e2 = errno;
            fcntl(fdof(fd), F_SETFL, fl);
            return sys_errmap(e2);
        }
    }
}

bs_err sys_bind(bs_osfd fd, const bs_addr *a, int reuseaddr, bs_addr *bound) {
    struct sockaddr_storage ss;
    socklen_t len = 0;
    bs_err e;

    e = to_sockaddr(a, &ss, &len);
    if (e != BS_OK) return e;

    if (reuseaddr) {
        int on = 1;
        if (setsockopt(fdof(fd), SOL_SOCKET, SO_REUSEADDR, &on, (socklen_t)sizeof on) < 0)
            return sys_errmap(errno);
    }
    if (bind(fdof(fd), (struct sockaddr *)&ss, len) < 0) return sys_errmap(errno);

    /* The address actually bound, which is the whole point of bind having a
     * reply: port 0 means "any" and there is no getsockname op, so this is
     * the only way a program can ever learn the ephemeral port it was given. */
    {
        struct sockaddr_storage got;
        socklen_t glen = (socklen_t)sizeof got;
        memset(&got, 0, sizeof got);
        if (getsockname(fdof(fd), (struct sockaddr *)&got, &glen) < 0) return sys_errmap(errno);
        from_sockaddr(&got, bound);
    }
    return BS_OK;
}

bs_err sys_listen(bs_osfd fd, bs_u32 backlog) {
    /* 0 means 128, per ABI.md section 7.6 to 7.9, because zero is the byte a
     * brainfuck program emits for free and "the sensible default" is what it
     * should get for free. */
    int b = backlog == 0 ? 128 : (backlog > 4096 ? 4096 : (int)backlog);
    if (listen(fdof(fd), b) < 0) return sys_errmap(errno);
    return BS_OK;
}

bs_err sys_accept(bs_osfd fd, int nowait, bs_osfd *out, bs_addr *peer) {
    struct sockaddr_storage ss;
    socklen_t len = (socklen_t)sizeof ss;
    int n;

    memset(&ss, 0, sizeof ss);

    if (nowait) {
        /* A zero timeout poll rather than toggling O_NONBLOCK, for the same
         * reason sys_read does it that way: toggling costs two fcntls, leaves
         * the descriptor in a different state if anything returns early, and
         * turns one op into three syscalls. */
        struct pollfd pf;
        int r;
        pf.fd = fdof(fd); pf.events = POLLIN; pf.revents = 0;
        do { r = poll(&pf, 1, 0); } while (r < 0 && errno == EINTR);
        if (r < 0) return sys_errmap(errno);
        if (r == 0) return BS_AGAIN;
    }

    do { n = accept(fdof(fd), (struct sockaddr *)&ss, &len); } while (n < 0 && errno == EINTR);
    if (n < 0) return sys_errmap(errno);
    if (fcntl(n, F_SETFD, FD_CLOEXEC) < 0) { int e = errno; close(n); return sys_errmap(e); }

    from_sockaddr(&ss, peer);
    *out = (bs_osfd)n;
    return BS_OK;
}
