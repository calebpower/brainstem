/* fdtab.c — the handle table. See fdtab.h for why handles are not fds.
 *
 * No allocation, no I/O beyond closing what it was given, and no knowledge of
 * the wire. It is a fixed array and a little arithmetic, which is what lets
 * the audit tier say this unit reaches outside itself for nothing at all.
 */
#include "fdtab.h"

static struct bs_slot tab[BS_HANDLES];

void bs_fdtab_init(void) {
    size_t i;
    for (i = 0; i < BS_HANDLES; i++) {
        tab[i].used    = 0;
        tab[i].gen     = 0;
        tab[i].kind    = BS_HK_NONE;
        tab[i].rights  = 0;
        tab[i].fd      = BS_OSFD_NONE;
        tab[i].dir     = 0;
        tab[i].name    = 0;
        tab[i].namelen = 0;
        tab[i].preopen = 0;
        tab[i].netstate = 0;
    }
}

/* index 0 is never allocated, so handle 0 is never issued */
static bs_u32 handle_of(size_t i) {
    return ((bs_u32)tab[i].gen << 16) | (bs_u32)i;
}

bs_err bs_fdtab_alloc(bs_u8 kind, bs_u16 rights, bs_osfd fd, bs_u32 *handle) {
    size_t i;
    /* Lowest free first, from 1. Deterministic by construction: the handle a
     * program gets depends only on the calls it has made, never on what the
     * kernel happened to hand back. */
    for (i = 1; i < BS_HANDLES; i++) {
        if (tab[i].used) continue;
        tab[i].used    = 1;
        tab[i].kind    = kind;
        tab[i].rights  = rights;
        tab[i].fd      = fd;
        tab[i].dir     = 0;
        tab[i].name    = 0;
        tab[i].namelen = 0;
        tab[i].preopen = 0;
        tab[i].netstate = 0;
        *handle = handle_of(i);
        return BS_OK;
    }
    return BS_EXHAUSTED;
}

void bs_fdtab_name(bs_u32 handle, const char *name) {
    struct bs_slot *s = bs_fdtab_get(handle);
    size_t n = 0;
    if (!s || !name) return;
    while (name[n] && n < 255) n++;
    s->name    = name;
    s->namelen = (bs_u8)n;
    s->preopen = 1;
}

struct bs_slot *bs_fdtab_get(bs_u32 handle) {
    size_t i = (size_t)(handle & 0xFFFFu);
    bs_u16 g = (bs_u16)(handle >> 16);
    if (i == 0 || i >= BS_HANDLES) return 0;
    if (!tab[i].used) return 0;
    /* The generation check. A handle whose slot has since been reused is
     * refused here, which is the whole reason this file exists. */
    if (tab[i].gen != g) return 0;
    return &tab[i];
}

struct bs_slot *bs_fdtab_get_kind(bs_u32 handle, bs_u8 kind) {
    struct bs_slot *s = bs_fdtab_get(handle);
    if (!s) return 0;
    if (s->kind != kind) return 0;
    return s;
}

bs_err bs_fdtab_free(bs_u32 handle) {
    struct bs_slot *s = bs_fdtab_get(handle);
    bs_err e = BS_OK, e2;
    size_t i;
    if (!s) return BS_BADF;

    /* A directory walk is closed first: it holds its own copy of the
     * descriptor, and closing them in the other order would leave a DIR
     * pointing at a descriptor the table has already released. */
    if (s->dir) {
        e2 = sys_dir_close(s->dir);
        if (e2 != BS_OK) e = e2;
        s->dir = 0;
    }
    if (s->fd != BS_OSFD_NONE) {
        e2 = sys_close(s->fd);
        if (e2 != BS_OK) e = e2;
    }

    i = (size_t)(handle & 0xFFFFu);
    tab[i].used    = 0;
    tab[i].kind    = BS_HK_NONE;
    tab[i].rights  = 0;
    tab[i].fd      = BS_OSFD_NONE;
    tab[i].name    = 0;
    tab[i].namelen = 0;
    tab[i].preopen = 0;
    tab[i].netstate = 0;
    /* The slot is free and its generation has moved on. Wrapping at 16 bits
     * is fine and is not a hole: a handle from 65536 closes ago is one the
     * program stopped being able to name long before the number came round,
     * and the alternative -- refusing to reuse the slot -- would turn a long
     * running program into one that runs out of handles. */
    tab[i].gen = (bs_u16)(tab[i].gen + 1);
    return e;
}

size_t bs_fdtab_count(void) { return BS_HANDLES; }
struct bs_slot *bs_fdtab_slot(size_t i) { return (i < BS_HANDLES) ? &tab[i] : 0; }
bs_u32 bs_fdtab_handle_of(size_t i) { return (i < BS_HANDLES) ? handle_of(i) : 0; }
