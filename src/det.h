/* det.h — the determinism knobs.
 *
 * brainstem introduces the two classic sources of nondeterminism the sibling
 * library was built to avoid. bfsodium's rule is "randomness supplied as
 * input, never generated -- which keeps every run deterministic and
 * replayable", and a syscall broker cannot keep that rule. So it keeps the
 * PROPERTY a different way: the sources stay, and both become steerable.
 *
 * The branch lives here and is read by exactly two ops. sys_clock_real and
 * sys_random stay pure platform calls that know nothing about any of this,
 * which is what lets the per-op syscall tier say "time.now issues one
 * clock_gettime" and mean it.
 */
#ifndef BS_DET_H
#define BS_DET_H

#include "sys.h"

enum { BS_CLOCK_LIVE = 0, BS_CLOCK_FROZEN = 1, BS_CLOCK_VIRTUAL = 2 };

/* --seed HEX, thirty two hex characters for sixteen bytes. BS_INVAL on
 * anything else, including the wrong length, because a seed that was
 * silently truncated would make two runs differ for a reason nobody could
 * see. */
bs_err det_set_seed(const char *hex);

/* --clock live | frozen[=EPOCH] | virtual[=EPOCH][,step=NS] */
bs_err det_set_clock(const char *spec);

/* --sort-readdir. The third source of nondeterminism, and the one that is
 * not a clock or a generator: a directory's ORDER is whatever the filesystem
 * feels like. ext4 hashes, ufs returns creation order, and neither is a
 * property of the program -- so a trace containing a directory walk cannot be
 * pinned on two platforms without this, which is exactly what it is for.
 *
 * It lives here rather than at the seam for the same reason the clock does:
 * the branch is a determinism policy, and sys_dir_next stays a pure platform
 * call that knows nothing about any of this. */
void det_set_sort_readdir(void);
int  det_sort_readdir(void);

int    det_rng_seeded(void);
int    det_clock_mode(void);
bs_u32 det_clock_step(void);
const bs_u8 *det_seed_bytes(void);   /* sixteen bytes, all zero when live */

/* Called once per REQUEST by the broker, not once per clock_now.
 *
 * Driving the virtual clock off the request count rather than off wall time
 * is what makes it deterministic: the request sequence is a property of the
 * program and of nothing else. It is also more useful than a frozen clock,
 * because elapsed time then correlates with work done -- and being strictly
 * monotonic it keeps "loop until the clock changes" terminating, which a
 * frozen clock does not.
 *
 * THE ONE LEAK, stated rather than hidden: a poll with a real timeout still
 * waits in real time, because I/O readiness cannot be virtualised. When such
 * a poll expires the clock advances by the timeout. A poll heavy program is
 * therefore not fully time deterministic, and ABI.md section 9 says so. */
void det_tick(void);

/* The two ops route through these rather than through sys.h directly. */
bs_err det_clock_real(bs_time *out);
bs_err det_clock_mono(bs_time *out);
bs_err det_random(bs_u8 *buf, unsigned int n);

#endif
