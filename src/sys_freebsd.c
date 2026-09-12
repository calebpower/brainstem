/* sys_freebsd.c — the FreeBSD half of the seam.
 *
 * One of only two files permitted to be compiled with a namespace widening
 * macro. It does not read errno at all, and there is a reason worth naming:
 * arc4random_buf cannot fail, so this half of the seam has no error to map.
 * tools/bsaudit.sh sees that in the object rather than taking it on trust.
 */
#include <stddef.h>
#include <stdlib.h>

#include "sys.h"

bs_u8 sys_platform(void) { return 1; }

/* arc4random_buf(3) rather than getrandom(2), and this is the divergence the
 * seam was shaped around.
 *
 * It is libc rather than a syscall, and it CANNOT FAIL -- there is no error
 * return to check, which is why this function has no loop and no errmap call
 * while its Linux twin has both. FreeBSD has had getrandom since 12, but
 * arc4random_buf is the interface the platform actually documents for this,
 * it needs no ENOSYS fallback, and the stable contract here is libc rather
 * than the syscall table.
 *
 * The per-op syscall tier therefore expects a DIFFERENT multiset on this
 * platform, which is expected and is why tests/syscalls/ is per platform. */
bs_err sys_random(bs_u8 *buf, size_t n) {
    if (n == 0) return BS_OK;
    arc4random_buf(buf, n);
    return BS_OK;
}

/* M8. Frozen signature, documented no-op. Capsicum's cap_enter() goes here,
 * and it is the sharper of the two lockdowns: after it there is no open() by
 * path at all, which forces the preopen model onto fs.open. */
bs_err sys_lockdown(void) { return BS_OK; }
