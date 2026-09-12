/* replay.h — re-run a recorded conversation, answering from the recording.
 *
 * WHAT --replay IS FOR, because "run it again" is not obviously worth a flag.
 *
 * A trace is an artifact this project pins: tier 7 requires two runs under
 * one seed to produce the same one, and tier 10 requires both platforms to
 * produce the byte-identical file committed in tests/trace/. All of that
 * treats the trace as a RECORD. Nothing checked that it was a COMPLETE
 * record -- a trace that dropped a frame, or truncated a payload, would still
 * compare equal to itself and to a pinned copy of itself.
 *
 * Replay is what makes the record load bearing. The broker reads the trace
 * instead of the ops: every request the program emits is compared against the
 * one recorded at that position, and the recorded reply is written back. A
 * trace that is missing anything cannot drive the program that produced it,
 * and a program that has become nondeterministic diverges at a numbered frame
 * with both payloads printed, rather than somewhere downstream.
 *
 * AND NO SYSCALL IS MADE ON THE ABI PATH. Under --replay the broker does not
 * install the standard handles, does not capture a clock origin, opens
 * nothing, and never calls an op handler. That claim is checked behaviourally
 * rather than with a tracer, which is stronger here: a frozen clock trace
 * replayed under --clock live still reports the frozen instant, a seeded
 * keystream replayed with no seed still comes back, and bf/fs/roundtrip.bf
 * replayed in an empty directory leaves it empty. Each of those proves the
 * VALUE came out of the recording, where a syscall count only proves nothing
 * was asked.
 *
 * The exit status is about the REPLAY, not about the program: zero when the
 * whole recording matched and was consumed, nonzero on any divergence. A
 * recorded ctl.exit of 3 does not make a replay exit 3, because the question
 * a replay answers is "does this still happen", not "what happened".
 */
#ifndef BS_REPLAY_H
#define BS_REPLAY_H

#include "brainstem.h"
#include "frame.h"

/* Loading. Fed as raw file bytes rather than as lines, so the caller needs no
 * buffer of its own: a trace line can be 131 kilobytes, because a payload can
 * be 65535 bytes and the trace does not truncate. */
void   bs_replay_begin(void);
bs_err bs_replay_push(const unsigned char *b, size_t n);
bs_err bs_replay_end(void);

int    bs_replay_active(void);
size_t bs_replay_count(void);
size_t bs_replay_left(void);

/* Match one request against the recording and fill rep with the recorded
 * reply. Returns the recorded STATUS on a match -- which may itself be an
 * error status, because that is what was recorded -- or BS_PROTO when the
 * request does not match, with the divergence printed to stderr. */
bs_err bs_replay_frame(unsigned char kind, const unsigned char *req,
                       unsigned int len, struct bs_buf *rep);

#endif
