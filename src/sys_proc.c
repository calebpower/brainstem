/* sys_proc.c — pipe, spawn and wait: the half of the seam that makes one
 * program able to drive another.
 *
 * WHY NOT posix_spawn, WHICH THE PLAN CALLED FOR. It cannot resolve a path
 * beneath a directory handle -- there is no posix_spawnat -- and spawn takes
 * a directory handle like every other path op in this ABI. The obvious repair is
 * openat plus fexecve, and fexecve is the trap: FreeBSD implements it in the
 * kernel, and glibc implements it through /proc/self/fd, so on a Linux system
 * without /proc mounted it fails with ENOSYS. An op that worked on the
 * primary platform and depended on a filesystem being mounted on the
 * secondary one is exactly the divergence this project refuses.
 *
 * So: fork, fchdir to the directory handle, install the descriptor map, and
 * execve a relative path. The child is single threaded between fork and exec,
 * which is what makes fchdir safe there and unsafe anywhere else. The
 * inheritance story is not weaker than posix_spawn's, it is stronger --
 * everything this broker opens is close-on-exec, so the map is not merely the
 * intended set of descriptors, it is the whole set.
 */
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "sys.h"

bs_err sys_errmap(int e);

static int fdof(bs_osfd f) { return (int)f; }

bs_err sys_pipe(bs_osfd *rd, bs_osfd *wr) {
    int p[2];
    if (pipe(p) != 0) return sys_errmap(errno);
    /* Close-on-exec on both ends, like everything else this broker opens.
     * spawn's map re-establishes exactly what the child should have, and a
     * pipe end that leaked into an unrelated child is the classic reason a
     * reader never sees end of file. */
    if (fcntl(p[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(p[1], F_SETFD, FD_CLOEXEC) < 0) {
        int e = errno; close(p[0]); close(p[1]); return sys_errmap(e);
    }
    *rd = (bs_osfd)p[0];
    *wr = (bs_osfd)p[1];
    return BS_OK;
}

/* brainstem's signal numbers from the platform's. Only the ones a child is
 * realistically killed by; anything else is UNKNOWN rather than a number the
 * program would have to look up in a table that does not exist. */
static bs_u8 sig_of(int s) {
    switch (s) {
    case SIGHUP:  return BS_SIG_HUP;
    case SIGINT:  return BS_SIG_INT;
    case SIGQUIT: return BS_SIG_QUIT;
    case SIGILL:  return BS_SIG_ILL;
    case SIGTRAP: return BS_SIG_TRAP;
    case SIGABRT: return BS_SIG_ABRT;
    case SIGBUS:  return BS_SIG_BUS;
    case SIGFPE:  return BS_SIG_FPE;
    case SIGKILL: return BS_SIG_KILL;
    case SIGUSR1: return BS_SIG_USR1;
    case SIGSEGV: return BS_SIG_SEGV;
    case SIGUSR2: return BS_SIG_USR2;
    case SIGPIPE: return BS_SIG_PIPE;
    case SIGALRM: return BS_SIG_ALRM;
    case SIGTERM: return BS_SIG_TERM;
    default:      return BS_SIG_UNKNOWN;
    }
}

/* Install the descriptor map in the freshly forked child.
 *
 * THE COLLISION IS THE WHOLE PROBLEM. A map that says "child 0 gets my fd 5,
 * child 5 gets my fd 0" cannot be applied in either order directly: the first
 * dup2 destroys the second's source. So every source is first moved somewhere
 * above every target, and only then dup2'd down. Doing it in one pass and
 * hoping the numbers do not overlap works until the day it does not, and the
 * symptom would be a child reading from the wrong end of a pipe.
 *
 * Runs in the child after fork, so nothing here may allocate and nothing here
 * may fail softly: on any error the child exits rather than execing with a
 * descriptor table that is not what was asked for. */
static void apply_map(const bs_fdmap *map, size_t n) {
    int tmp[BS_SPAWN_MAXFD];
    size_t i;
    int high = 3, j;

    for (i = 0; i < n; i++) if ((int)map[i].child_fd >= high) high = (int)map[i].child_fd + 1;

    for (i = 0; i < n; i++) {
        tmp[i] = fcntl(fdof(map[i].fd), F_DUPFD, high + (int)i);
        if (tmp[i] < 0) _exit(126);
    }
    for (i = 0; i < n; i++) {
        if (dup2(tmp[i], (int)map[i].child_fd) < 0) _exit(126);
        close(tmp[i]);
    }

    /* Any child descriptor not named in the map is closed, per ABI.md section
     * 7.14 to 7.16. Everything this broker opened is close-on-exec, so only
     * the three the child would otherwise inherit from the BROKER's own
     * stdio need dealing with here. A child that kept the broker's stderr
     * would be writing into the operator's terminal unasked.
     *
     * THAT SENTENCE WAS FALSE UNTIL M7 and the failure was a deadlock rather
     * than a leak. child.c created the interpreter's two pipes with a plain
     * pipe(), so a grandchild inherited the write end of the interpreter's
     * stdin -- and then closing the broker's copy no longer gave the
     * interpreter end of input. See the note in child.c; the fix is there,
     * and this comment is only true because of it. */
    for (j = 0; j < 3; j++) {
        int named = 0;
        for (i = 0; i < n; i++) if ((int)map[i].child_fd == j) named = 1;
        if (!named) close(j);
    }
}

bs_err sys_spawn(bs_osfd dir, const char *path,
                 char *const *argv, char *const *envp,
                 const bs_fdmap *map, size_t nmap, bs_i64 *pid) {
    pid_t p;

    if (nmap > BS_SPAWN_MAXFD) return BS_INVAL;

    p = fork();
    if (p < 0) return sys_errmap(errno);
    if (p == 0) {
        /* THE CHILD. Single threaded, between fork and exec, which is the one
         * place fchdir is safe: nothing else in this process can observe the
         * working directory changing because there is nothing else in this
         * process. */
        /* BS_OSFD_CWD means "wherever the broker already is", so there is
         * nothing to change to. fchdir(AT_FDCWD) is not a thing. */
        if (dir != BS_OSFD_CWD && fchdir(fdof(dir)) != 0) _exit(126);
        apply_map(map, nmap);
        /* SIGPIPE is ignored in the broker (main.c) and ignoring is inherited
         * across exec, unlike a handler. A child that inherited "ignore"
         * would get EPIPE where it expected to die, which is a behaviour
         * difference the program did not ask for and cannot see. */
        signal(SIGPIPE, SIG_DFL);
        execve(path, argv, envp);
        _exit(127);
    }
    *pid = (bs_i64)p;
    return BS_OK;
}

bs_err sys_wait(bs_i64 pid, int nowait, bs_u8 *state, bs_u8 *code, bs_u8 *sig) {
    int st = 0;
    pid_t r;

    *state = BS_PS_RUNNING;
    *code = 0;
    *sig = 0;

    do { r = waitpid((pid_t)pid, &st, nowait ? WNOHANG : 0); } while (r < 0 && errno == EINTR);
    if (r < 0) {
        /* No such child. The commonest way to get here is waiting twice on
         * the same process, which the op layer answers from its cached status
         * instead -- ABI.md requires wait to be idempotent after reaping. So
         * reaching this really does mean the pid was never ours. */
        if (errno == ECHILD) return BS_NOCHILD;
        return sys_errmap(errno);
    }
    if (r == 0) return BS_OK;              /* still running; state stays 0 */

    if (WIFEXITED(st)) {
        *state = BS_PS_EXITED;
        *code  = (bs_u8)(WEXITSTATUS(st) & 0xFF);
    } else if (WIFSIGNALED(st)) {
        *state = BS_PS_SIGNALLED;
        *sig   = sig_of(WTERMSIG(st));
    } else {
        /* Stopped or continued. This broker does not trace children, so there
         * is nothing useful to report and nothing the program could do about
         * it; running is the honest answer. */
        *state = BS_PS_RUNNING;
    }
    return BS_OK;
}
