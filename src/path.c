/* path.c — see path.h. Pure inspection: no I/O, no allocation, no syscall. */
#include "path.h"

bs_err bs_path_check(const unsigned char *p, size_t n) {
    size_t i;

    /* Two refusals, and that is all there is.
     *
     * An EMPTY path is not the directory itself except where an op says so,
     * and silently meaning "." would make a typo succeed.
     *
     * An EMBEDDED NUL would reach the kernel truncated, because the seam
     * needs a C string -- the classic way a check and a use come to disagree
     * about the same bytes.
     *
     * ABSOLUTE PATHS AND ".." ARE BOTH FINE. An earlier version refused them,
     * on the theory that a program should only reach what the operator named
     * on the command line. That theory is gone: brainstem exists to make the
     * system VISIBLE to a brainfuck program, and a literal path is the
     * cheapest thing such a program can produce -- it is bytes to emit, and
     * emitting bytes is the one operation brainfuck is good at. A handle
     * index, by contrast, has to be agreed out of band between the operator
     * and a program that cannot compare strings.
     *
     * It was never a containment boundary either; CONVENTIONS has said
     * "brainstem is not a sandbox" since the first commit. Refusing ".." only
     * ever cost reachability and bought a claim the project explicitly did
     * not make. */
    if (n == 0) return BS_INVAL;
    for (i = 0; i < n; i++) if (p[i] == '\0') return BS_INVAL;
    return BS_OK;
}
