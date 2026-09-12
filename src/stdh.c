/* stdh.c — see stdh.h. */
#include "stdh.h"
#include "fdtab.h"

static const struct { int fd; const char *name; } std3[] = {
    { 0, "stdin"  },
    { 1, "stdout" },
    { 2, "stderr" }
};

bs_err bs_stdh_install(int *failed_fd) {
    size_t i;
    for (i = 0; i < sizeof std3 / sizeof std3[0]; i++) {
        bs_osfd fd;
        bs_stat st;
        bs_u8 r = 0, w = 0;
        bs_u16 rights;
        bs_u8 kind;
        bs_u32 h;
        bs_err e;

        *failed_fd = std3[i].fd;

        /* A COPY, not the descriptor itself. The program may close its
         * handle, and closing the broker's own stderr on its behalf would
         * take away the one channel that reports what went wrong. */
        e = sys_dup((bs_osfd)std3[i].fd, &fd);
        if (e != BS_OK) return e;
        e = sys_stat(fd, 0, 0, &st);
        if (e != BS_OK) { sys_close(fd); return e; }
        e = sys_fd_mode(fd, &r, &w);
        if (e != BS_OK) { sys_close(fd); return e; }

        /* The kind is worked out rather than declared, because what fd 1 IS
         * depends entirely on how the broker was invoked: a terminal, a file,
         * a pipe into another program. A program that cares can read it out
         * of the hello table; one that does not can just write. */
        rights = (bs_u16)((r ? BS_R_READ : 0) | (w ? BS_R_WRITE : 0));
        switch (st.type) {
        case BS_FT_CHR:  kind = BS_HK_TTY; break;
        case BS_FT_FIFO: kind = w ? BS_HK_PIPE_W : BS_HK_PIPE_R; break;
        case BS_FT_SOCK: kind = BS_HK_SOCKET; break;
        case BS_FT_DIR:  kind = BS_HK_DIR;
                         rights = (bs_u16)(rights | BS_R_LIST | BS_R_SEEK); break;
        default:         kind = BS_HK_FILE;
                         rights = (bs_u16)(rights | BS_R_SEEK); break;
        }

        e = bs_fdtab_alloc(kind, rights, fd, &h);
        if (e != BS_OK) { sys_close(fd); return e; }
        bs_fdtab_name(h, std3[i].name);
    }
    return BS_OK;
}
