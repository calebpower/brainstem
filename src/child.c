/* child.c — fork, wire two pipes, exec the interpreter.
 *
 * Plain fork and execvp rather than posix_spawn, for one reason: the fd
 * plumbing here has to be exact and visible. Every descriptor the child
 * should not have is closed by name in the child between fork and exec,
 * where it can be read off the page. posix_spawn's file_actions would do the
 * same thing through an API, and at M6 -- when spawn becomes an ABI op and
 * the program chooses the mapping -- that is the right trade. Here it is not
 * worth the indirection.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#include "child.h"

bs_err bs_child_start(struct bs_chan *ch, const char *interp, const char *prog) {
    int to_child[2], from_child[2];
    pid_t pid;

    ch->from_prog = ch->to_prog = -1;
    ch->pid = -1;

    if (pipe(to_child) != 0) {
        fprintf(stderr, "brainstem: pipe: %s\n", strerror(errno));
        return BS_IO;
    }
    if (pipe(from_child) != 0) {
        fprintf(stderr, "brainstem: pipe: %s\n", strerror(errno));
        close(to_child[0]); close(to_child[1]);
        return BS_IO;
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "brainstem: fork: %s\n", strerror(errno));
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        return BS_IO;
    }

    if (pid == 0) {
        /* the child: the program's stdin is our pipe, its stdout is ours,
         * and its stderr is passed through untouched so the interpreter can
         * still complain to the terminal. */
        if (dup2(to_child[0], 0) < 0 || dup2(from_child[1], 1) < 0) _exit(127);
        close(to_child[0]);  close(to_child[1]);
        close(from_child[0]); close(from_child[1]);

        execlp(interp, interp, prog, (char *)0);

        /* Only reachable when exec failed. Write the reason to stderr and
         * use 127, the shell's convention for "command not found", so the
         * parent can tell this apart from a program that ran and exited. */
        fprintf(stderr, "brainstem: cannot run interpreter '%s': %s\n",
                interp, strerror(errno));
        _exit(127);
    }

    /* the parent keeps one end of each */
    close(to_child[0]);
    close(from_child[1]);
    ch->to_prog   = to_child[1];
    ch->from_prog = from_child[0];
    ch->pid       = (long)pid;
    return BS_OK;
}

bs_err bs_child_finish(struct bs_chan *ch, int *exited, int *code) {
    int st = 0;
    pid_t r;

    /* Closing the program's stdin is what lets a program blocked on ',' see
     * end of input and finish, rather than waiting for a reply that is never
     * coming. Without this the wait below would hang for exactly the
     * programs that most need to be cleaned up after. */
    if (ch->to_prog >= 0) { close(ch->to_prog); ch->to_prog = -1; }

    do { r = waitpid((pid_t)ch->pid, &st, 0); } while (r < 0 && errno == EINTR);

    if (ch->from_prog >= 0) { close(ch->from_prog); ch->from_prog = -1; }

    if (r < 0) { *exited = 1; *code = 127; return BS_IO; }

    if (WIFEXITED(st))        { *exited = 1; *code = WEXITSTATUS(st); }
    else if (WIFSIGNALED(st)) { *exited = 0; *code = WTERMSIG(st); }
    else                      { *exited = 1; *code = 127; }
    return BS_OK;
}

void bs_child_kill(struct bs_chan *ch) {
    int st;
    if (ch->pid <= 0) return;
    if (ch->to_prog   >= 0) { close(ch->to_prog);   ch->to_prog   = -1; }
    if (ch->from_prog >= 0) { close(ch->from_prog); ch->from_prog = -1; }
    kill((pid_t)ch->pid, SIGKILL);
    while (waitpid((pid_t)ch->pid, &st, 0) < 0 && errno == EINTR) { }
    ch->pid = -1;
}
