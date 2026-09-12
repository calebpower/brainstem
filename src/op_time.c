/* op_time.c — clock_now.
 *
 * ABI.md section 7.3. Request: clock_id{u8}, 0 realtime and 1 monotonic.
 * Reply: sec{u64} nsec{u32}, twelve bytes.
 *
 * Seconds and nanoseconds are split rather than folded into one count of
 * milliseconds so that NEITHER SIDE DIVIDES. A brainfuck program dividing a
 * 64 bit value is not a thing anyone should write, and a program wanting
 * whole seconds reads the first eight bytes and ignores four.
 */
#include "ops.h"
#include "det.h"

bs_err op_time_clock_now(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    unsigned int id;
    bs_time t;
    bs_err e;
    (void)ctx;

    id = bs_get_u8(req);
    if (!bs_cur_ok(req) || !bs_cur_done(req)) return BS_BADLEN;

    /* An unknown clock is INVAL rather than a silent fallback to realtime.
     * A program that asked for the wrong clock and got a plausible answer
     * would be wrong in a way nothing could show it. */
    if (id == 0)      e = det_clock_real(&t);
    else if (id == 1) e = det_clock_mono(&t);
    else              return BS_INVAL;

    if (e != BS_OK) return e;

    /* sec is signed on the tape and unsigned on the wire. Before 1970 is not
     * a thing this ABI expresses, and a negative value would be a broken
     * clock rather than a legitimate answer. */
    bs_put_u64(rep, (unsigned long long)t.sec);
    bs_put_u32(rep, (unsigned long)t.nsec);
    return bs_buf_ok(rep) ? BS_OK : BS_IO;
}
