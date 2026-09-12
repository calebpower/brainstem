/* op_rand.c — random_bytes.
 *
 * ABI.md section 7.4. Request: n{u16}. Reply: n bytes.
 *
 * n of zero is LEGAL and returns an empty OK. A program computing a length
 * that happens to come out zero should not have to special case it, and the
 * frame already carries its own length so an empty reply is unambiguous.
 */
#include "ops.h"
#include "det.h"
#include "brainstem.h"

bs_err op_rand_bytes(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    unsigned int n;
    bs_err e;
    (void)ctx;

    n = bs_get_u16(req);
    if (!bs_cur_ok(req) || !bs_cur_done(req)) return BS_BADLEN;
    if (n == 0) return BS_OK;
    /* No ceiling check: n is a u16 and BS_PAYLOAD_MAX is 65535, so the
     * condition cannot be true. Writing it anyway would be dead code that
     * looks like diligence. */

    /* Written straight into the reply buffer rather than through a scratch
     * array. There is no second copy to forget to clear, and the buffer is
     * already sized for the largest frame the ABI can express. */
    {
        unsigned char *dst = rep->p + rep->n;
        if (n > rep->cap - rep->n) return BS_IO;
        e = det_random(dst, n);
        if (e != BS_OK) return e;
        rep->n += n;
    }
    return BS_OK;
}
