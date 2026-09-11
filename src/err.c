/* err.c — the status name and text tables, built from errs.def.
 *
 * A switch rather than an array indexed by the code: the codes are sparse
 * (0x1A then 0x20, 0x31 then 0xE0) and a 256-entry array would be mostly
 * holes that say nothing about whether a code exists. The compiler turns a
 * dense switch into a jump table anyway, and a sparse one into a search,
 * which is the right shape for a lookup that happens only when something has
 * already gone wrong.
 */
#include "err.h"

const char *bs_err_name(bs_err e) {
    switch (e) {
#define X(v, n, t) case (v): return #n;
#include "errs.def"
#undef X
    default: return "?";
    }
}

const char *bs_err_text(bs_err e) {
    switch (e) {
#define X(v, n, t) case (v): return (t);
#include "errs.def"
#undef X
    default: return "not a declared status";
    }
}

int bs_err_fatal(bs_err e) {
    /* The protocol errors live in 0xE0:0xEF and nothing else does. Stated as
     * a range rather than a list on purpose -- a new fatal code added to
     * errs.def in that range is fatal without anyone remembering to update
     * this function, and a new recoverable code cannot land there by
     * accident because the range is documented as reserved. */
    return e >= 0xE0 && e <= 0xEF;
}
