/* err.h — the status codes, built from errs.def.
 *
 * Every function that can fail returns a bs_err, never -1 and never errno.
 * There is exactly one place errno is read (the platform seam, from M3), and
 * exactly one table that maps it here.
 */
#ifndef BS_ERR_H
#define BS_ERR_H

typedef int bs_err;

#define X(v, n, t) BS_##n = (v),
enum {
#include "errs.def"
    BS_ERR_SENTINEL_UNUSED = -1
};
#undef X

/* The name of a status, for diagnostics on the broker's own stderr. Never
 * for the wire: the wire carries the byte. Returns "?" for a value that is
 * not a declared status, which is itself worth seeing in a diagnostic. */
const char *bs_err_name(bs_err e);

/* The one-line human text. Same rules. */
const char *bs_err_text(bs_err e);

/* Is this status fatal to the conversation? The 0xE0 range is exactly the
 * protocol errors, which is why they were put there: a program can test the
 * range rather than carry a table, and so can the broker. */
int bs_err_fatal(bs_err e);

#endif
