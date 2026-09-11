/* frame.h — the wire codec, and nothing else.
 *
 * This translation unit touches no file descriptor, allocates nothing, and
 * holds no state between calls. That is not minimalism for its own sake: it
 * is what lets the codec tier run with no process, no descriptor and no
 * kernel, which in turn means a codec failure can never be confused with a
 * pipe failure. Short reads, EINTR and the deadline all belong to broker.c.
 *
 * Frames are symmetric. A request leads with an opcode, a reply with a
 * status, and the rest is identical:
 *
 *     kind{u8}  len{u16 LE}  payload{len}
 *
 * Little-endian because the sibling library's thirty-three primitives all
 * declare "len{2} LE" inputs and a program should not have to hold two byte
 * orders in its head. See ABI.md section 2.
 */
#ifndef BS_FRAME_H
#define BS_FRAME_H

#include <stddef.h>
#include "err.h"

/* Three bytes, and the length is a u16, so the largest frame is 65538 and no
 * arithmetic on a length can overflow a size_t on any platform we target.
 * That is a real safety property and the reason the broker can use two fixed
 * static buffers and never allocate on the ABI path. */
#define BS_HDR_LEN      3
#define BS_PAYLOAD_MAX  65535
#define BS_FRAME_MAX    (BS_HDR_LEN + BS_PAYLOAD_MAX)

/* ---- header ------------------------------------------------------------ */

/* Write a header into at least BS_HDR_LEN bytes. Cannot fail: len is a u16
 * and every u16 is a legal length. */
void bs_hdr_put(unsigned char *dst, unsigned char kind, unsigned int len);

/* Read a header from at least BS_HDR_LEN bytes. Cannot fail either, for the
 * same reason -- there is no bit pattern in three bytes that is not a valid
 * header. Validation of whether THIS op may carry THIS length is the
 * dispatcher's, because only it knows the op. Keeping that out of here is
 * what stops the codec needing an op table. */
void bs_hdr_get(const unsigned char *src, unsigned char *kind, unsigned int *len);

/* ---- reading a payload ------------------------------------------------- */

/* A cursor with a STICKY failure flag, so a parse is a run of straight-line
 * gets and exactly one check at the end. The alternative -- a status return
 * on every get -- produces either a wall of branches or, far more likely,
 * three branches and four unchecked calls. */
struct bs_cur {
    const unsigned char *p;
    size_t n;      /* bytes available */
    size_t i;      /* bytes consumed */
    int    bad;    /* set on the first underrun and never cleared */
};

void bs_cur_init(struct bs_cur *c, const unsigned char *p, size_t n);

/* Each returns 0 and sets ->bad on underrun. A zero from a failed get is
 * indistinguishable from a real zero BY DESIGN: the caller is expected to
 * check bs_cur_ok once, not to inspect each value. */
unsigned char       bs_get_u8 (struct bs_cur *c);
unsigned int        bs_get_u16(struct bs_cur *c);
unsigned long       bs_get_u32(struct bs_cur *c);
unsigned long long  bs_get_u64(struct bs_cur *c);

/* Returns a pointer INTO the caller's buffer, or NULL on underrun. No copy,
 * so the caller must not hold it past the arena's next reuse. */
const unsigned char *bs_get_bytes(struct bs_cur *c, size_t n);

int    bs_cur_ok(const struct bs_cur *c);
size_t bs_cur_left(const struct bs_cur *c);

/* True when the payload was consumed exactly. A parse that succeeds while
 * leaving bytes behind has misread the frame, and the op tables say every
 * request has an exact or minimum length precisely so this can be checked. */
int bs_cur_done(const struct bs_cur *c);

/* ---- writing a payload ------------------------------------------------- */

/* Same shape in the other direction: a sticky overflow flag rather than a
 * status on every put. */
struct bs_buf {
    unsigned char *p;
    size_t cap;
    size_t n;
    int    over;
};

void bs_buf_init(struct bs_buf *b, unsigned char *p, size_t cap);

void bs_put_u8   (struct bs_buf *b, unsigned int v);
void bs_put_u16  (struct bs_buf *b, unsigned int v);
void bs_put_u32  (struct bs_buf *b, unsigned long v);
void bs_put_u64  (struct bs_buf *b, unsigned long long v);
void bs_put_bytes(struct bs_buf *b, const unsigned char *s, size_t n);
void bs_put_zero (struct bs_buf *b, size_t n);   /* padding, of which the ABI has plenty */

int    bs_buf_ok (const struct bs_buf *b);
size_t bs_buf_len(const struct bs_buf *b);

#endif
