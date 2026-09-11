/* broker.h — the loop, and the only place the wire is read or written. */
#ifndef BS_BROKER_H
#define BS_BROKER_H

#include "brainstem.h"

struct bs_opts {
    const char *interp;
    const char *prog;
    long hello_timeout_ms;   /* 0 disables */
    long op_timeout_ms;      /* 0 disables */
    int  trace;              /* print every frame to stderr */
};

/* Run one program to completion. Returns the process exit code brainstem
 * should use -- see the BS_EXIT_* constants for what each means. */
int bs_broker_run(const struct bs_opts *o);

#endif
