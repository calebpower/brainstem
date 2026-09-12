/* sys_posix.c — what both platforms genuinely share.
 *
 * The bulk of the seam lives here. sys_freebsd.c and sys_linux.c hold only
 * what actually differs, which at M3 is randomness and the platform byte --
 * and that is the point of starting the seam with time and rand rather than
 * with something larger. The shape comes from a real divergence
 * (arc4random_buf against getrandom) rather than from a guess about what
 * might diverge later.
 */
#include <errno.h>
#include <time.h>

#include "sys.h"

/* The monotonic origin, captured once. See sys.h for why this is normalised
 * rather than passed through. */
static bs_time mono_origin;
static int     mono_ready;

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
    default:       return BS_IO;
    }
}

static bs_err now_raw(int monotonic, bs_time *out) {
    struct timespec ts;
    int r = clock_gettime(monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts);
    if (r != 0) return sys_errmap(errno);
    out->sec  = (bs_i64)ts.tv_sec;
    out->nsec = (bs_u32)ts.tv_nsec;
    return BS_OK;
}

void sys_clock_init(void) {
    if (now_raw(1, &mono_origin) != BS_OK) { mono_origin.sec = 0; mono_origin.nsec = 0; }
    mono_ready = 1;
}

bs_err sys_clock_real(bs_time *out) { return now_raw(0, out); }

bs_err sys_clock_mono(bs_time *out) {
    /* Initialised because the compiler cannot see that now_raw fills both
     * fields whenever it returns BS_OK, and -Wmaybe-uninitialized is not a
     * warning worth teaching anyone to ignore. */
    bs_time t = { 0, 0 };
    bs_err e;
    if (!mono_ready) sys_clock_init();
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
