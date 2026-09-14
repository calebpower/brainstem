/* brainstem.h — the constants and types shared across the broker.
 *
 * Every tunable here carries the reasoning for its value, because a bare
 * number in a header is a decision nobody can revisit.
 */
#ifndef BRAINSTEM_H
#define BRAINSTEM_H

#include <stddef.h>
#include "err.h"
#include "frame.h"

/* How long to wait for the program's first frame before concluding that its
 * interpreter is buffering.
 *
 * This is the single most valuable number in the program. An interpreter
 * using default stdio on a pipe holds the request in a 4 KiB buffer, the
 * broker blocks reading a request that was never sent, the program blocks on
 * a reply that cannot come, and the symptom is a hang with no output and no
 * core. Without this timeout that is where a first-time user stops. With it,
 * they get a sentence naming the cause.
 *
 * Five seconds is long enough that a slow machine starting a large program
 * is not accused, and short enough that nobody waits wondering. */
#define BS_HELLO_TIMEOUT_MS 5000

/* A ceiling on any single blocking operation, so a wedged conversation ends
 * rather than hanging a CI job. Zero disables it. There is deliberately no
 * MID-FRAME deadline by default: a program may compute for minutes between
 * two bytes of one payload, and killing that would kill correct programs. */
#define BS_OP_TIMEOUT_MS 30000

/* The ABI version this broker speaks. Major is never negotiated; a minor
 * mismatch is fine in both directions and both sides operate at the lower. */
#define BS_VER_MAJOR 1
#define BS_VER_MINOR 1

/* Exit codes. The suite asserts on these, so they are contract rather than
 * convenience, and they are spread out rather than sequential so a new one
 * can be added in the right family later.
 *
 *   0   the program ran and asked to exit 0
 *   1   the program asked for a nonzero exit, or the interpreter died nonzero
 *   2   usage
 *   70  a fatal protocol error: a frame the broker could not parse, a desync,
 *       or a handshake that did not agree. ABI.md section 3.1 names this one.
 *   71  a timeout: the program stopped speaking mid conversation
 *   72  the interpreter could not be started, or broke its contract
 */
#define BS_EXIT_OK       0
#define BS_EXIT_PROGRAM  1
#define BS_EXIT_USAGE    2
#define BS_EXIT_PROTO   70
#define BS_EXIT_TIMEOUT 71
#define BS_EXIT_INTERP  72

/* The longest path the ABI will carry, and the size of the one buffer a path
 * is ever copied into.
 *
 * A path arrives on the wire as bytes with no terminator, and the seam needs
 * a C string, so there is exactly one copy and it is into a fixed buffer --
 * no allocation on the ABI path, here as everywhere. 1024 is under PATH_MAX
 * on both platforms with room to spare; a longer path is NAMETOOLONG, which
 * is a status the program can act on rather than a truncation it cannot
 * detect. */
#define BS_PATH_MAX 1024

/* The two pipes, and the child on the far end of them. */
struct bs_chan {
    int from_prog;   /* read: the interpreter's stdout */
    int to_prog;     /* write: the interpreter's stdin */
    long pid;
};

/* Broker-wide state an op handler is allowed to see. Deliberately small: an
 * op that needs more than this is reaching for something that belongs at the
 * platform seam or in the dispatcher. */
struct bs_ctx {
    int hello_done;      /* the handshake has completed */
    int exiting;         /* ctl.exit was called; this code is the broker's */
    int exit_code;
};

#endif
