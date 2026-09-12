/* sys_linux.c — the Linux half of the seam.
 *
 * One of only two files permitted to be compiled with a namespace widening
 * macro, and one of only three permitted to read errno. tools/bsaudit.sh
 * checks both claims against the objects rather than trusting this comment.
 */
#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "sys.h"

bs_err sys_errmap(int e);

bs_u8 sys_platform(void) { return 2; }

/* getrandom(2). Called through syscall() rather than the glibc wrapper for
 * one specific reason: glibc's getrandom can fall back to opening
 * /dev/urandom on older kernels, which is a second code path and an open()
 * that the per-op syscall tier would see and could not explain. A direct
 * call either works or returns ENOSYS, and both are answers.
 *
 * This is the one place in the tree that names a syscall number, and it is
 * here rather than at a call site because that is what the seam is for. */
bs_err sys_random(bs_u8 *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        long r = syscall(SYS_getrandom, buf + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return sys_errmap(errno);
        }
        if (r == 0) return BS_IO;
        got += (size_t)r;
    }
    return BS_OK;
}

/* M8. Frozen signature, documented no-op. */
bs_err sys_lockdown(void) { return BS_OK; }
