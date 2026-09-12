/* replay.c — the recording, and the comparison. See replay.h for why.
 *
 * No allocation, here as everywhere: the recording lands in one fixed arena
 * and one fixed pair of frame tables, and a trace too large for them is
 * REFUSED with a number rather than truncated into something that would
 * replay wrongly. The sizes are generous against what this project's fixtures
 * produce -- the largest is under two kilobytes across sixteen frames -- and
 * the failure is at load time, before the interpreter has been started.
 */
#include <stdio.h>
#include <string.h>

#include "replay.h"

/* Sixty five thousand bytes of payload is two hex characters each, so a
 * single trace line is bounded at a little over 131 kilobytes. The line
 * buffer is the largest thing in this file and it is why the caller does not
 * need one. */
#define RP_LINE   (32u + 2u * 65535u)
#define RP_FRAMES 2048u
#define RP_ARENA  (256u * 1024u)

struct rp_frame {
    unsigned char kind;
    unsigned int  off, len;
};

static struct rp_frame rp_req[RP_FRAMES];
static struct rp_frame rp_rep[RP_FRAMES];
static unsigned char   rp_arena[RP_ARENA];

static char   rp_line[RP_LINE];
static size_t rp_linelen;
static size_t rp_lineno;
static size_t rp_used;          /* bytes of arena consumed */
static size_t rp_n;             /* complete request/reply pairs */
static int    rp_want_reply;    /* a request is recorded and its reply is not */
static int    rp_on;            /* --replay was given */
static int    rp_bad;           /* a load error has already been reported */
static size_t rp_at;            /* how many pairs have been replayed */

static int rp_nyb(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void rp_hex(const unsigned char *p, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n; i++) fprintf(stderr, "%02x", p[i]);
    if (n == 0) fprintf(stderr, "(empty)");
}

void bs_replay_begin(void) {
    rp_linelen = 0;
    rp_lineno = 0;
    rp_used = 0;
    rp_n = 0;
    rp_want_reply = 0;
    rp_bad = 0;
    rp_at = 0;
    rp_on = 1;
}

/* One line of a trace. Anything that is not a frame line is IGNORED, because
 * a real trace carries the broker's own diagnostics alongside the frames --
 * "the program ended without an exit frame" is not a frame and never was.
 *
 * A line that has the frame SHAPE and a payload that is not hex is a
 * different matter and is refused by name: that is what a normalised trace
 * from tests/trace/ looks like, where a mask has replaced the platform byte
 * with %% or the handle table with H. Those files exist to be compared, not
 * replayed, and silently skipping them would produce an empty recording and a
 * replay that matched nothing. */
static bs_err rp_line_done(void) {
    const char *p = rp_line;
    const char *tag = "brainstem: ";
    unsigned char kind;
    unsigned long declared = 0;
    unsigned int got = 0;
    int dir;

    rp_lineno++;
    /* The shortest frame line is "brainstem: > 01 len=0", twenty one bytes.
     * Anything shorter cannot be one. */
    if (rp_linelen < 21) return BS_OK;
    if (strncmp(p, tag, strlen(tag)) != 0) return BS_OK;
    p += strlen(tag);

    if (*p == '>') dir = 0;
    else if (*p == '<') dir = 1;
    else return BS_OK;
    p++;
    if (*p != ' ') return BS_OK;
    p++;

    if (rp_nyb(p[0]) < 0 || rp_nyb(p[1]) < 0) return BS_OK;
    kind = (unsigned char)(rp_nyb(p[0]) * 16 + rp_nyb(p[1]));
    p += 2;

    if (strncmp(p, " len=", 5) != 0) return BS_OK;
    p += 5;
    if (*p < '0' || *p > '9') return BS_OK;
    while (*p >= '0' && *p <= '9') { declared = declared * 10 + (unsigned long)(*p - '0'); p++; }
    if (declared > 65535) {
        fprintf(stderr, "brainstem: --replay: line %lu declares %lu bytes, which is not a frame\n",
                (unsigned long)rp_lineno, declared);
        return BS_PROTO;
    }
    if (*p == ' ') p++;
    else if (*p != '\0') return BS_OK;

    /* the payload, and from here a malformed line is an ERROR rather than
     * something to skip: the shape has already been matched */
    if (rp_used + declared > RP_ARENA) {
        fprintf(stderr, "brainstem: --replay: the recording is larger than %lu bytes\n",
                (unsigned long)RP_ARENA);
        return BS_EXHAUSTED;
    }
    while (*p) {
        int hi = rp_nyb(p[0]);
        int lo = p[1] ? rp_nyb(p[1]) : -1;
        if (hi < 0 || lo < 0) {
            fprintf(stderr,
                "brainstem: --replay: line %lu has a payload that is not hex.\n"
                "brainstem: if this came from tests/trace/ it is a NORMALISED trace --\n"
                "brainstem: masked bytes are written %%%% or M or H -- and those files exist\n"
                "brainstem: to be compared, not replayed. Record a fresh one with --trace.\n",
                (unsigned long)rp_lineno);
            return BS_PROTO;
        }
        /* Written only while it fits: one hex pair too many must be an
         * error rather than a byte past the end of the arena. */
        if (got < declared) rp_arena[rp_used + got] = (unsigned char)(hi * 16 + lo);
        got++;
        p += 2;
        if (got > declared) break;
    }
    if (got != declared) {
        fprintf(stderr, "brainstem: --replay: line %lu says len=%lu and carries %u bytes\n",
                (unsigned long)rp_lineno, declared, got);
        return BS_PROTO;
    }

    /* STRICT ALTERNATION, which is invariant I3 and the whole deadlock proof.
     * A recording that broke it could not have come from this broker, and
     * checking it here means a replay cannot be driven by one. */
    if (dir == 0) {
        if (rp_want_reply) {
            fprintf(stderr, "brainstem: --replay: line %lu is a second request with no reply between\n",
                    (unsigned long)rp_lineno);
            return BS_PROTO;
        }
        if (rp_n >= RP_FRAMES) {
            fprintf(stderr, "brainstem: --replay: more than %lu frames in the recording\n",
                    (unsigned long)RP_FRAMES);
            return BS_EXHAUSTED;
        }
        rp_req[rp_n].kind = kind;
        rp_req[rp_n].off  = (unsigned int)rp_used;
        rp_req[rp_n].len  = (unsigned int)declared;
        rp_want_reply = 1;
    } else {
        if (!rp_want_reply) {
            fprintf(stderr, "brainstem: --replay: line %lu is a reply to nothing\n",
                    (unsigned long)rp_lineno);
            return BS_PROTO;
        }
        rp_rep[rp_n].kind = kind;
        rp_rep[rp_n].off  = (unsigned int)rp_used;
        rp_rep[rp_n].len  = (unsigned int)declared;
        rp_want_reply = 0;
        rp_n++;
    }
    rp_used += declared;
    return BS_OK;
}

bs_err bs_replay_push(const unsigned char *b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (b[i] == '\n' || b[i] == '\r') {
            if (rp_linelen) {
                bs_err e;
                rp_line[rp_linelen] = '\0';
                e = rp_line_done();
                rp_linelen = 0;
                if (e != BS_OK) { rp_bad = 1; return e; }
            }
            continue;
        }
        if (rp_linelen + 1 >= RP_LINE) {
            fprintf(stderr, "brainstem: --replay: a line longer than %lu bytes\n",
                    (unsigned long)RP_LINE);
            rp_bad = 1;
            return BS_EXHAUSTED;
        }
        rp_line[rp_linelen++] = (char)b[i];
    }
    return BS_OK;
}

bs_err bs_replay_end(void) {
    if (rp_linelen) {
        bs_err e;
        rp_line[rp_linelen] = '\0';
        e = rp_line_done();
        rp_linelen = 0;
        if (e != BS_OK) { rp_bad = 1; return e; }
    }
    if (rp_bad) return BS_PROTO;
    if (rp_want_reply) {
        fprintf(stderr, "brainstem: --replay: the recording ends with a request and no reply\n");
        return BS_PROTO;
    }
    if (rp_n == 0) {
        fprintf(stderr,
            "brainstem: --replay: no frames in that file.\n"
            "brainstem: a recording is what --trace writes to stderr, so capture it with\n"
            "brainstem: '2>trace.txt' and replay that.\n");
        return BS_PROTO;
    }
    return BS_OK;
}

int    bs_replay_active(void) { return rp_on; }
size_t bs_replay_count(void)  { return rp_n; }
size_t bs_replay_left(void)   { return rp_n - rp_at; }

bs_err bs_replay_frame(unsigned char kind, const unsigned char *req,
                       unsigned int len, struct bs_buf *rep) {
    const struct rp_frame *q, *a;

    if (rp_at >= rp_n) {
        fprintf(stderr,
            "brainstem: --replay: the program sent frame %lu and the recording has %lu\n",
            (unsigned long)(rp_at + 1), (unsigned long)rp_n);
        return BS_PROTO;
    }
    q = &rp_req[rp_at];
    a = &rp_rep[rp_at];

    if (kind != q->kind || len != q->len ||
        (len && memcmp(req, rp_arena + q->off, len) != 0)) {
        fprintf(stderr, "brainstem: --replay: frame %lu diverges from the recording\n",
                (unsigned long)(rp_at + 1));
        fprintf(stderr, "brainstem:   recorded: %02x len=%u ", q->kind, q->len);
        rp_hex(rp_arena + q->off, q->len);
        fprintf(stderr, "\nbrainstem:   sent:     %02x len=%u ", kind, len);
        rp_hex(req, len);
        fprintf(stderr, "\n");
        return BS_PROTO;
    }

    if (a->len) bs_put_bytes(rep, rp_arena + a->off, a->len);
    rp_at++;
    if (!bs_buf_ok(rep)) return BS_IO;
    return (bs_err)a->kind;
}
