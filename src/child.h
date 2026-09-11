/* child.h — starting the interpreter and holding the two pipes to it.
 *
 * brainstem does not interpret brainfuck. It spawns an interpreter of the
 * caller's choosing and speaks the protocol through the program's own ',' and
 * '.'. That indirection is the whole reason the portability claim is
 * checkable: the same file runs unmodified under a bare interpreter with no
 * broker, where it reads end of input and does nothing.
 */
#ifndef BS_CHILD_H
#define BS_CHILD_H

#include "brainstem.h"

/* Start INTERP with PROG as its argument, wired to two pipes. Returns BS_OK,
 * or a status with a diagnosis already on stderr. */
bs_err bs_child_start(struct bs_chan *ch, const char *interp, const char *prog);

/* Close the program's stdin, wait for the interpreter, and report how it
 * ended. *exited is 1 and *code its status when it exited normally; *exited
 * is 0 and *code the signal number when it was killed. */
bs_err bs_child_finish(struct bs_chan *ch, int *exited, int *code);

/* Stop waiting. Used when the conversation has already failed and the
 * interpreter may be blocked forever on a read that will never be answered. */
void bs_child_kill(struct bs_chan *ch);

#endif
