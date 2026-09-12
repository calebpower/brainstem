/* path.c — see path.h. Pure inspection: no I/O, no allocation, no syscall. */
#include "path.h"

bs_err bs_path_check(const unsigned char *p, size_t n) {
    size_t i, seg;

    if (n == 0) return BS_INVAL;
    for (i = 0; i < n; i++) if (p[i] == '\0') return BS_INVAL;
    if (p[0] == '/') return BS_INVAL;

    /* Walk the components. ".." is DENIED rather than INVAL: the path is well
     * formed and the broker is refusing it, which is a different thing from
     * the program having sent nonsense, and a program that can tell them
     * apart can report something useful. */
    seg = 0;
    for (i = 0; i <= n; i++) {
        if (i == n || p[i] == '/') {
            if (i - seg == 2 && p[seg] == '.' && p[seg + 1] == '.') return BS_DENIED;
            seg = i + 1;
        }
    }
    return BS_OK;
}
