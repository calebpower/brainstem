/* broker.c — the loop. The only translation unit that reads or writes the
 * wire, which is why no op handler can: the descriptors live in a struct
 * declared over in brainstem.h, and nothing an op file includes can name
 * this file's channel variable.
 *
 * THE DEADLOCK PROOF, because every line below depends on it.
 *
 * Two pipes, one process at each end, and frames that can be larger than a
 * pipe buffer. That is the classic deadlock shape, and the protocol is safe
 * only because of strict half duplex (ABI.md invariant I3): the program
 * emits a whole request before any read, and the broker reads a whole
 * request before any write and writes a whole reply before any read.
 *
 * Given that, at every instant exactly one side is writing and the other is
 * blocked in a read that will drain it. A pipe write blocks only when the
 * buffer is full AND nobody is reading; here the reader is always reading.
 * So no frame size and no pipe buffer size can deadlock the protocol, and
 * BS_PAYLOAD_MAX is a memory budget rather than a safety limit. Worth
 * stating because the natural assumption is the opposite, and because
 * FreeBSD's smaller default pipe buffer would otherwise look alarming.
 *
 * What CAN still hang is an interpreter that buffers its output, which is
 * not a protocol flaw but has the same signature. That is what the hello
 * timeout is for, and it is the most valuable error message in the program.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

#include "broker.h"
#include "child.h"
#include "ops.h"
#include "sys.h"
#include "det.h"

/* Two fixed buffers and no allocation on the ABI path, per CONVENTIONS
 * section 5. A frame is at most three bytes of header and a u16 of payload,
 * so these are the largest anything can ever be and the sizes cannot be
 * exceeded by arithmetic. */
static unsigned char in_arena[BS_FRAME_MAX];
static unsigned char out_arena[BS_PAYLOAD_MAX];

/* ---- raw I/O, with a deadline ------------------------------------------ */

/* Read exactly n bytes, or fail. Short reads are normal on a pipe, EINTR is
 * normal, and a header split across two reads is normal; all three are
 * handled here, once, so nothing above this has to think about them.
 *
 * Returns BS_OK, BS_END at a clean end of stream before any byte of this
 * read, BS_TIMEDOUT, or BS_IO. */
static bs_err read_exact(int fd, unsigned char *p, size_t n, long deadline_ms) {
    size_t got = 0;
    while (got < n) {
        if (deadline_ms > 0) {
            struct pollfd pf;
            int r;
            pf.fd = fd; pf.events = POLLIN; pf.revents = 0;
            do { r = poll(&pf, 1, (int)deadline_ms); } while (r < 0 && errno == EINTR);
            if (r == 0) return BS_TIMEDOUT;
            if (r < 0)  return BS_IO;
        }
        {
            ssize_t k = read(fd, p + got, n - got);
            if (k < 0) { if (errno == EINTR) continue; return BS_IO; }
            if (k == 0) return got == 0 ? BS_END : BS_PROTO;   /* EOF mid frame is truncation */
            got += (size_t)k;
        }
    }
    return BS_OK;
}

/* Write exactly n bytes, or fail. The peer is always reading (see the proof
 * above), so this cannot block forever against a live program -- but it can
 * against a dead one, which is why EPIPE is a status rather than a signal.
 * main.c ignores SIGPIPE so this returns instead of killing the broker. */
static bs_err write_exact(int fd, const unsigned char *p, size_t n) {
    size_t put = 0;
    while (put < n) {
        ssize_t k = write(fd, p + put, n - put);
        if (k < 0) {
            if (errno == EINTR) continue;
            if (errno == EPIPE) return BS_PIPE;
            return BS_IO;
        }
        put += (size_t)k;
    }
    return BS_OK;
}

/* ---- framing ----------------------------------------------------------- */

static bs_err read_frame(int fd, unsigned char *kind, unsigned int *len,
                         unsigned char *body, long first_ms, long rest_ms) {
    unsigned char hdr[BS_HDR_LEN];
    bs_err e = read_exact(fd, hdr, BS_HDR_LEN, first_ms);
    if (e != BS_OK) return e;
    bs_hdr_get(hdr, kind, len);

    /* A declared length is refused BEFORE a byte of body is read -- except
     * that it cannot be, here, because the length is a u16 and the arena is
     * sized for the largest u16. The check would be dead code and saying so
     * is better than writing an if nobody can make true. */
    if (*len == 0) return BS_OK;

    /* Past the header, the rest of the frame is already in flight, so the
     * per-op deadline applies rather than the handshake one. A program that
     * emits half a frame and then computes for a minute is doing nothing
     * wrong; one that emits half a frame and stops is. */
    return read_exact(fd, body, *len, rest_ms);
}

static bs_err write_frame(int fd, unsigned char kind, const unsigned char *body, unsigned int len) {
    unsigned char hdr[BS_HDR_LEN];
    bs_err e;
    bs_hdr_put(hdr, kind, len);
    e = write_exact(fd, hdr, BS_HDR_LEN);
    if (e != BS_OK) return e;
    if (len == 0) return BS_OK;
    return write_exact(fd, body, len);
}

/* One line per frame: direction, opcode or status, length, and THE PAYLOAD
 * IN HEX.
 *
 * The payload is what makes the trace an artifact a test can pin rather than
 * a debugging aid. Tier 7 asserts two runs under the same seed produce the
 * same trace and two runs under different seeds do not; tier 10 asserts the
 * trace matches a file committed to this repository rather than whatever
 * this machine happened to produce. Neither is possible from lengths alone.
 *
 * It is NOT truncated, and a 65535 byte payload really does print as 131072
 * characters. A trace that silently elided the interesting part would be
 * worse than no trace, and --trace is opt-in: nothing prints it by accident.
 * For the same reason, note that a trace of a real program contains whatever
 * that program read and wrote -- it is a debugging tool, not an audit log,
 * and it should be treated as carrying the data it names. */
static void trace(const struct bs_opts *o, const char *dir, unsigned char kind,
                  const unsigned char *body, unsigned int len) {
    unsigned int i;
    if (!o->trace) return;
    fprintf(stderr, "brainstem: %s %02x len=%u", dir, kind, len);
    if (len) {
        fputc(' ', stderr);
        for (i = 0; i < len; i++) fprintf(stderr, "%02x", body[i]);
    }
    fputc('\n', stderr);
}

/* ---- the loop ---------------------------------------------------------- */

int bs_broker_run(const struct bs_opts *o) {
    struct bs_chan ch;
    struct bs_ctx  ctx;
    int exited = 1, code = 0;
    int rc = BS_EXIT_OK;
    int first = 1;

    if (bs_ops_init() != 0) return BS_EXIT_USAGE;

    ctx.hello_done = 0;
    ctx.exiting    = 0;
    ctx.exit_code  = 0;

    /* The monotonic clock is normalised to zero here, before the child
     * exists, so "time since the broker started" means the same thing in
     * every run rather than encoding how long this machine has been up. */
    sys_clock_init();

    if (bs_child_start(&ch, o->interp, o->prog) != BS_OK) return BS_EXIT_INTERP;

    /* Privilege is dropped HERE: after the child exists and before the first
     * frame is read, so the filter measures the steady state ABI path by
     * construction and libc's own startup is out of scope without anyone
     * having to account for it. A no-op in v1 with its signature frozen, so
     * M8 is an implementation rather than a refactor -- see sys.h. */
    if (sys_lockdown() != BS_OK) {
        fprintf(stderr, "brainstem: could not drop privilege\n");
        bs_child_kill(&ch);
        return BS_EXIT_INTERP;
    }


    for (;;) {
        unsigned char kind;
        unsigned int  len;
        bs_err e;

        /* The handshake gets its own, shorter deadline, because the thing it
         * is diagnosing is specific: an interpreter that buffers its stdout
         * will never deliver the first frame, and without a bound on the
         * first read that is a hang with no output and no core. */
        long first_ms = first ? o->hello_timeout_ms : o->op_timeout_ms;

        e = read_frame(ch.from_prog, &kind, &len, in_arena, first_ms, o->op_timeout_ms);

        if (e == BS_END) break;                       /* the program ended, cleanly */
        if (e == BS_TIMEDOUT) {
            if (first) {
                fprintf(stderr,
                    "brainstem: no hello within %ld ms.\n"
                    "brainstem: the interpreter is probably buffering its stdout, which\n"
                    "brainstem: deadlocks the protocol -- the request never leaves it.\n"
                    "brainstem: try 'brainstem --check-interpreter %s', or stdbuf -o0.\n",
                    o->hello_timeout_ms, o->interp);
            } else {
                fprintf(stderr, "brainstem: the program stopped speaking mid conversation\n");
            }
            bs_child_kill(&ch);
            return BS_EXIT_TIMEOUT;
        }
        if (e != BS_OK) {
            fprintf(stderr, "brainstem: truncated or unreadable frame\n");
            bs_child_kill(&ch);
            return BS_EXIT_PROTO;
        }
        first = 0;
        trace(o, ">", kind, in_arena, len);

        /* ONE TICK PER REQUEST, not per clock_now, and counted here rather
         * than in the handler so that every op advances the virtual clock.
         * A program's request sequence is a property of the program alone,
         * which is what makes the virtual clock deterministic; tying it to
         * clock_now instead would make the clock depend on how often it was
         * read, which is the one thing a clock must not do. */
        det_tick();


        {
            const struct bs_op *op = bs_op_lookup(kind);
            struct bs_cur req;
            struct bs_buf rep;
            bs_err st;

            bs_cur_init(&req, in_arena, len);
            bs_buf_init(&rep, out_arena, sizeof out_arena);

            if (!op) {
                /* Not fatal. Every frame is length prefixed, so the payload
                 * has already been consumed and the stream is still in step.
                 * This is the forward compatibility path for a program built
                 * against a later minor version. */
                st = BS_NOSUCHOP;
            } else if (!ctx.hello_done && op->code != BS_OP_HELLO) {
                st = BS_NOHELLO;
            } else if (!bs_op_arity_ok(op, len)) {
                st = BS_BADLEN;
            } else if (!op->fn) {
                st = BS_NOSUCHOP;
            } else {
                st = op->fn(&ctx, &req, &rep);
            }

            /* An error response carries no payload, ever (invariant I6). A
             * program that got a bad status reads exactly two more bytes,
             * both zero, and is done. */
            if (st != BS_OK) bs_buf_init(&rep, out_arena, sizeof out_arena);

            trace(o, "<", (unsigned char)st, out_arena, (unsigned int)bs_buf_len(&rep));
            e = write_frame(ch.to_prog, (unsigned char)st,
                            out_arena, (unsigned int)bs_buf_len(&rep));

            if (e == BS_PIPE) {
                /* The program stopped reading. Nothing left to say to it. */
                break;
            }
            if (e != BS_OK) { bs_child_kill(&ch); return BS_EXIT_PROTO; }

            if (bs_err_fatal(st)) {
                fprintf(stderr, "brainstem: %s: %s\n", bs_err_name(st), bs_err_text(st));
                bs_child_kill(&ch);
                return BS_EXIT_PROTO;
            }
            if (ctx.exiting) break;
        }
    }

    bs_child_finish(&ch, &exited, &code);

    if (ctx.exiting) {
        /* The program asked for a status; that is the answer, whatever the
         * interpreter then did on its way out. */
        rc = ctx.exit_code == 0 ? BS_EXIT_OK : BS_EXIT_PROGRAM;
    } else if (!exited) {
        fprintf(stderr, "brainstem: the interpreter was killed by signal %d\n", code);
        rc = BS_EXIT_INTERP;
    } else if (code == 127) {
        rc = BS_EXIT_INTERP;
    } else if (code != 0) {
        fprintf(stderr, "brainstem: the interpreter exited %d\n", code);
        rc = BS_EXIT_PROGRAM;
    } else {
        /* Ended without an exit frame. Not an error -- a program is allowed
         * to simply stop -- but worth saying, because the commonest reason
         * is that it fell off the end of its instructions by accident. */
        fprintf(stderr, "brainstem: the program ended without an exit frame\n");
        rc = BS_EXIT_OK;
    }
    return rc;
}
