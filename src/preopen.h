/* preopen.h — the command line's contribution to the program's world.
 *
 * Specs are collected while argv is parsed and installed once, from the
 * broker, immediately before the child starts. Parsing everything before
 * opening anything means a typo in the fifth preopen is reported before the
 * first one has touched the filesystem.
 */
#ifndef BS_PREOPEN_H
#define BS_PREOPEN_H

#include "sys.h"

#define BS_PRE_DIR  1
#define BS_PRE_FILE 2
#define BS_PRE_FD   3

/* word is "NAME=REST", pointing into argv. */
bs_err bs_preopen_add(int kind, const char *word);

size_t bs_preopen_count(void);
const char *bs_preopen_word(size_t i);

/* Opens each spec in command line order, so handles are assigned in that
 * order starting at index 1 -- which is the only ordering the ABI promises
 * and the reason there is no name discovery op. On failure *failed_at is the
 * index of the spec that could not be opened, for a message that names the
 * word the operator typed rather than the ordinal of a loop. */
bs_err bs_preopen_install(size_t *failed_at);

#endif
