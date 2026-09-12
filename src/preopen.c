/* preopen.c — turning command line words into the program's whole world.
 *
 * NOTHING IS REACHABLE THAT WAS NOT NAMED HERE. There are no absolute paths
 * in the ABI and no op that opens one, so the set of preopens is an upper
 * bound on everything the program can ever touch. ABI.md section 8 is the
 * normative text and is careful about what that does and does not claim: it
 * is a usability and determinism property, not a containment boundary. The
 * broker runs with its operator's credentials and makes no sandbox claim.
 *
 * Specs are COLLECTED during argument parsing and INSTALLED once, from the
 * broker, immediately before the child is started. The split matters: argv is
 * parsed before anything has been opened, so a typo in the fifth preopen is
 * reported before the first one has had any effect on the filesystem.
 */
#include <string.h>
#include <stdlib.h>

#include "brainstem.h"
#include "preopen.h"
#include "fdtab.h"

struct spec {
    int   kind;            /* BS_PRE_* */
    const char *name;      /* into argv, which outlives everything here */
    const char *arg;       /* into argv */
};

static struct spec specs[BS_HANDLES];
static size_t nspecs;

/* argv strings are NUL terminated and live for the life of the process, so
 * nothing here copies. What it does need is to split NAME=REST without
 * writing into argv, which is why the pieces travel as pointer plus length
 * and the path gets copied into a fixed buffer at install time. */
bs_err bs_preopen_add(int kind, const char *word) {
    const char *eq;
    if (nspecs >= BS_HANDLES) return BS_EXHAUSTED;
    if (!word) return BS_INVAL;
    eq = strchr(word, '=');
    if (!eq || eq == word || !eq[1]) return BS_INVAL;
    specs[nspecs].kind = kind;
    specs[nspecs].name = word;      /* name is word[0 .. eq-word) */
    specs[nspecs].arg  = eq + 1;
    nspecs++;
    return BS_OK;
}

size_t bs_preopen_count(void) { return nspecs; }

/* The name, which is the part before the '='. Returned as a pointer and a
 * length rather than a copy: the hello reply writes it straight onto the
 * wire, and there is no allocation on this path any more than on any other. */
static const char *spec_name(size_t i, size_t *len) {
    const char *eq = strchr(specs[i].name, '=');
    *len = (size_t)(eq - specs[i].name);
    return specs[i].name;
}

/* r, w, rw, a. Anything else is a usage error rather than a default, because
 * a preopen quietly opened read-only is a program that fails much later with
 * a status that points at the wrong thing. */
static bs_err mode_rights(const char *m, bs_u32 *oflags, bs_u16 *rights) {
    if (strcmp(m, "r") == 0)  { *oflags = BS_O_READ;               *rights = BS_R_READ | BS_R_SEEK; return BS_OK; }
    if (strcmp(m, "w") == 0)  { *oflags = BS_O_WRITE | BS_O_CREATE | BS_O_TRUNC;
                                *rights = BS_R_WRITE | BS_R_SEEK; return BS_OK; }
    if (strcmp(m, "rw") == 0) { *oflags = BS_O_READ | BS_O_WRITE | BS_O_CREATE;
                                *rights = BS_R_READ | BS_R_WRITE | BS_R_SEEK; return BS_OK; }
    if (strcmp(m, "a") == 0)  { *oflags = BS_O_WRITE | BS_O_CREATE | BS_O_APPEND;
                                *rights = BS_R_WRITE; return BS_OK; }
    return BS_INVAL;
}

static bs_err install_one(size_t i, bs_u32 *handle) {
    char path[BS_PATH_MAX];
    const char *arg = specs[i].arg;
    bs_osfd fd = BS_OSFD_NONE;
    bs_u32 oflags = BS_O_READ;
    bs_u16 rights = 0;
    bs_u8  kind   = BS_HK_FILE;
    bs_err e;

    switch (specs[i].kind) {
    case BS_PRE_DIR:
        if (strlen(arg) >= sizeof path) return BS_NAMETOOLONG;
        e = sys_open_host(arg, BS_O_READ | BS_O_DIRECTORY, &fd);
        if (e != BS_OK) return e;
        kind   = BS_HK_DIR;
        /* A directory preopen carries everything a directory can do, EXEC
         * included, so spawn can run a program out of it. Narrower sets are
         * useful and are what --preopen-file is for; splitting a directory's
         * rights further needs a syntax that should arrive with the
         * capability work at M7 and M8, alongside the answer to the ambient
         * network question in ABI.md section 8.1. Until then EXEC is granted
         * with the rest and the right narrows only through derived handles --
         * which is honest, and the same posture the network already has. */
        rights = BS_R_READ | BS_R_WRITE | BS_R_SEEK | BS_R_CREATE | BS_R_DELETE
               | BS_R_LIST | BS_R_EXEC;
        break;

    case BS_PRE_FILE: {
        const char *colon = strrchr(arg, ':');
        size_t plen;
        if (!colon || !colon[1]) return BS_INVAL;
        plen = (size_t)(colon - arg);
        if (plen == 0 || plen >= sizeof path) return BS_NAMETOOLONG;
        memcpy(path, arg, plen);
        path[plen] = '\0';
        e = mode_rights(colon + 1, &oflags, &rights);
        if (e != BS_OK) return e;
        e = sys_open_host(path, oflags, &fd);
        if (e != BS_OK) return e;
        kind = BS_HK_FILE;
        break;
    }

    case BS_PRE_FD: {
        char *end;
        long n = strtol(arg, &end, 10);
        bs_stat st;
        bs_u8 r = 0, w = 0;
        if (*arg == '\0' || *end != '\0' || n < 0 || n > 0xFFFF) return BS_INVAL;
        /* A COPY, not the descriptor itself. The program may close its
         * handle, and closing the broker's own stderr on its behalf would
         * take away the one channel that reports what went wrong. */
        e = sys_dup((bs_osfd)n, &fd);
        if (e != BS_OK) return e;
        e = sys_stat(fd, 0, 0, &st);
        if (e != BS_OK) { sys_close(fd); return e; }
        e = sys_fd_mode(fd, &r, &w);
        if (e != BS_OK) { sys_close(fd); return e; }
        rights = (bs_u16)((r ? BS_R_READ : 0) | (w ? BS_R_WRITE : 0));
        switch (st.type) {
        case BS_FT_CHR:  kind = BS_HK_TTY;  rights |= 0; break;
        case BS_FT_DIR:  kind = BS_HK_DIR;  rights |= BS_R_LIST | BS_R_SEEK; break;
        case BS_FT_FIFO: kind = w ? BS_HK_PIPE_W : BS_HK_PIPE_R; break;
        case BS_FT_SOCK: kind = BS_HK_SOCKET; break;
        default:         kind = BS_HK_FILE; rights |= BS_R_SEEK; break;
        }
        break;
    }

    default:
        return BS_INVAL;
    }

    e = bs_fdtab_alloc(kind, rights, fd, handle);
    if (e != BS_OK) { sys_close(fd); return e; }
    return BS_OK;
}

bs_err bs_preopen_install(size_t *failed_at) {
    size_t i;
    for (i = 0; i < nspecs; i++) {
        bs_u32 h;
        size_t namelen;
        const char *name = spec_name(i, &namelen);
        bs_err e = install_one(i, &h);
        if (e != BS_OK) { *failed_at = i; return e; }
        /* The name is recorded pointing into argv rather than copied, and the
         * hello reply writes namelen bytes from it. It is not NUL terminated
         * at that length -- the '=' follows -- which is why everything here
         * carries a length. */
        bs_fdtab_name(h, name);
        {
            struct bs_slot *s = bs_fdtab_get(h);
            if (s) s->namelen = (bs_u8)(namelen > 255 ? 255 : namelen);
        }
    }
    return BS_OK;
}

const char *bs_preopen_word(size_t i) { return (i < nspecs) ? specs[i].name : 0; }
