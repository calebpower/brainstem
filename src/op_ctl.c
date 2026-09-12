/* op_ctl.c — hello and exit.
 *
 * These two are the only ops that issue no syscall at all: they touch no
 * clock, no descriptor and nothing beyond the broker's own lifecycle. That
 * is why M2 is built out of them. The pipe topology, the half duplex
 * discipline, the deadlock proof and the buffering diagnosis all get settled
 * here, once, with no platform surface to confuse the result -- and twenty
 * one more ops inherit a channel that has already been proved.
 *
 * Since M3, hello does READ the seam -- sys_platform() and the determinism
 * knobs -- to report what world the program is in. Every one of those is a
 * compile time constant or a variable set from argv, so the claim the
 * syscall tier makes about this file is unchanged: hello's pinned multiset
 * is empty, and tests/syscalls/<platform>/ctl.hello.txt says so.
 *
 * Note what this file cannot do. It cannot write to the wire: the type that
 * holds the descriptors is declared in broker.c and is not in any header
 * included here, so an op physically has no way to reach the channel. That
 * is the "one code path per op" property made structural rather than
 * remembered.
 */
#include "ops.h"
#include "det.h"
#include "fdtab.h"

/* ABI.md section 3. Request: magic "BSTM", want_major u16, want_minor u16,
 * flags u16. Exactly ten bytes, and the dispatcher has already checked that
 * before this runs. */
bs_err op_ctl_hello(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    const unsigned char *magic;
    unsigned int want_major, want_minor, flags;

    if (ctx->hello_done) return BS_REHELLO;

    magic      = bs_get_bytes(req, 4);
    want_major = bs_get_u16(req);
    want_minor = bs_get_u16(req);
    flags      = bs_get_u16(req);
    (void)flags;                         /* reserved; a nonzero value is not an error yet */

    if (!bs_cur_ok(req) || !bs_cur_done(req)) return BS_BADLEN;

    if (magic[0] != 'B' || magic[1] != 'S' || magic[2] != 'T' || magic[3] != 'M')
        return BS_PROTO;

    /* The BROKER does the version comparison, not the program. A numeric
     * comparison is cheap in C and expensive in brainfuck, and a program
     * that got it wrong would misbehave in a way nobody could debug. */
    if (want_major != BS_VER_MAJOR) return BS_VERSION;
    if (want_minor > BS_VER_MINOR)  return BS_VERSION;

    /* The reply must be NON-EMPTY and must contain non-zero bytes, and that
     * is load bearing rather than incidental. An interpreter that stores 0
     * at end of input gives a program a status byte of 0x00, which is OK --
     * so the only thing distinguishing "the broker said yes" from "there is
     * no broker at all" is that a real reply has a length and a magic in it.
     * See ABI.md section 3.2. Never make this reply empty. */
    bs_put_bytes(rep, (const unsigned char *)"BSTM", 4);
    bs_put_u16(rep, BS_VER_MAJOR);
    bs_put_u16(rep, BS_VER_MINOR);
    bs_put_u16(rep, BS_PAYLOAD_MAX);     /* max_payload */
    bs_put_u16(rep, 0x17);               /* max_op, the highest declared opcode */

    /* feature bits: bit n set means opcode n is available. Built from the
     * table so it cannot drift from what the broker will actually answer --
     * a hand written constant here would be a second statement of which ops
     * exist, and the first one to go stale. */
    {
        unsigned long lo = 0, hi = 0;
        size_t i;
        for (i = 0; i < bs_op_count(); i++) {
            const struct bs_op *o = bs_op_row(i);
            if (!o->fn) continue;
            if (o->code < 32) lo |= 1UL << o->code;
            else              hi |= 1UL << (o->code - 32);
        }
        bs_put_u32(rep, lo);
        bs_put_u32(rep, hi);
    }

    /* The determinism block. A program reads these to know what world it is
     * in without having to be told out of band -- which matters because the
     * SAME fixture runs live in the per-op tier and seeded in the
     * determinism tier, and a fixture that cannot tell the difference cannot
     * assert anything about either. */
    bs_put_u8(rep, sys_platform());
    bs_put_u8(rep, (unsigned int)det_clock_mode());
    bs_put_u8(rep, det_rng_seeded() ? 1u : 0u);
    bs_put_u8(rep, 0);                          /* reserved */
    bs_put_u32(rep, det_clock_step());

    /* The seed is REPORTED BACK, all sixteen bytes, and that is deliberate.
     * It is not a secret -- it is on the command line -- and echoing it is
     * what lets a trace be replayed from the trace alone. When no seed is in
     * force these are sixteen zeros, which is also the value that says so. */
    bs_put_bytes(rep, det_seed_bytes(), 16);
    /* The preopen table, and the two counts that introduce it.
     *
     * Positional, with no name discovery op, because a brainfuck program
     * cannot usefully compare strings and there is nothing intelligent it
     * could do with a name it discovered. What it CAN read cheaply is each
     * preopen's kind and rights, which is what the table carries, at no
     * round trip cost at all. */
    {
        size_t i, n = bs_fdtab_count();
        unsigned int npre = 0, tail = 0;
        for (i = 1; i < n; i++) {
            struct bs_slot *s = bs_fdtab_slot(i);
            if (!s || !s->preopen) continue;
            npre++;
            tail += 1u + s->namelen;
        }
        bs_put_u16(rep, npre);
        bs_put_u16(rep, tail);

        for (i = 1; i < n; i++) {
            struct bs_slot *s = bs_fdtab_slot(i);
            if (!s || !s->preopen) continue;
            bs_put_u32(rep, bs_fdtab_handle_of(i));
            bs_put_u8 (rep, s->kind);
            bs_put_u8 (rep, s->namelen);
            bs_put_u16(rep, s->rights);
            bs_put_zero(rep, 4);
        }

        /* The name tail: one length byte then the bytes, per entry, in the
         * same order. A program that does not care reads 48 + 12 * npreopen
         * and then counts nametail_len bytes away; one that does walks it
         * with a u8 counter. Both are flat loops, which is the entire design
         * requirement. */
        for (i = 1; i < n; i++) {
            struct bs_slot *s = bs_fdtab_slot(i);
            if (!s || !s->preopen) continue;
            bs_put_u8(rep, s->namelen);
            if (s->namelen) bs_put_bytes(rep, (const unsigned char *)s->name, s->namelen);
        }
    }

    if (!bs_buf_ok(rep)) return BS_IO;
    ctx->hello_done = 1;
    return BS_OK;
}

/* ABI.md section 7.2. One byte: the exit status.
 *
 * The broker replies OK FIRST and exits afterwards. That keeps invariant I2
 * -- exactly one response per request -- with no special case, and I2 is the
 * whole deadlock proof. An op that replied with nothing would be the one
 * place a program could not know whether to read. */
bs_err op_ctl_exit(struct bs_ctx *ctx, struct bs_cur *req, struct bs_buf *rep) {
    unsigned int code = bs_get_u8(req);
    (void)rep;
    if (!bs_cur_ok(req) || !bs_cur_done(req)) return BS_BADLEN;
    ctx->exiting   = 1;
    ctx->exit_code = (int)(code & 0xFF);
    return BS_OK;
}
