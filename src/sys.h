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

/* Called ONCE at startup, before the child exists and before anything reads
 * the monotonic clock.
 *
 * It also captures the broker's effective identity, which stat needs for its
 * advisory permission bytes. Anything the seam would otherwise work out
 * lazily on first use belongs here: a one-time initialisation on the ABI path
 * lands inside the window the syscall tier measures and the M8 filter will
 * cover, which is precisely the defect M3 found in FreeBSD's arc4random. */
void sys_init(void);

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

/* ---- descriptors and directories --------------------------------------- */

/* AN OS DESCRIPTOR, AND NOT AN int.
 *
 * A POSIX fd is an int, a Windows SOCKET is a UINT_PTR, and a Windows HANDLE
 * is a pointer. Writing `int` here would be free today and would be the one
 * thing that made sys_win32.c a rewrite rather than an implementation -- the
 * signatures below are the whole reason the seam exists, so the descriptor
 * type is the last place to assume.
 *
 * bs_i64 is wide enough for all three and signed, so BS_OSFD_NONE can be -1.
 * NOTHING ABOVE THE SEAM MAY INTERPRET THIS VALUE: the handle table stores it
 * and hands it back, and that is all. It is deliberately not the number a
 * program sees -- see ABI.md section 5 for why a handle is generation tagged
 * instead. */
typedef bs_i64 bs_osfd;
#define BS_OSFD_NONE ((bs_osfd)-1)

/* The broker's own working directory, as a descriptor.
 *
 * POSIX spells this AT_FDCWD, which is -100 on Linux and -2 on FreeBSD --
 * a number chosen by an OS header, so it may not appear above the seam any
 * more than AF_INET6 may. This is brainstem's spelling and sys_posix.c maps
 * it, in the one function that turns a bs_osfd into an int. */
#define BS_OSFD_CWD  ((bs_osfd)-3)

/* An open directory, for readdir. Opaque above the seam: only sys_*.c knows
 * this is a DIR *, because DIR is a POSIX type and may not be named in this
 * header. */
typedef void *bs_osdir;

/* ---- brainstem's own constants, which are the ones on the wire ---------- */
/* None of these is an OS number. AF_INET6 is 28 on FreeBSD and 10 on Linux;
 * O_CREAT is 0x0200 and 0x0040. A value chosen by an OS header may not appear
 * in a frame (CONVENTIONS section 3), so the seam maps these and they die
 * there. ABI.md section 7.17 is the normative list. */

/* open flags */
#define BS_O_READ       0x0001
#define BS_O_WRITE      0x0002
#define BS_O_CREATE     0x0004
#define BS_O_EXCL       0x0008
#define BS_O_TRUNC      0x0010
#define BS_O_APPEND     0x0020
#define BS_O_DIRECTORY  0x0040
#define BS_O_NOFOLLOW   0x0080
#define BS_O_NOWAIT     0x0100
#define BS_O_KNOWN      0x01FF

/* seek whence. There is no signed field anywhere in this ABI and seek is why
 * the rule survives: the sign lives here, as a different one-byte constant,
 * so the offset on the wire is an unsigned magnitude. */
#define BS_SEEK_SET      0
#define BS_SEEK_CUR_FWD  1
#define BS_SEEK_CUR_BACK 2
#define BS_SEEK_END_BACK 3
#define BS_SEEK_END_FWD  4

/* file kinds, shared by stat and readdir */
#define BS_FT_UNKNOWN 0
#define BS_FT_REG     1
#define BS_FT_DIR     2
#define BS_FT_LINK    3
#define BS_FT_FIFO    4
#define BS_FT_SOCK    5
#define BS_FT_CHR     6
#define BS_FT_BLK     7

/* The stat record, ABI.md section 7.19, in the order it goes on the wire.
 * Note what is not here: dev, ino, uid, gid, nlink, blocks, atime, ctime and
 * birthtime. A program asks four things about a path -- do I read it or
 * enumerate it, how big a loop do I need, has it changed, is it worth
 * trying -- and none of those answers any of them. */
typedef struct {
    bs_u8  type;
    bs_u8  readable;      /* advisory: computed from the mode with NO extra
                           * syscall, because faccessat three times would turn
                           * one request into four. The authoritative answer
                           * is to try the open. */
    bs_u8  writable;
    bs_u8  executable;
    bs_u64 size;
    bs_u64 mtime_sec;
    bs_u32 mtime_nsec;
    bs_u16 mode;
} bs_stat;

/* One descriptor's worth of poll, in and out together. Four separate bytes
 * rather than a revents bitmask, because brainfuck has no bitwise instruction
 * and a bit decomposition per descriptor per poll is the one cost this ABI
 * refuses to impose. */
typedef struct {
    bs_osfd fd;
    bs_u8   want_read;
    bs_u8   want_write;
    bs_u8   readable;
    bs_u8   writable;
    bs_u8   hup;
    bs_u8   err;
} bs_pollfd;

/* ---- sockets ------------------------------------------------------------ */
/* brainstem's own numbers again, and here the divergence is at its worst:
 * AF_INET6 is 28 on FreeBSD and 10 on Linux, SOCK_DGRAM is 2 on both but
 * SOCK_STREAM's neighbours are not, and FreeBSD's sockaddr_in carries a
 * leading length byte that Linux's does not. None of that reaches a frame. */
#define BS_AF_NONE   0
#define BS_AF_INET   1
#define BS_AF_INET6  2
#define BS_AF_UNIX   3

#define BS_SOCK_STREAM 1
#define BS_SOCK_DGRAM  2

/* The address record, ABI.md section 6, parsed. Thirty two bytes on the wire
 * for EVERY family, with up to twenty four zero padding bytes, because a zero
 * byte is free to emit in brainfuck -- '.' on a cell never touched -- and the
 * padding buys a constant offset and a constant length in both directions.
 * This is the clearest case in the whole design where the brainfuck cost
 * model inverts an ordinary engineering instinct.
 *
 * PORT IS HOST ORDER, little-endian like every other integer here, and the
 * broker byte-swaps. The ADDRESS is in reading order, because it is a byte
 * string rather than an integer: 192.168.1.10 has no endianness and writing
 * it backwards would be a legibility trap. Those two look inconsistent and
 * are not. */
typedef struct {
    bs_u8  family;
    bs_u16 port;
    bs_u8  body[28];
} bs_addr;

/* Sockets are created BLOCKING and close-on-exec. Blocking is a per-call flag
 * and never socket state, which is why there is no fcntl op and no hidden
 * mode a program could get stuck in. */
bs_err sys_socket(bs_u32 domain, bs_u32 type, bs_osfd *out);

/* NON-BLOCKING CONNECT, without a getsockopt op existing.
 *
 * *state is 0 for a fresh attempt and 1 while one is in flight; the seam
 * reads it and writes it back, so the state lives on the handle where the
 * rest of a socket's identity lives, and the mechanism lives here. The
 * program's side of this is: connect with NOWAIT, get AGAIN, poll for
 * writable, and RE-ISSUE THE IDENTICAL CONNECT. The broker recognises the
 * in-progress state and reports the result, which is what lets the ABI have
 * no getsockopt at all. */
bs_err sys_connect(bs_osfd fd, const bs_addr *a, int nowait, int *state);

/* Replies with the address ACTUALLY bound, which is what makes port 0 usable:
 * there is no getsockname op, so without this an ephemeral port could never
 * be discovered by the program that asked for one. */
bs_err sys_bind(bs_osfd fd, const bs_addr *a, int reuseaddr, bs_addr *bound);

bs_err sys_listen(bs_osfd fd, bs_u32 backlog);
bs_err sys_accept(bs_osfd fd, int nowait, bs_osfd *out, bs_addr *peer);

/* Family 3 Unix is DECLARED BY THE ABI AND NOT BUILT, and the reason is a
 * platform divergence rather than a shortage of time: every path in this ABI
 * resolves beneath a preopened directory handle, FreeBSD has bindat(2) and
 * connectat(2) which take exactly that, and Linux has neither. The
 * workarounds are respectively racy and Linux-only. An op that needed a
 * different mechanism on the two platforms is the thing this project will
 * not ship, so family 3 answers NOTSUP on both. */
bs_u8 sys_net_family_supported(bs_u32 family);

/* ---- processes ---------------------------------------------------------- */

/* SIGNAL NUMBERS ARE BRAINSTEM'S OWN. SIGUSR1 is 30 on FreeBSD and 10 on
 * Linux, SIGBUS is 10 and 7. That several of these happen to match Linux is a
 * coincidence of history and is NOT the contract -- the mapping lives at the
 * seam and a program may only ever read these names. */
#define BS_SIG_UNKNOWN 0
#define BS_SIG_HUP     1
#define BS_SIG_INT     2
#define BS_SIG_QUIT    3
#define BS_SIG_ILL     4
#define BS_SIG_TRAP    5
#define BS_SIG_ABRT    6
#define BS_SIG_BUS     7
#define BS_SIG_FPE     8
#define BS_SIG_KILL    9
#define BS_SIG_USR1   10
#define BS_SIG_SEGV   11
#define BS_SIG_USR2   12
#define BS_SIG_PIPE   13
#define BS_SIG_ALRM   14
#define BS_SIG_TERM   15

/* wait's state byte */
#define BS_PS_RUNNING  0
#define BS_PS_EXITED   1
#define BS_PS_SIGNALLED 2

/* One entry of spawn's descriptor map: the child sees child_fd, the broker
 * hands over fd. ANY CHILD DESCRIPTOR NOT NAMED IN THE MAP IS CLOSED, which
 * is what makes inheritance auditable rather than accidental -- everything
 * this broker opens is close-on-exec, so the map is not merely the intended
 * set, it is the whole set. */
typedef struct {
    bs_u8   child_fd;
    bs_osfd fd;
} bs_fdmap;

#define BS_SPAWN_MAXFD 16
#define BS_SPAWN_MAXV  64

bs_err sys_pipe(bs_osfd *rd, bs_osfd *wr);

/* argv and envp are NULL terminated arrays, as execve wants them. path is
 * relative to dir, like every other path in this ABI.
 *
 * *pid is opaque above the seam and never reaches the wire: pids differ
 * between runs and between platforms, and the wire must not. The program gets
 * a process HANDLE out of the same table every other handle comes from. */
bs_err sys_spawn(bs_osfd dir, const char *path,
                 char *const *argv, char *const *envp,
                 const bs_fdmap *map, size_t nmap, bs_i64 *pid);

/* nowait asks whether it has finished rather than waiting for it to. */
bs_err sys_wait(bs_i64 pid, int nowait, bs_u8 *state, bs_u8 *code, bs_u8 *sig);

/* ---- the filesystem half of the seam ------------------------------------
 *
 * Every path is RELATIVE TO A DIRECTORY DESCRIPTOR. There is no call here
 * that takes a bare path, which is what makes "nothing is reachable that was
 * not named on the command line" a property of the interface rather than of
 * anyone remembering to check. See ABI.md section 8 for what that does and
 * does not claim.
 */

/* oflags is a BS_O_* set, not a platform one. mode is the low nine
 * permission bits, which is the single place a raw POSIX number crosses the
 * seam -- those bits are identical on both platforms and are what a human
 * writes as 0644. */
bs_err sys_open(bs_osfd dir, const char *path, bs_u32 oflags, bs_u32 mode, bs_osfd *out);

/* THE ONE CALL THAT TAKES A BARE PATH, and it exists only for --preopen-*.
 * Those paths come from the command line, which is the broker's operator
 * speaking rather than the program, and they are resolved before the
 * interpreter is started at all. Nothing on the ABI path may call this. */
bs_err sys_open_host(const char *path, bs_u32 oflags, bs_osfd *out);
bs_err sys_close(bs_osfd fd);

/* nowait asks for a non-blocking attempt. got/put are set on BS_OK only.
 * sys_read sets *got to 0 at end of file; distinguishing that from "nothing
 * available right now" is the caller's job and the ABI gives them different
 * status codes for it. */
bs_err sys_read (bs_osfd fd, bs_u8 *buf, size_t n, int nowait, size_t *got);
bs_err sys_write(bs_osfd fd, const bs_u8 *buf, size_t n, int nowait, size_t *put);

/* whence is a BS_SEEK_* constant, off an unsigned magnitude. */
bs_err sys_seek(bs_osfd fd, bs_u64 off, bs_u32 whence, bs_u64 *pos);

/* A NULL or empty path stats the descriptor itself. */
bs_err sys_stat(bs_osfd dir, const char *path, int nofollow, bs_stat *out);

bs_err sys_mkdir (bs_osfd dir, const char *path, bs_u32 mode);
bs_err sys_unlink(bs_osfd dir, const char *path, int removedir);
bs_err sys_rename(bs_osfd olddir, const char *oldpath, bs_osfd newdir, const char *newpath);

/* Directory enumeration. sys_dir_open takes ownership of nothing: the caller
 * still owns fd and must not close it while the bs_osdir is live.
 *
 * sys_dir_next sets *end when the directory is exhausted. "." and ".." are
 * NOT filtered here -- that is the op's job, because it is an ABI rule rather
 * than a platform one, and a rule enforced at the seam is a rule the parity
 * tier cannot see. type is filled in with a stat when the kernel says
 * unknown, which both of them are entitled to do. */
bs_err sys_dir_open (bs_osfd fd, bs_osdir *out);
bs_err sys_dir_next (bs_osdir d, bs_osfd dirfd, bs_u8 *type, char *name, size_t cap, size_t *namelen, int *end);
bs_err sys_dir_rewind(bs_osdir d);
bs_err sys_dir_close (bs_osdir d);

/* nfds is capped so the array can live on the stack, with no allocation on
 * the ABI path. Sixty four is the ABI's own limit (section 7.13) and is set
 * here rather than in two places. */
#define BS_POLL_MAX 64

/* timeout_ms of 0xFFFFFFFF waits forever; 0 returns immediately. */
bs_err sys_poll(bs_pollfd *fds, size_t n, bs_u32 timeout_ms, size_t *nready);

/* Duplicate one of the broker's own descriptors, for --preopen-fd. Separate
 * from sys_open because there is no path involved and no directory to resolve
 * beneath, and because a Windows implementation of the two has nothing in
 * common. */
bs_err sys_dup(bs_osfd fd, bs_osfd *out);

/* Which directions this descriptor was opened for, for --preopen-fd. The
 * operator names a number and the broker works out what it is rather than
 * making them say, because a preopen whose declared rights disagree with the
 * descriptor fails much later with a status pointing somewhere else. */
bs_err sys_fd_mode(bs_osfd fd, bs_u8 *readable, bs_u8 *writable);

/* Does this build ask the KERNEL to keep a path beneath its directory, or
 * only the broker's own string check? 1 when the kernel is doing it.
 *
 * Reported rather than assumed because the answer differs: FreeBSD has
 * O_RESOLVE_BENEATH as a plain open flag, and Linux has the same property
 * only through openat2, which is a raw syscall and arrives with M8. The
 * platform parity tier asserts the OBSERVABLE behaviour is identical either
 * way -- a traversal is refused on both -- and this is how the suite knows
 * which mechanism refused it. */
bs_u8 sys_beneath_is_kernel(void);

/* Which platform this is, for the hello reply. brainstem's own numbering:
 * 1 FreeBSD, 2 Linux, 3 Windows, 0 other. Not a uname string -- a program
 * cannot usefully compare strings, and a byte it can branch on is worth more
 * than a name it cannot read. */
bs_u8 sys_platform(void);

#endif
