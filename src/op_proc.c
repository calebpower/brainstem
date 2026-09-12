/* op_proc.c — pipe, spawn and wait.
 *
 * The payoff. These three are what make brainfuck itself the harness: a
 * brainfuck program can start an interpreter on a second brainfuck program,
 * write into its stdin, read its stdout, and collect its status. Everything
 * before this milestone was a program talking to an operating system; this is
 * a program talking to another program.
 *
 * Last, because it is the hairiest: descriptor leaks, zombies, and the
 * child's exit racing the broker's own reaping of its interpreter.
 */
#include <string.h>

#include "ops.h"
#include "fdtab.h"
#include "path.h"

static bs_err want(bs_u32 h, bs_u16 rights, struct bs_slot **out) {
    struct bs_slot *s = bs_fdtab_get(h);
    if (!s) return BS_BADF;
    if ((s->rights & rights) != rights) return BS_DENIED;
    *out = s;
    return BS_OK;
}

/* ABI.md section 7.14. Request: flags{u16}. Reply: read_handle{u32}
 * write_handle{u32}.
 *
 * Two handles from one op, and in that order, because a program almost always
 * wants to keep one and hand the other to a child -- and reading them in a
 * fixed order costs it nothing while searching for them would cost a
 * comparison brainfuck does not have. */
bs_err op_proc_pipe(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    unsigned int flags;
    bs_osfd rd, wr;
    bs_u32 hr, hw;
    bs_err e;
    (void)ctx;

    flags = bs_get_u16(req);
    if (!bs_cur_ok(req) || !bs_cur_done(req)) return BS_BADLEN;
    if (flags != 0) return BS_INVAL;      /* no flag bits are defined yet */

    e = sys_pipe(&rd, &wr);
    if (e != BS_OK) return e;

    e = bs_fdtab_alloc(BS_HK_PIPE_R, BS_R_READ, rd, &hr);
    if (e != BS_OK) { sys_close(rd); sys_close(wr); return e; }
    e = bs_fdtab_alloc(BS_HK_PIPE_W, BS_R_WRITE, wr, &hw);
    if (e != BS_OK) { bs_fdtab_free(hr); sys_close(wr); return e; }

    bs_put_u32(rep, hr);
    bs_put_u32(rep, hw);
    return bs_buf_ok(rep) ? BS_OK : BS_IO;
}

/* Strings for execve, copied out of the request into one fixed arena.
 *
 * No allocation on the ABI path, here as everywhere, so the arena is a fixed
 * size and an argv that does not fit is OVERFLOW -- a status the program can
 * act on rather than a truncation it cannot detect. Eight kilobytes is far
 * more than a brainfuck program will ever assemble by hand.
 *
 * The tempting alternative is to NUL-terminate the strings in place inside
 * the request buffer, since they are already contiguous there. Each string is
 * immediately followed by the next one's length prefix, so that would mean
 * parsing every offset first and then writing terminators backwards. It
 * works, it is clever, and it destroys the frame the trace just printed.
 * Copying is duller and can be read. */
struct sargs {
    char  arena[8192];
    size_t used;
    char *v[BS_SPAWN_MAXV + 1];
    size_t n;
};

static bs_err take_string(struct bs_cur *c, struct sargs *a, char **out) {
    unsigned int len = bs_get_u16(c);
    const unsigned char *p;
    if (!bs_cur_ok(c)) return BS_BADLEN;
    p = bs_get_bytes(c, len);
    if (!bs_cur_ok(c)) return BS_BADLEN;
    if (a->used + len + 1 > sizeof a->arena) return BS_OVERFLOW;
    if (memchr(p, '\0', len)) return BS_INVAL;   /* a NUL would truncate it */
    *out = a->arena + a->used;
    memcpy(a->arena + a->used, p, len);
    a->arena[a->used + len] = '\0';
    a->used += len + 1;
    return BS_OK;
}

/* ABI.md section 7.15. Request: dir{u32} flags{u16} nfdmap{u8} nargv{u8}
 * nenv{u8} reserved{u8}, then nfdmap descriptor mappings, then the path and
 * the argv and env strings, each u16-prefixed. Reply: a process handle.
 *
 * A PROCESS HANDLE, NEVER A PID. Pids differ between runs and between
 * platforms and the wire must not; the program gets a handle out of the same
 * table every other handle comes from, with the same generation tag and the
 * same refusal when it goes stale.
 *
 * THE ENVIRONMENT IS EXPLICIT AND NEVER INHERITED. A child gets exactly nenv
 * variables, which is a capability decision -- the broker's own environment is
 * not the program's to pass on -- and it removes an obvious source of replay
 * nondeterminism at the same time. */
bs_err op_proc_spawn(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    struct sargs args;
    char *envv[BS_SPAWN_MAXV + 1];
    bs_fdmap map[BS_SPAWN_MAXFD];
    bs_u32 dh;
    unsigned int flags, nfdmap, nargv, nenv;
    struct bs_slot *d;
    bs_osfd dfd;
    char *path;
    bs_i64 pid = 0;
    bs_u32 h;
    size_t i, nenvv = 0;
    bs_err e;
    (void)ctx;

    dh     = (bs_u32)bs_get_u32(req);
    flags  = bs_get_u16(req);
    nfdmap = bs_get_u8(req);
    nargv  = bs_get_u8(req);
    nenv   = bs_get_u8(req);
    (void)bs_get_u8(req);                 /* reserved */
    if (!bs_cur_ok(req)) return BS_BADLEN;
    if (flags != 0) return BS_INVAL;
    if (nfdmap > BS_SPAWN_MAXFD) return BS_INVAL;
    if (nargv == 0 || nargv > BS_SPAWN_MAXV || nenv > BS_SPAWN_MAXV) return BS_INVAL;

    args.used = 0;
    args.n = 0;

    /* The map is resolved through the handle table, so a program can only
     * hand a child a descriptor it already holds -- and the generation check
     * means it cannot hand over one it closed. */
    for (i = 0; i < nfdmap; i++) {
        bs_u32 fh;
        struct bs_slot *fs;
        map[i].child_fd = bs_get_u8(req);
        fh = (bs_u32)bs_get_u32(req);
        if (!bs_cur_ok(req)) return BS_BADLEN;
        fs = bs_fdtab_get(fh);
        if (!fs) return BS_BADF;
        if (fs->kind == BS_HK_PROC) return BS_BADF;   /* not a descriptor */
        map[i].fd = fs->fd;
    }

    e = take_string(req, &args, &path);
    if (e != BS_OK) return e;
    /* The same rule the filesystem ops use, from the same place. spawn
     * fchdirs to the directory handle and execs a relative path, so ".."
     * here escapes exactly as it would in open -- and a second copy of the
     * check is a second thing to get wrong. */
    {
        size_t plen = 0;
        while (path[plen]) plen++;
        e = bs_path_check((const unsigned char *)path, plen);
        if (e != BS_OK) return e;
    }

    for (i = 0; i < nargv; i++) {
        e = take_string(req, &args, &args.v[i]);
        if (e != BS_OK) return e;
    }
    args.v[nargv] = 0;

    for (i = 0; i < nenv; i++) {
        e = take_string(req, &args, &envv[i]);
        if (e != BS_OK) return e;
        nenvv++;
    }
    envv[nenvv] = 0;

    if (!bs_cur_done(req)) return BS_BADLEN;

    /* BS_HANDLE_NONE resolves the path the way any other process would:
     * against the broker's working directory, or absolutely. A real
     * directory handle makes the child START there instead, which is how a
     * program runs something out of a tree it has walked to. */
    if (dh == BS_HANDLE_NONE) {
        dfd = BS_OSFD_CWD;
    } else {
        e = want(dh, (bs_u16)(BS_R_READ | BS_R_EXEC), &d);
        if (e != BS_OK) return e;
        if (d->kind != BS_HK_DIR) return BS_NOTDIR;
        dfd = d->fd;
    }

    e = sys_spawn(dfd, path, args.v, envv, map, nfdmap, &pid);
    if (e != BS_OK) return e;

    e = bs_fdtab_alloc(BS_HK_PROC, 0, BS_OSFD_NONE, &h);
    if (e != BS_OK) return e;             /* the child is running and orphaned */
    {
        struct bs_slot *s = bs_fdtab_get(h);
        s->pid    = pid;
        s->reaped = 0;
    }

    bs_put_u32(rep, h);
    return bs_buf_ok(rep) ? BS_OK : BS_IO;
}

/* ABI.md section 7.16. Request: proc{u32} flags{u16}. Reply: state{u8}
 * code{u8} signal{u8} reserved{u8}.
 *
 * IDEMPOTENT AFTER REAPING. A process can only be waited for once by the
 * kernel, so the status is cached on the handle and repeated until the handle
 * is closed. Without that, a program that asked twice would get NOCHILD the
 * second time and have no way to tell that from a handle it made up. */
bs_err op_proc_wait(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    bs_u32 h;
    unsigned int flags;
    struct bs_slot *s;
    bs_u8 state = BS_PS_RUNNING, code = 0, sig = 0;
    bs_err e;
    (void)ctx;

    h     = (bs_u32)bs_get_u32(req);
    flags = bs_get_u16(req);
    if (!bs_cur_ok(req) || !bs_cur_done(req)) return BS_BADLEN;

    s = bs_fdtab_get(h);
    if (!s) return BS_BADF;
    if (s->kind != BS_HK_PROC) return BS_BADF;

    if (s->reaped) {
        state = s->pstate; code = s->pcode; sig = s->psig;
    } else {
        e = sys_wait(s->pid, (flags & 1u) ? 1 : 0, &state, &code, &sig);
        if (e != BS_OK) return e;
        if (state != BS_PS_RUNNING) {
            s->reaped = 1;
            s->pstate = state; s->pcode = code; s->psig = sig;
        }
    }

    bs_put_u8(rep, state);
    bs_put_u8(rep, code);
    bs_put_u8(rep, sig);
    bs_put_u8(rep, 0);
    return bs_buf_ok(rep) ? BS_OK : BS_IO;
}
