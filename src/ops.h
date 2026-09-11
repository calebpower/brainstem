/* ops.h — the op table and its dispatcher.
 *
 * Dispatch is a bounds-checked array index. No switch, no default arm that
 * does work, no fallthrough. Arity is checked from the table before the
 * handler runs, the handler is called exactly once, and the reply is written
 * in exactly one place -- in broker.c, which is the only translation unit
 * that can name the channel at all.
 *
 * That last point is structural rather than a rule somebody remembers: an
 * op handler receives a request payload and a reply buffer and has no way to
 * reach a file descriptor, because the type that holds them is not in any
 * header an op file includes.
 */
#ifndef BS_OPS_H
#define BS_OPS_H

#include "brainstem.h"
#include "frame.h"

/* A length that depends on the frame rather than the op. */
#define BS_VAR 0xFFFF

/* An op handler. It sees the request payload, a cursor already positioned at
 * its start, a buffer to build the reply in, and the broker context. It
 * returns a status; BS_OK means the reply buffer is the payload. */
typedef bs_err (*bs_op_fn)(struct bs_ctx *ctx,
                           struct bs_cur *req,
                           struct bs_buf *rep);

struct bs_op {
    unsigned char  code;
    const char    *name;
    bs_op_fn       fn;
    unsigned int   reqlen;
    unsigned int   replen;
};

#define OP(c, n, f, q, p) BS_OP_##n = (c),
enum {
#include "ops.def"
    BS_OP_SENTINEL_UNUSED = -1
};
#undef OP

/* Build the index. Verifies the table on the way: no duplicate opcode, no
 * opcode 0, no row whose declared name is empty. Returns nonzero on a
 * malformed table, which is a programming error rather than a runtime one
 * and so aborts startup rather than being reported per frame. */
int bs_ops_init(void);

/* NULL when no row has this code. A row whose fn is NULL exists but is not
 * built, which is a different answer and gets NOSUCHOP. */
const struct bs_op *bs_op_lookup(unsigned char code);

/* Does this request length satisfy the row's declared arity? BS_VAR accepts
 * anything, because only the handler knows the tail's shape. */
int bs_op_arity_ok(const struct bs_op *o, unsigned int len);

/* Every row, for --dump-abi and for the table checker. */
const struct bs_op *bs_op_row(size_t i);
size_t bs_op_count(void);

#endif
