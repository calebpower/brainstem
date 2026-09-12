/* sys_posix.c — what both platforms genuinely share.
 *
 * The bulk of the seam lives here. sys_freebsd.c and sys_linux.c hold only
 * what actually differs, which through M4 is randomness and the platform
 * byte -- and that is the point of having started the seam with time and
 * rand rather than with something larger. The shape came from a real
 * divergence (arc4random_buf against getrandom) rather than from a guess
 * about what might diverge later, and the filesystem half below inherited a
 * seam that already had the right joints.
 *
 * NOTHING HERE MAY BE COMPILED WITH A NAMESPACE WIDENING MACRO. That is the
 * rule that keeps this file portable, and it is enforced by tools/bsaudit.sh
 * and by tools/build.sh applying BS_SEAM_FLAGS to exactly one object. It has
 * one consequence worth knowing before reading further: under
 * -D_POSIX_C_SOURCE=200809L, glibc does NOT expose `d_type` in struct dirent.
 * See sys_dir_next.
 */
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
/* For renameat, which POSIX puts in <stdio.h> and nowhere else. The header is
 * here for one declaration; the audit tier watches the SYMBOLS this unit
 * ends up referencing, so "no stdio on the ABI path" stays a measurement
 * rather than a promise about which headers were included. */
#include <stdio.h>
#include <poll.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "sys.h"

/* Captured once, at sys_init, before the program exists. See sys.h. */
static bs_time mono_origin;
static int     mono_ready;
static unsigned long self_uid, self_gid;

/* The ONLY reader of errno outside the two platform files, and it is here
 * rather than there because the mapping is identical on both -- the numbers
 * differ, the names do not, and this table is written in names. */
bs_err sys_errmap(int e) {
    switch (e) {
    case 0:        return BS_OK;
    case EINTR:    return BS_INTR;
    case EAGAIN:   return BS_AGAIN;
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK: return BS_AGAIN;
#endif
    case EBADF:    return BS_BADF;
    case EACCES:   return BS_DENIED;
    case EPERM:    return BS_DENIED;
    case ENOENT:   return BS_NOENT;
    case EINVAL:   return BS_INVAL;
    case EPIPE:    return BS_PIPE;
    case EEXIST:   return BS_EXIST;
    case ENOTDIR:  return BS_NOTDIR;
    case EISDIR:   return BS_ISDIR;
    case ENOSPC:   return BS_NOSPC;
    case ENOMEM:   return BS_EXHAUSTED;
    case EMFILE:   return BS_EXHAUSTED;
    case ENFILE:   return BS_EXHAUSTED;
    case ENOSYS:   return BS_NOSYS;
    case ENOTEMPTY:return BS_NOTEMPTY;
    case ELOOP:    return BS_LOOP;
    case ENAMETOOLONG: return BS_NAMETOOLONG;
    case ESPIPE:   return BS_SPIPE;
    case EXDEV:    return BS_XDEV;
    case EROFS:    return BS_ROFS;
    case EOVERFLOW:return BS_OVERFLOW;
    case EBUSY:    return BS_BUSY;
    case ENOTSUP:  return BS_NOTSUP;
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
    case EOPNOTSUPP: return BS_NOTSUP;
#endif
    case EFBIG:    return BS_NOSPC;
    case EDQUOT:   return BS_NOSPC;
    /* The net family. These were missing until M5 and ECONNREFUSED arrived on
     * the wire as IO -- "an errno that escaped the map", which is the exact
     * phrase CONVENTIONS uses for what the platform parity tier is for. It
     * was a net fixture that found it, on the first run, which is the
     * argument for writing the refusal fixture at the same time as the op. */
    case ECONNREFUSED:  return BS_CONNREFUSED;
    case ECONNRESET:    return BS_CONNRESET;
    case ECONNABORTED:  return BS_CONNRESET;
    case EADDRINUSE:    return BS_ADDRINUSE;
    case EADDRNOTAVAIL: return BS_ADDRNOTAVAIL;
    case ENETUNREACH:   return BS_NETUNREACH;
    case EHOSTUNREACH:  return BS_NETUNREACH;
    case ENETDOWN:      return BS_NETUNREACH;
    case ENOTCONN:      return BS_NOTCONN;
    case EISCONN:       return BS_ISCONN;
    case EMSGSIZE:      return BS_MSGSIZE;
    case ETIMEDOUT:     return BS_TIMEDOUT;
    case EAFNOSUPPORT:  return BS_NOTSUP;
    case EPROTONOSUPPORT: return BS_NOTSUP;
    case EDESTADDRREQ:  return BS_NOTCONN;
    case EALREADY:      return BS_AGAIN;
    case EINPROGRESS:   return BS_AGAIN;
#ifdef ENOTCAPABLE
    /* FreeBSD only, and it arrives from a capability restricted descriptor.
     * Mapped here rather than left to fall through to IO because M8 turns
     * cap_enter() on and this becomes a common answer overnight. */
    case ENOTCAPABLE: return BS_DENIED;
#endif
    default:       return BS_IO;
    }
}

/* Descriptors cross the seam as bs_osfd and are int only in here.
 *
 * This is also where BS_OSFD_CWD becomes AT_FDCWD. That number is -100 on
 * Linux and -2 on FreeBSD, so it is exactly the kind of value CONVENTIONS
 * section 3 forbids above the seam, and exactly the kind of one-line mapping
 * the seam exists to hold. */
static int fdof(bs_osfd f) {
    if (f == BS_OSFD_CWD) return AT_FDCWD;
    return (int)f;
}

/* ---- clocks ------------------------------------------------------------- */

static bs_err now_raw(int monotonic, bs_time *out) {
    struct timespec ts;
    int r = clock_gettime(monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts);
    if (r != 0) return sys_errmap(errno);
    out->sec  = (bs_i64)ts.tv_sec;
    out->nsec = (bs_u32)ts.tv_nsec;
    return BS_OK;
}

void sys_init(void) {
    if (now_raw(1, &mono_origin) != BS_OK) { mono_origin.sec = 0; mono_origin.nsec = 0; }
    mono_ready = 1;

    /* Captured once, HERE, and not on first use.
     *
     * stat reports readable/writable/executable as advisory bytes computed
     * from the mode and the broker's own identity with no extra syscall
     * (ABI.md section 7.19). Asking the kernel who we are on the first stat
     * instead would put a one-time lazy initialisation inside the window the
     * syscall tier measures and the M8 filter will cover -- which is exactly
     * the defect M3 found in FreeBSD's arc4random and recorded in HANDOFF.
     * Finding it once is education; shipping it twice would be a habit. */
    self_uid = (unsigned long)geteuid();
    self_gid = (unsigned long)getegid();
}

bs_err sys_clock_real(bs_time *out) { return now_raw(0, out); }

bs_err sys_clock_mono(bs_time *out) {
    /* Initialised because the compiler cannot see that now_raw fills both
     * fields whenever it returns BS_OK, and -Wmaybe-uninitialized is not a
     * warning worth teaching anyone to ignore. */
    bs_time t = { 0, 0 };
    bs_err e;
    if (!mono_ready) sys_init();
    e = now_raw(1, &t);
    if (e != BS_OK) return e;
    /* borrow a second when the nanoseconds would go negative; doing this in
     * the seam rather than in the op is what keeps the op free of arithmetic
     * that could differ per platform */
    if (t.nsec < mono_origin.nsec) {
        t.sec -= 1;
        t.nsec += 1000000000u;
    }
    out->sec  = t.sec - mono_origin.sec;
    out->nsec = t.nsec - mono_origin.nsec;
    return BS_OK;
}

/* ---- open and close ----------------------------------------------------- */

/* brainstem's open flags to the platform's. This function is the reason
 * CONVENTIONS section 3 can say no value chosen by an OS header appears in a
 * frame: O_CREAT is 0x0200 on FreeBSD and 0x0040 on Linux, and neither number
 * exists anywhere above this line. */
static int oflags_of(bs_u32 f) {
    int o;
    if ((f & (BS_O_READ | BS_O_WRITE)) == (BS_O_READ | BS_O_WRITE)) o = O_RDWR;
    else if (f & BS_O_WRITE)                                        o = O_WRONLY;
    else                                                            o = O_RDONLY;

    if (f & BS_O_CREATE)    o |= O_CREAT;
    if (f & BS_O_EXCL)      o |= O_EXCL;
    if (f & BS_O_TRUNC)     o |= O_TRUNC;
    if (f & BS_O_APPEND)    o |= O_APPEND;
    if (f & BS_O_DIRECTORY) o |= O_DIRECTORY;
    if (f & BS_O_NOFOLLOW)  o |= O_NOFOLLOW;
    if (f & BS_O_NOWAIT)    o |= O_NONBLOCK;

    /* Always. Nothing this broker opens has any business surviving into a
     * spawned process, and M6 is where that would otherwise become a leak
     * nobody could see -- an fd inherited by accident is inherited silently.
     * spawn will hand over exactly what it is told to and nothing else. */
    o |= O_CLOEXEC;
    return o;
}

bs_err sys_open(bs_osfd dir, const char *path, bs_u32 oflags, bs_u32 mode, bs_osfd *out) {
    int fd;
    do {
        fd = openat(fdof(dir), path, oflags_of(oflags), (mode_t)(mode & 0777));
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return sys_errmap(errno);
    *out = (bs_osfd)fd;
    return BS_OK;
}

bs_err sys_close(bs_osfd fd) {
    /* No EINTR retry, deliberately. On Linux the descriptor is released even
     * when close returns EINTR, so retrying can close a descriptor some other
     * part of the program has since been given. The portable rule is to treat
     * a close as final whatever it says. */
    if (close(fdof(fd)) != 0 && errno != EINTR) return sys_errmap(errno);
    return BS_OK;
}

bs_err sys_dup(bs_osfd fd, bs_osfd *out) {
    int n = fcntl(fdof(fd), F_DUPFD_CLOEXEC, 0);
    if (n < 0) return sys_errmap(errno);
    *out = (bs_osfd)n;
    return BS_OK;
}

/* ---- read and write ----------------------------------------------------- */

/* Is this descriptor ready, right now?
 *
 * NOWAIT is implemented as a zero timeout poll rather than by toggling
 * O_NONBLOCK around the call. Toggling costs two fcntls per request, leaves
 * the descriptor in a different state if anything returns early, and would
 * make one op three syscalls -- the largest audit violation available. A
 * zero timeout poll costs one, and it is the same call on both platforms. */
static bs_err ready(int fd, int forwrite, int *is) {
    struct pollfd pf;
    int r;
    pf.fd = fd;
    pf.events = (short)(forwrite ? POLLOUT : POLLIN);
    pf.revents = 0;
    do { r = poll(&pf, 1, 0); } while (r < 0 && errno == EINTR);
    if (r < 0) return sys_errmap(errno);
    *is = r > 0;
    return BS_OK;
}

bs_err sys_read(bs_osfd fd, bs_u8 *buf, size_t n, int nowait, size_t *got) {
    ssize_t k;
    if (n == 0) { *got = 0; return BS_OK; }
    if (nowait) {
        int is = 0;
        bs_err e = ready(fdof(fd), 0, &is);
        if (e != BS_OK) return e;
        if (!is) return BS_AGAIN;
    }
    do { k = read(fdof(fd), buf, n); } while (k < 0 && errno == EINTR);
    if (k < 0) return sys_errmap(errno);
    *got = (size_t)k;                 /* zero means end of file; the op decides */
    return BS_OK;
}

bs_err sys_write(bs_osfd fd, const bs_u8 *buf, size_t n, int nowait, size_t *put) {
    size_t done = 0;
    *put = 0;
    if (n == 0) return BS_OK;

    if (nowait) {
        int is = 0;
        bs_err e = ready(fdof(fd), 1, &is);
        if (e != BS_OK) return e;
        if (!is) return BS_AGAIN;
        {
            ssize_t k;
            do { k = write(fdof(fd), buf, n); } while (k < 0 && errno == EINTR);
            if (k < 0) return sys_errmap(errno);
            *put = (size_t)k;
            return BS_OK;
        }
    }

    /* THE BROKER RETRIES SHORT WRITES, and ABI.md section 7.11 calls this the
     * single biggest kindness in the ABI. A partial write loop would make the
     * program subtract a 16 bit count, re-slice its buffer at a computed
     * offset and re-emit a header -- and re-slicing at an offset on a tape is
     * pointer travel proportional to that offset. The cost here is a loop; the
     * cost there is quadratic. */
    while (done < n) {
        ssize_t k = write(fdof(fd), buf + done, n - done);
        if (k < 0) {
            if (errno == EINTR) continue;
            return sys_errmap(errno);
        }
        if (k == 0) return BS_IO;
        done += (size_t)k;
    }
    *put = done;
    return BS_OK;
}

/* ---- seek --------------------------------------------------------------- */

bs_err sys_seek(bs_osfd fd, bs_u64 off, bs_u32 whence, bs_u64 *pos) {
    off_t r;
    int w;
    int negate = 0;

    /* The offset arrives as an unsigned magnitude and the direction arrives
     * as whence, because a signed 64 bit field would make every backwards
     * seek an eight limb two's complement on a brainfuck tape. The negation
     * costs the broker one instruction and the program nothing. */
    switch (whence) {
    case BS_SEEK_SET:      w = SEEK_SET; break;
    case BS_SEEK_CUR_FWD:  w = SEEK_CUR; break;
    case BS_SEEK_CUR_BACK: w = SEEK_CUR; negate = 1; break;
    case BS_SEEK_END_BACK: w = SEEK_END; negate = 1; break;
    case BS_SEEK_END_FWD:  w = SEEK_END; break;
    default: return BS_INVAL;
    }

    /* An off_t is signed, so a magnitude above its maximum has no
     * representation and is refused rather than wrapped into a seek to
     * somewhere surprising. */
    if (off > (bs_u64)0x7FFFFFFFFFFFFFFFULL) return BS_OVERFLOW;

    r = lseek(fdof(fd), negate ? -(off_t)off : (off_t)off, w);
    if (r == (off_t)-1) return sys_errmap(errno);
    *pos = (bs_u64)r;
    return BS_OK;
}

/* ---- stat --------------------------------------------------------------- */

static bs_u8 type_of(mode_t m) {
    if (S_ISREG(m))  return BS_FT_REG;
    if (S_ISDIR(m))  return BS_FT_DIR;
    if (S_ISLNK(m))  return BS_FT_LINK;
    if (S_ISFIFO(m)) return BS_FT_FIFO;
    if (S_ISSOCK(m)) return BS_FT_SOCK;
    if (S_ISCHR(m))  return BS_FT_CHR;
    if (S_ISBLK(m))  return BS_FT_BLK;
    return BS_FT_UNKNOWN;
}

/* The three advisory bytes, from the mode and the identity captured at
 * startup. Advisory because an ACL or a MAC label can still refuse the open
 * that follows; the authoritative answer is always to try it.
 *
 * `exec` is not just another bit, and the difference is the whole reason this
 * takes a flag rather than three identical calls. Root bypasses the read and
 * write checks entirely -- the mode is not the answer for uid 0 -- but root
 * CANNOT execute a file with no execute bit set anywhere. The first version
 * here returned 1 for all three under root and reported a 0644 file as
 * executable, which the container lane found immediately because it runs as
 * root and the development host does not. */
static bs_u8 permitted(const struct stat *st, unsigned int ubit,
                       unsigned int gbit, unsigned int obit, int exec) {
    if (self_uid == 0)
        return exec ? ((st->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) ? 1 : 0) : 1;
    if ((unsigned long)st->st_uid == self_uid) return (st->st_mode & ubit) ? 1 : 0;
    if ((unsigned long)st->st_gid == self_gid) return (st->st_mode & gbit) ? 1 : 0;
    return (st->st_mode & obit) ? 1 : 0;
}

static void fill_stat(const struct stat *st, bs_stat *out) {
    out->type       = type_of(st->st_mode);
    out->readable   = permitted(st, S_IRUSR, S_IRGRP, S_IROTH, 0);
    out->writable   = permitted(st, S_IWUSR, S_IWGRP, S_IWOTH, 0);
    out->executable = permitted(st, S_IXUSR, S_IXGRP, S_IXOTH, 1);
    out->size       = (bs_u64)st->st_size;
    out->mtime_sec  = (bs_u64)st->st_mtime;
    out->mtime_nsec = (bs_u32)st->st_mtim.tv_nsec;
    out->mode       = (bs_u16)(st->st_mode & 07777);
}

bs_err sys_stat(bs_osfd dir, const char *path, int nofollow, bs_stat *out) {
    struct stat st;
    int r;
    if (!path || !*path) {
        do { r = fstat(fdof(dir), &st); } while (r < 0 && errno == EINTR);
    } else {
        do {
            r = fstatat(fdof(dir), path, &st, nofollow ? AT_SYMLINK_NOFOLLOW : 0);
        } while (r < 0 && errno == EINTR);
    }
    if (r != 0) return sys_errmap(errno);
    fill_stat(&st, out);
    return BS_OK;
}

/* ---- the rest of fs ----------------------------------------------------- */

bs_err sys_mkdir(bs_osfd dir, const char *path, bs_u32 mode) {
    if (mkdirat(fdof(dir), path, (mode_t)(mode & 0777)) != 0) return sys_errmap(errno);
    return BS_OK;
}

bs_err sys_unlink(bs_osfd dir, const char *path, int removedir) {
    if (unlinkat(fdof(dir), path, removedir ? AT_REMOVEDIR : 0) != 0) return sys_errmap(errno);
    return BS_OK;
}

bs_err sys_rename(bs_osfd olddir, const char *oldpath, bs_osfd newdir, const char *newpath) {
    if (renameat(fdof(olddir), oldpath, fdof(newdir), newpath) != 0) return sys_errmap(errno);
    return BS_OK;
}

/* ---- directories -------------------------------------------------------- */

bs_err sys_dir_open(bs_osfd fd, bs_osdir *out) {
    DIR *d;
    int dup2fd = fcntl(fdof(fd), F_DUPFD_CLOEXEC, 0);
    /* fdopendir TAKES OWNERSHIP, and closedir will close what it was given.
     * The caller still owns its own descriptor and is entitled to stat and
     * openat through it afterwards, so this gets a copy. Getting that wrong
     * gives a handle whose fd dies when enumeration ends, which presents as
     * the NEXT op on that handle failing for no visible reason. */
    if (dup2fd < 0) return sys_errmap(errno);
    d = fdopendir(dup2fd);
    if (!d) { int e = errno; close(dup2fd); return sys_errmap(e); }
    *out = (bs_osdir)d;
    return BS_OK;
}

bs_err sys_dir_next(bs_osdir dir, bs_osfd dirfd, bs_u8 *type, char *name,
                    size_t cap, size_t *namelen, int *end) {
    DIR *d = (DIR *)dir;
    struct dirent *e;
    size_t len;

    *end = 0;
    for (;;) {
        errno = 0;
        e = readdir(d);
        if (!e) {
            if (errno != 0) return sys_errmap(errno);
            *end = 1;
            return BS_OK;
        }
        /* "." and ".." are filtered HERE rather than in the op, even though
         * the rule is an ABI rule, because the alternative is to stat an
         * entry the ABI has already decided the program will never see. The
         * parity tier watches the trace and cannot tell the difference; the
         * syscall tier can, and would count two pointless fstatats per
         * directory. */
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
            continue;
        break;
    }

    len = strlen(e->d_name);
    if (len == 0 || len >= cap) return BS_NAMETOOLONG;
    memcpy(name, e->d_name, len);
    name[len] = '\0';
    *namelen = len;

    /* THE TYPE ALWAYS COSTS A STAT, and that is a deliberate departure from
     * one request one syscall, recorded in ABI.md section 7.20.
     *
     * The obvious implementation reads d_type and stats only when the kernel
     * says DT_UNKNOWN. d_type is not POSIX: glibc hides it unless _DEFAULT_
     * SOURCE is defined, and this file may not be compiled with a namespace
     * widening macro. Reaching for it would have meant either moving readdir
     * into both platform files or widening the namespace of the portable
     * half, to save a syscall on an op that already costs a round trip
     * through a brainfuck interpreter.
     *
     * Statting unconditionally also makes the two platforms identical by
     * construction rather than by agreement: neither kernel is obliged to
     * fill d_type in at all, so a version that trusted it would produce
     * different bytes on different filesystems of the SAME platform. */
    {
        bs_stat st;
        bs_err r = sys_stat(dirfd, name, 1, &st);
        if (r != BS_OK) {
            /* The entry was there a moment ago and is not now, or cannot be
             * statted. Report it as unknown rather than failing the whole
             * enumeration: a directory being modified underneath a walk is
             * ordinary, and the alternative is an op that fails for a reason
             * the program did not cause. */
            *type = BS_FT_UNKNOWN;
            return BS_OK;
        }
        *type = st.type;
    }
    return BS_OK;
}

bs_err sys_dir_rewind(bs_osdir dir) {
    rewinddir((DIR *)dir);
    return BS_OK;
}

bs_err sys_dir_close(bs_osdir dir) {
    if (closedir((DIR *)dir) != 0) return sys_errmap(errno);
    return BS_OK;
}

/* ---- poll --------------------------------------------------------------- */

bs_err sys_poll(bs_pollfd *fds, size_t n, bs_u32 timeout_ms, size_t *nready) {
    struct pollfd pf[BS_POLL_MAX];
    size_t i;
    int r, t;

    if (n > BS_POLL_MAX) return BS_INVAL;
    *nready = 0;
    if (n == 0) return BS_OK;

    for (i = 0; i < n; i++) {
        pf[i].fd = fdof(fds[i].fd);
        pf[i].events = (short)((fds[i].want_read ? POLLIN : 0) | (fds[i].want_write ? POLLOUT : 0));
        pf[i].revents = 0;
    }

    /* 0xFFFFFFFF is "forever", which poll spells -1. Every other value is
     * clamped to what an int can hold rather than wrapping into a short wait,
     * because a program asking for a long timeout and getting a brief one is
     * a bug that looks like a race. */
    if (timeout_ms == 0xFFFFFFFFUL) t = -1;
    else if (timeout_ms > 0x7FFFFFFFUL) t = 0x7FFFFFFF;
    else t = (int)timeout_ms;

    do { r = poll(pf, (nfds_t)n, t); } while (r < 0 && errno == EINTR);
    if (r < 0) return sys_errmap(errno);

    for (i = 0; i < n; i++) {
        /* Each byte is exactly 0 or 1. A brainfuck program tests a byte by
         * decrementing it, so anything else here would cost it a comparison
         * it has no instruction for. */
        fds[i].readable = (pf[i].revents & POLLIN)  ? 1 : 0;
        fds[i].writable = (pf[i].revents & POLLOUT) ? 1 : 0;
        fds[i].hup      = (pf[i].revents & POLLHUP) ? 1 : 0;
        fds[i].err      = (pf[i].revents & (POLLERR | POLLNVAL)) ? 1 : 0;
    }
    *nready = (size_t)r;
    return BS_OK;
}

bs_err sys_fd_mode(bs_osfd fd, bs_u8 *readable, bs_u8 *writable) {
    int f = fcntl(fdof(fd), F_GETFL, 0);
    int acc;
    if (f < 0) return sys_errmap(errno);
    acc = f & O_ACCMODE;
    *readable = (acc == O_RDONLY || acc == O_RDWR) ? 1 : 0;
    *writable = (acc == O_WRONLY || acc == O_RDWR) ? 1 : 0;
    return BS_OK;
}
