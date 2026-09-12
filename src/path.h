/* path.h — the one statement of what a path in this ABI may be.
 *
 * op_fs and op_proc both resolve a path beneath a directory handle, so the
 * rule that keeps a path beneath that directory has two callers. It gets one
 * definition. A security-relevant rule written down twice is a rule that will
 * be corrected once.
 */
#ifndef BS_PATH_H
#define BS_PATH_H

#include "sys.h"

/* Refuses, each with a defect behind it:
 *
 *   empty        an empty path is not the directory itself except where an op
 *                says so, and silently meaning "." would make a typo succeed
 *   embedded NUL the seam needs a C string, so a path with a NUL in it would
 *                reach the kernel truncated -- the classic way a check and a
 *                use come to disagree about the same bytes
 *   absolute     there are no absolute paths in this ABI at all
 *   ".."         the only relative component that can leave the directory,
 *                and refused ANYWHERE in the path rather than only at the
 *                front, because a/../../b climbs just as well
 *
 * The last two are the broker's own string check, standing in for kernel
 * confinement until M8. It is honest about its limit: a SYMLINK pointing
 * upward defeats it. ABI.md section 8.0 states that in a table rather than
 * implying otherwise, and sys_beneath_is_kernel() reports which mechanism is
 * actually in force.
 *
 * Returns BS_OK, or the status the op should answer with. */
bs_err bs_path_check(const unsigned char *p, size_t n);

#endif
