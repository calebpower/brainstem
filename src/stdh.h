/* stdh.h — the three handles every program starts with.
 *
 * A brainstem program is handed the broker's own stdin, stdout and stderr as
 * handles 1, 2 and 3, always, with no flag and no command line. Writing to
 * handle 2 prints; that is the whole interface.
 *
 * THIS REPLACED THE PREOPEN MODEL, and the reason is worth keeping.
 *
 * Preopens required the operator and the program to agree on a handle INDEX
 * out of band -- and a brainfuck program cannot compare strings, so there was
 * deliberately no name-discovery op for it to use instead. That is the one
 * kind of coordination brainfuck is worst at, and it was imposed to buy a
 * containment property the project never claimed: CONVENTIONS has said
 * "brainstem is not a sandbox" since the first commit, and the approved plan
 * said in as many words that the broker "hands a brainfuck program the
 * filesystem, network and process spawn with its own credentials".
 *
 * What a brainfuck program IS good at is emitting literal bytes, so a literal
 * path is the cheapest thing it can produce. Paths now resolve the way they
 * do for any other process, and these three handles cover the one thing a
 * path cannot portably express: a descriptor the broker was handed by a shell.
 *
 * They still travel in the hello reply's handle table (ABI.md section 8.2),
 * because a program that wants to know what it was actually given -- a
 * terminal, a file, a pipe -- should not have to assume.
 */
#ifndef BS_STDH_H
#define BS_STDH_H

#include "sys.h"

/* Installs the three, in order, as handles 1, 2 and 3. Called once from the
 * broker before the child starts. On failure *failed_fd is the descriptor
 * that could not be taken over. */
bs_err bs_stdh_install(int *failed_fd);

#endif
