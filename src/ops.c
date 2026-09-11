/* ops.c — the op table, built from ops.def, and the index over it. */
#include <stdio.h>
#include "ops.h"

/* The handlers, declared by hand rather than generated from ops.def.
 *
 * Generating them would mean emitting a prototype for every row including
 * the ones whose handler is NULL, and "bs_err NULL(...)" is not a
 * declaration. Listing the built ones here is a second list, but it is one
 * the compiler checks: a missing declaration will not compile, and a
 * declaration whose signature drifts from the typedef will not compile
 * either. A second list that cannot be wrong is not the kind this project
 * worries about.
 */
bs_err op_ctl_hello(struct bs_ctx *, struct bs_cur *, struct bs_buf *);
bs_err op_ctl_exit (struct bs_ctx *, struct bs_cur *, struct bs_buf *);

static const struct bs_op bs_ops[] = {
#define OP(c, n, f, q, p) { (c), #n, f, (q), (p) },
#include "ops.def"
#undef OP
};

#define BS_NOPS (sizeof bs_ops / sizeof bs_ops[0])

/* index[code] is the row, or 0xFF for none. Built once rather than written
 * out as a 256 row literal: a literal would be mostly holes that say nothing
 * about whether a code exists, and it would be a second place the opcodes
 * are listed. */
static unsigned char bs_index[256];
static int bs_ready;

int bs_ops_init(void) {
    size_t i;
    for (i = 0; i < 256; i++) bs_index[i] = 0xFF;
    for (i = 0; i < BS_NOPS; i++) {
        unsigned char c = bs_ops[i].code;
        if (c == 0x00) {
            fprintf(stderr, "brainstem: ops.def has an op at code 0, which is reserved\n");
            return 1;
        }
        if (bs_index[c] != 0xFF) {
            fprintf(stderr, "brainstem: ops.def has two ops at code 0x%02x (%s and %s)\n",
                    c, bs_ops[bs_index[c]].name, bs_ops[i].name);
            return 1;
        }
        if (!bs_ops[i].name || !bs_ops[i].name[0]) {
            fprintf(stderr, "brainstem: ops.def has a row at 0x%02x with no name\n", c);
            return 1;
        }
        bs_index[c] = (unsigned char)i;
    }
    bs_ready = 1;
    return 0;
}

const struct bs_op *bs_op_lookup(unsigned char code) {
    if (!bs_ready) return 0;
    if (bs_index[code] == 0xFF) return 0;
    return &bs_ops[bs_index[code]];
}

int bs_op_arity_ok(const struct bs_op *o, unsigned int len) {
    if (o->reqlen == BS_VAR) return 1;
    return len == o->reqlen;
}

const struct bs_op *bs_op_row(size_t i) { return i < BS_NOPS ? &bs_ops[i] : 0; }
size_t bs_op_count(void) { return BS_NOPS; }
