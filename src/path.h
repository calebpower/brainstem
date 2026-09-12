/* path.h — the one statement of what a path in this ABI may be.
 *
 * op_fs and op_proc both turn payload bytes into a path, so the rule gets one
 * definition. It is a short rule: a path may not be empty and may not contain
 * a NUL. Absolute paths are fine and so is "..".
 *
 * See path.c for why that is the whole list, and ABI.md section 8 for what
 * the project does and does not claim about reachability.
 */
#ifndef BS_PATH_H
#define BS_PATH_H

#include "sys.h"

/* BS_OK, or the status the op should answer with. */
bs_err bs_path_check(const unsigned char *p, size_t n);

#endif
