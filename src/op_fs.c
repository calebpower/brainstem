/* op_fs.c — open, seek, stat, readdir, unlink, mkdir, rename.
 *
 * Everything here resolves beneath a directory handle. There is no op in this
 * file that takes a path on its own, which is what makes "nothing is
 * reachable that was not named on the command line" a property of the
 * interface rather than of anyone remembering to check.
 *
 * WHAT THAT IS AND IS NOT. ABI.md section 8 says it plainly and so does this
 * file: it is a usability and determinism property, not a containment
 * boundary. The broker runs with its operator's credentials. At M4 the
 * confinement is the broker's own string check -- see path_of -- which cannot
 * see a symlink pointing upward. Asking the kernel instead is M8's job,
 * beside sys_lockdown, and sys_beneath_is_kernel() reports which of the two
 * is in force so the suite can check the behaviour either way.
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

/* Handle, then KIND, then rights, and the order is the point.
 *
 * All three are free, so the only thing the order decides is which answer the
 * program gets, and the most specific true one is the most useful. A handle
 * that was never issued is BADF. A handle that is a file is NOTDIR -- checked
 * from the table before the op runs, so readdir on a regular file says so
 * rather than something stranger further down on whichever platform noticed
 * first. Only a real directory the preopen did not grant this right on is
 * DENIED. Checking rights first would collapse the middle case into the last
 * and tell a program its capability was wrong when its handle was. */
static bs_err want_dir(bs_u32 h, bs_u16 rights, struct bs_slot **out) {
    struct bs_slot *s = bs_fdtab_get(h);
    if (!s) return BS_BADF;
    if (s->kind != BS_HK_DIR) return BS_NOTDIR;
    if ((s->rights & rights) != rights) return BS_DENIED;
    *out = s;
    return BS_OK;
}

/* Resolve the `dir` field of a path op.
 *
 * BS_HANDLE_NONE means "the way any other process would": against the
 * broker's own working directory, or absolutely if the path begins with '/'.
 * That is the ordinary case now, and a real directory handle is the special
 * one -- it is openat-beneath, which readdir needs and which a program
 * walking a tree will want.
 *
 * There is no parent to inherit rights from in the NONE case, so the mask is
 * everything: what the resulting handle can do is decided by the flags the
 * open asked for, and by the kernel. Rights still narrow through derived
 * handles; they are simply no longer bounded by a command line. */
static bs_err resolve_dir(bs_u32 h, bs_u16 need, bs_osfd *fd, bs_u16 *parent) {
    struct bs_slot *d;
    bs_err e;
    if (h == BS_HANDLE_NONE) {
        *fd = BS_OSFD_CWD;
        *parent = 0xFFFFu;
        return BS_OK;
    }
    e = want_dir(h, need, &d);
    if (e != BS_OK) return e;
    *fd = d->fd;
    *parent = d->rights;
    return BS_OK;
}

/* Copy the rest of the payload out as a path, having checked it.
 *
 * The rule itself lives in path.c, because op_proc needs the same one and a
 * security-relevant rule written down twice is a rule that will be corrected
 * once. This function is the copy and the length bound; bs_path_check is the
 * judgement. */
static bs_err path_of(struct bs_cur *c, char *buf, size_t cap, int allow_empty) {
    size_t n = bs_cur_left(c);
    const unsigned char *p = bs_get_bytes(c, n);
    bs_err e;

    if (!bs_cur_ok(c)) return BS_BADLEN;
    if (n == 0) {
        if (!allow_empty) return BS_INVAL;
        buf[0] = '\0';
        return BS_OK;
    }
    if (n >= cap) return BS_NAMETOOLONG;
    e = bs_path_check(p, n);
    if (e != BS_OK) return e;

    memcpy(buf, p, n);
    buf[n] = '\0';
    return BS_OK;
}

/* ABI.md section 7.17. Request: dir{u32} oflags{u16} mode{u16} path.
 * Reply: handle{u32}. */
bs_err op_fs_open(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    char path[BS_PATH_MAX];
    bs_u32 dh;
    unsigned int oflags, mode;
    bs_osfd dfd, fd;
    bs_u16 parent;
    bs_u32 h;
    bs_u16 need = BS_R_READ, give;
    bs_err e;
    bs_stat st;
    (void)ctx;

    dh     = (bs_u32)bs_get_u32(req);
    oflags = bs_get_u16(req);
    mode   = bs_get_u16(req);
    if (!bs_cur_ok(req)) return BS_BADLEN;

    /* An unknown flag bit is refused rather than ignored. A program built
     * against a later minor version, using a bit this broker has never heard
     * of, must not silently get an open without it. */
    if (oflags & ~(unsigned int)BS_O_KNOWN) return BS_INVAL;

    e = path_of(req, path, sizeof path, 0);
    if (e != BS_OK) return e;

    /* RIGHTS ONLY EVER NARROW. What the open asks for is checked against the
     * directory's rights first, and what the new handle gets is the
     * intersection -- so no sequence of opens can arrive at a handle that can
     * do more than the preopen it descends from. */
    if (oflags & BS_O_WRITE)  need = (bs_u16)(need | BS_R_WRITE);
    if (oflags & BS_O_CREATE) need = (bs_u16)(need | BS_R_CREATE);
    e = resolve_dir(dh, need, &dfd, &parent);
    if (e != BS_OK) return e;

    e = sys_open(dfd, path, oflags, mode & 0777u, &fd);
    if (e != BS_OK) return e;

    /* The kind comes from the descriptor, not from the flags. A program can
     * open a directory without saying O_DIRECTORY, and a handle whose kind
     * was a guess would be refused by the wrong op later. */
    if (sys_stat(fd, 0, 0, &st) != BS_OK) { sys_close(fd); return BS_IO; }

    give = (bs_u16)(parent & ((oflags & BS_O_READ  ? BS_R_READ  : 0) |
                                 (oflags & BS_O_WRITE ? BS_R_WRITE : 0) |
                                 BS_R_SEEK | BS_R_CREATE | BS_R_DELETE | BS_R_LIST));
    /* A regular file cannot be listed and a directory cannot be seeked
     * through by this ABI, so those rights are dropped here rather than left
     * to be refused one op later with a less useful status. */
    if (st.type == BS_FT_DIR) give = (bs_u16)(give & ~(bs_u16)BS_R_SEEK);
    else                      give = (bs_u16)(give & ~(bs_u16)(BS_R_LIST | BS_R_CREATE | BS_R_DELETE));

    e = bs_fdtab_alloc(st.type == BS_FT_DIR ? BS_HK_DIR : BS_HK_FILE, give, fd, &h);
    if (e != BS_OK) { sys_close(fd); return e; }

    bs_put_u32(rep, h);
    return bs_buf_ok(rep) ? BS_OK : BS_IO;
}

/* ABI.md section 7.18. Request: handle{u32} offset{u64} whence{u8}, thirteen
 * bytes. Reply: pos{u64}.
 *
 * There is no signed field anywhere in this ABI and seek is why the rule
 * survives: the direction is a one-byte constant and the offset is an
 * unsigned magnitude, so a backwards seek costs the program a different
 * literal instead of an eight-limb two's complement on a tape. */
bs_err op_fs_seek(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    bs_u32 h;
    bs_u64 off;
    unsigned int whence;
    struct bs_slot *s;
    bs_u64 pos = 0;
    bs_err e;
    (void)ctx;

    h      = (bs_u32)bs_get_u32(req);
    off    = (bs_u64)bs_get_u64(req);
    whence = bs_get_u8(req);
    if (!bs_cur_ok(req) || !bs_cur_done(req)) return BS_BADLEN;

    e = want(h, BS_R_SEEK, &s);
    if (e != BS_OK) return e;

    e = sys_seek(s->fd, off, whence, &pos);
    if (e != BS_OK) return e;

    bs_put_u64(rep, pos);
    return bs_buf_ok(rep) ? BS_OK : BS_IO;
}

/* ABI.md section 7.19. Request: dir{u32} flags{u16} path. A zero pathlen
 * stats the handle itself. Reply: the 32 byte record. */
bs_err op_fs_stat(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    char path[BS_PATH_MAX];
    bs_u32 dh;
    unsigned int flags;
    bs_osfd dfd = BS_OSFD_CWD;
    bs_u16 parent;
    bs_stat st;
    bs_err e;
    (void)ctx;

    dh    = (bs_u32)bs_get_u32(req);
    flags = bs_get_u16(req);
    if (!bs_cur_ok(req)) return BS_BADLEN;

    e = path_of(req, path, sizeof path, 1);
    if (e != BS_OK) return e;

    /* A path stats beneath a directory; an empty path stats whatever the
     * handle is, which may be a file. So the directory requirement applies
     * only when there is something to resolve. */
    /* A path stats beneath a directory; an empty path stats whatever the
     * handle is, which may be a file -- so the directory requirement applies
     * only when there is something to resolve. An empty path with no handle
     * at all would be asking about nothing, and is INVAL. */
    if (path[0]) {
        e = resolve_dir(dh, BS_R_READ, &dfd, &parent);
    } else if (dh == BS_HANDLE_NONE) {
        return BS_INVAL;
    } else {
        struct bs_slot *d;
        e = want(dh, BS_R_READ, &d);
        if (e == BS_OK) dfd = d->fd;
    }
    if (e != BS_OK) return e;

    e = sys_stat(dfd, path, (flags & 1u) ? 1 : 0, &st);
    if (e != BS_OK) return e;

    bs_put_u8 (rep, st.type);
    bs_put_u8 (rep, st.readable);
    bs_put_u8 (rep, st.writable);
    bs_put_u8 (rep, st.executable);
    bs_put_u64(rep, st.size);
    bs_put_u64(rep, st.mtime_sec);
    bs_put_u32(rep, st.mtime_nsec);
    bs_put_u16(rep, st.mode);
    bs_put_zero(rep, 6);            /* reserved; ino can come back here later */
    return bs_buf_ok(rep) ? BS_OK : BS_IO;
}

/* ABI.md section 7.20. Request: dir_handle{u32} flags{u16}, six bytes.
 * Reply: zero bytes meaning END, or type{u8} namelen{u8} reserved{u16} name.
 *
 * ONE ENTRY PER CALL. A batch would be a run of variable length records the
 * consumer walks by repeatedly adding a length to a cursor, which is
 * precisely the operation brainfuck cannot do cheaply and precisely what
 * bfsodium's "conveyors, not indices" was learned from. Against that: one
 * extra round trip per entry, measured in microseconds, against an
 * interpreter spending 10^6 to 10^9 instructions between calls. The round
 * trip is free at this timescale. There is no trade-off here, only an
 * apparent one. */
bs_err op_fs_readdir(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    char name[256];
    bs_u32 dh;
    unsigned int flags;
    struct bs_slot *d;
    bs_u8 type = BS_FT_UNKNOWN;
    size_t namelen = 0;
    int end = 0;
    bs_err e;
    (void)ctx;

    dh    = (bs_u32)bs_get_u32(req);
    flags = bs_get_u16(req);
    if (!bs_cur_ok(req) || !bs_cur_done(req)) return BS_BADLEN;

    e = want_dir(dh, BS_R_LIST, &d);
    if (e != BS_OK) return e;

    /* The walk is opened on first use and lives on the slot, so the program
     * does not have to hold a second handle for it and closing the directory
     * closes the walk. */
    if (!d->dir) {
        e = sys_dir_open(d->fd, &d->dir);
        if (e != BS_OK) return e;
    } else if (flags & 1u) {
        e = sys_dir_rewind(d->dir);
        if (e != BS_OK) return e;
    }

    e = sys_dir_next(d->dir, d->fd, &type, name, sizeof name, &namelen, &end);
    if (e != BS_OK) return e;

    /* END is an empty reply rather than a status, because the program is
     * already reading a length and a zero length costs it nothing to test --
     * whereas branching on a status byte costs a decrement per code. */
    if (end) return BS_END;

    bs_put_u8(rep, type);
    bs_put_u8(rep, (unsigned int)namelen);
    bs_put_zero(rep, 2);
    bs_put_bytes(rep, (const unsigned char *)name, namelen);
    return bs_buf_ok(rep) ? BS_OK : BS_IO;
}

/* ABI.md section 7.21. Request: dir{u32} flags{u16} path. Flags bit 0 is
 * REMOVEDIR, and there is no rmdir op BECAUSE of that bit: it is one syscall
 * on both platforms, so a separate opcode would be ABI surface for a bit. */
bs_err op_fs_unlink(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    char path[BS_PATH_MAX];
    bs_u32 dh;
    unsigned int flags;
    bs_osfd dfd;
    bs_u16 parent;
    bs_err e;
    (void)ctx; (void)rep;

    dh    = (bs_u32)bs_get_u32(req);
    flags = bs_get_u16(req);
    if (!bs_cur_ok(req)) return BS_BADLEN;

    e = path_of(req, path, sizeof path, 0);
    if (e != BS_OK) return e;
    e = resolve_dir(dh, BS_R_DELETE, &dfd, &parent);
    if (e != BS_OK) return e;

    return sys_unlink(dfd, path, (flags & 1u) ? 1 : 0);
}

/* ABI.md section 7.22. Request: dir{u32} mode{u16} path. */
bs_err op_fs_mkdir(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    char path[BS_PATH_MAX];
    bs_u32 dh;
    unsigned int mode;
    bs_osfd dfd;
    bs_u16 parent;
    bs_err e;
    (void)ctx; (void)rep;

    dh   = (bs_u32)bs_get_u32(req);
    mode = bs_get_u16(req);
    if (!bs_cur_ok(req)) return BS_BADLEN;

    e = path_of(req, path, sizeof path, 0);
    if (e != BS_OK) return e;
    e = resolve_dir(dh, BS_R_CREATE, &dfd, &parent);
    if (e != BS_OK) return e;

    return sys_mkdir(dfd, path, mode & 0777u);
}

/* ABI.md section 7.23. Request: olddir{u32} newdir{u32} oldlen{u16}
 * newlen{u16} oldpath newpath.
 *
 * Both lengths are hoisted into the fixed prefix so the program emits a fully
 * fixed twelve byte header and then two flat runs. It knows both lengths
 * before it starts, so this costs it nothing and saves it a cursor.
 *
 * No NOREPLACE flag: Linux has renameat2, FreeBSD has no equivalent, and an
 * op that behaved differently on the primary and the secondary platform is
 * the one thing this ABI will not ship. */
bs_err op_fs_rename(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    char oldp[BS_PATH_MAX], newp[BS_PATH_MAX];
    bs_u32 odh, ndh;
    unsigned int olen, nlen;
    bs_osfd ofd, nfd;
    bs_u16 parent;
    const unsigned char *p;
    bs_err e;
    (void)ctx; (void)rep;

    odh  = (bs_u32)bs_get_u32(req);
    ndh  = (bs_u32)bs_get_u32(req);
    olen = bs_get_u16(req);
    nlen = bs_get_u16(req);
    if (!bs_cur_ok(req)) return BS_BADLEN;

    /* The two declared lengths must account for the payload EXACTLY. This is
     * the op most able to desync a conversation, because it is the only one
     * carrying two variable runs, so the check is equality rather than a
     * bound. */
    if (bs_cur_left(req) != (size_t)olen + (size_t)nlen) return BS_BADLEN;
    if (olen == 0 || nlen == 0) return BS_INVAL;
    if (olen >= sizeof oldp || nlen >= sizeof newp) return BS_NAMETOOLONG;

    p = bs_get_bytes(req, olen);
    if (!bs_cur_ok(req)) return BS_BADLEN;
    memcpy(oldp, p, olen); oldp[olen] = '\0';
    p = bs_get_bytes(req, nlen);
    if (!bs_cur_ok(req)) return BS_BADLEN;
    memcpy(newp, p, nlen); newp[nlen] = '\0';

    /* Both halves go through the same refusals as every other path, by
     * running them back through the same checker rather than by repeating
     * the rules. A second copy of the traversal check is a second thing to
     * get wrong. */
    {
        struct bs_cur c;
        char tmp[BS_PATH_MAX];
        bs_cur_init(&c, (const unsigned char *)oldp, olen);
        e = path_of(&c, tmp, sizeof tmp, 0);
        if (e != BS_OK) return e;
        bs_cur_init(&c, (const unsigned char *)newp, nlen);
        e = path_of(&c, tmp, sizeof tmp, 0);
        if (e != BS_OK) return e;
    }

    e = resolve_dir(odh, BS_R_DELETE, &ofd, &parent);
    if (e != BS_OK) return e;
    e = resolve_dir(ndh, BS_R_CREATE, &nfd, &parent);
    if (e != BS_OK) return e;

    return sys_rename(ofd, oldp, nfd, newp);
}
