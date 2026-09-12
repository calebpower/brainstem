/* sys.h — THE PLATFORM SEAM.
 *
 * NO POSIX TYPE APPEARS BELOW THIS LINE. No struct stat, no sockaddr, no
 * mode_t, no pid_t, no time_t, no errno. Only fixed width types and
 * brainstem's own records.
 *
 * That single rule does three jobs, and it is worth naming all three because
 * the rule looks like fussiness until you see what it buys:
 *
 *   1. The wire stays identical across platforms. An op handler cannot leak a
 *      platform value into a frame when the seam never hands it one. AF_INET6
 *      is 28 on FreeBSD and 10 on Linux; O_CREAT is 0x0200 and 0x0040; errno
 *      11 is EAGAIN on one and EDEADLK on the other. None of those can reach
 *      a frame through this header, because none of them can be named in it.
 *
 *   2. The extension surface is confined. Only sys_freebsd.c and sys_linux.c
 *      may be compiled with a namespace widening macro, and only those two
 *      files may read errno. tools/bsaudit.sh checks both.
 *
 *   3. A sys_win32.c becomes an implementation rather than a refactor. The
 *      signatures below do not assume a descriptor is an int, a process is a
 *      pid, or a time is a timespec.
 *
 * Every function returns a bs_err, never -1. There is exactly one place errno
 * is read and exactly one table that maps it here.
 */
#ifndef BS_SYS_H
#define BS_SYS_H

#include <stddef.h>
#include "err.h"

/* Fixed width types, spelled out rather than pulled from stdint.h, so this
 * header depends on nothing at all. C99 guarantees these widths for the
 * platforms brainstem targets and the build would fail loudly elsewhere. */
typedef unsigned char      bs_u8;
typedef unsigned short     bs_u16;
typedef unsigned int       bs_u32;
typedef unsigned long long bs_u64;
typedef long long          bs_i64;

/* A moment. Seconds and nanoseconds are split so that neither side divides,
 * and a program wanting whole seconds reads eight bytes and ignores four. */
typedef struct {
    bs_i64 sec;
    bs_u32 nsec;
} bs_time;

/* ---- the seam ----------------------------------------------------------- */

/* Wall clock, Unix epoch seconds, UTC. There is no timezone concept in this
 * ABI and there will not be one. */
bs_err sys_clock_real(bs_time *out);

/* Monotonic, and NORMALISED TO ZERO AT BROKER START. That is a guarantee
 * rather than a passthrough: raw monotonic time is time since boot on both
 * platforms, and its absolute value is machine state that would make two
 * runs of the same program differ. Zeroing it also lets a program time an
 * interval using only the low four bytes. */
bs_err sys_clock_mono(bs_time *out);

/* Called once at startup, before anything reads the monotonic clock. */
void sys_clock_init(void);

/* Fill with randomness from the host. Never called when a seed is in force;
 * see det.h for that path, which issues no syscall at all. */
bs_err sys_random(bs_u8 *buf, size_t n);

/* Drop privilege to what the ABI actually needs.
 *
 * A DOCUMENTED NO-OP IN v1, with the signature frozen so M8 is an
 * implementation rather than a refactor. seccomp-notify on Linux,
 * cap_enter() on FreeBSD -- and Capsicum is the sharper of the two, because
 * after cap_enter() there is no open() by path at all, which forces the
 * preopen model onto fs.open. That is the "WASI for brainfuck" claim arrived
 * at because the primary platform insisted on it.
 *
 * Installed immediately before the main loop, so libc's own startup is out of
 * scope by construction rather than by anyone accounting for it. */
bs_err sys_lockdown(void);

/* Which platform this is, for the hello reply. brainstem's own numbering:
 * 1 FreeBSD, 2 Linux, 3 Windows, 0 other. Not a uname string -- a program
 * cannot usefully compare strings, and a byte it can branch on is worth more
 * than a name it cannot read. */
bs_u8 sys_platform(void);

#endif
