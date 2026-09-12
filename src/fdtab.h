/* fdtab.h — the handle table.
 *
 * A handle is a u32 on the wire and it is NOT an OS file descriptor:
 *
 *     handle = (generation << 16) | index
 *
 * THE DEFECT THIS EXISTS FOR. The program closes handle 3. The OS recycles
 * descriptor 3 for the next open, which the program also does. Now the
 * program's stale copy of handle 3 reads someone else's file, silently, and
 * every byte after that is wrong in a way no test would attribute to the
 * close. With a generation in the handle, that is BADF on the first use.
 *
 * It is also the indirection that lets a Windows SOCKET -- which is not an
 * int and not small -- sit behind the same four bytes later.
 *
 * Index 0 is never allocated, so handle 0 is never issued, which makes the
 * commonest brainfuck defect loud: a cell you forgot to fill is zero, and
 * zero is not a handle. Generations start at 0 so the FIRST handle is simply
 * its index -- the broker's stdin is handle 1 -- and only a reused slot
 * carries a generation the program has to echo back. That keeps the common case cheap
 * to emit and the dangerous case unforgeable.
 *
 * Allocation is LOWEST FREE FIRST and that is required rather than
 * incidental: it makes the handle sequence a deterministic function of the
 * program's own calls, which is what lets a trace be replayed and compared
 * byte for byte across the two platforms.
 */
#ifndef BS_FDTAB_H
#define BS_FDTAB_H

#include "sys.h"

/* Slots. 128 is not a resource limit anyone will reach with a brainfuck
 * program -- it is a bound that keeps the table a fixed array, because there
 * is no allocation on the ABI path (CONVENTIONS section 5). Running out is
 * EXHAUSTED, which is a status a program can act on. */
#define BS_HANDLES 128

/* "No handle", where an op accepts an absence. ABI.md section 5 has always
 * specified this value; since the preopen model was removed it is also how a
 * program says "resolve this path the way any other process would" -- against
 * the broker's own working directory, or absolutely. */
#define BS_HANDLE_NONE 0xFFFFFFFFu

/* Kinds. THESE ARE THE WIRE VALUES from ABI.md section 8.2, so the hello
 * reply's handle table can report them without a second mapping -- a second
 * mapping being how a kind comes to mean two things. */
#define BS_HK_NONE     0
#define BS_HK_FILE     1
#define BS_HK_DIR      2
#define BS_HK_PIPE_R   3
#define BS_HK_PIPE_W   4
#define BS_HK_LISTENER 5
#define BS_HK_SOCKET   6
#define BS_HK_TTY      7
/* Not a listable kind: a process is not something the broker can hand over
 * ready-made, so this one never appears in the hello reply's table and is
 * numbered after the ones that do. */
#define BS_HK_PROC     8

/* Rights, ABI.md section 8.2. RIGHTS ONLY EVER NARROW: a derived handle gets
 * its parent's rights intersected with what the operation asked for, so a
 * file opened read-only stays read-only however it is passed around. Nothing
 * in this file widens a right, and there is no op that grants one.
 *
 * That is a LEGIBILITY property and not a containment one, and since the
 * preopen model was removed it is nothing else: the program can open the same
 * path again with different flags whenever it likes. ABI.md section 8 says so
 * in those words. */
#define BS_R_READ    0x0001
#define BS_R_WRITE   0x0002
#define BS_R_SEEK    0x0004
#define BS_R_CREATE  0x0008
#define BS_R_DELETE  0x0010
#define BS_R_LIST    0x0020
#define BS_R_EXEC    0x0040
#define BS_R_ACCEPT  0x0080
#define BS_R_CONNECT 0x0100

struct bs_slot {
    int      used;
    bs_u16   gen;
    bs_u8    kind;
    bs_u16   rights;
    bs_osfd  fd;
    bs_osdir dir;          /* non-null only while a readdir walk is open */
    /* --sort-readdir: the name this walk returned last, which is the cursor
     * the next selection starts after. Empty means "before the first name".
     * Unused, and unread, when the flag is off. */
    char     sortcur[BS_NAME_MAX];
    const char *name;      /* the name reported in the hello table, or null */
    bs_u8    namelen;
    /* Appears in the hello reply's handle table. True for the three standard
     * handles and for nothing else, because they are the only handles a
     * program has before its first frame and so the only ones it could not
     * have learned about from a reply. This flag was called `preopen` until
     * the preopen model was removed; the field survived because the TABLE
     * did, and the name did not survive because it no longer described
     * anything. */
    int      listed;
    /* A non-blocking connect in flight. The seam reads and writes it, so the
     * mechanism stays at the seam and the STATE stays on the handle, where
     * the rest of a socket's identity already lives. It is what lets a
     * re-issued connect mean 'how did that go' with no getsockopt op in the
     * ABI at all. */
    int      netstate;

    /* A spawned child. The pid never reaches the wire -- pids differ between
     * runs and platforms and the wire must not -- and the reaped status is
     * cached here because the kernel will only report it once, while ABI.md
     * requires wait to be idempotent after reaping. */
    bs_i64   pid;
    int      reaped;
    bs_u8    pstate, pcode, psig;
};

void bs_fdtab_init(void);

/* Take a descriptor the seam has already produced. On BS_OK the table owns
 * the descriptor and will close it; on failure the caller still does, which
 * is the only ownership rule here and the only one worth stating. */
bs_err bs_fdtab_alloc(bs_u8 kind, bs_u16 rights, bs_osfd fd, bs_u32 *handle);

/* Record a handle's name for the hello reply, and list it there. Called only
 * during startup, by stdh.c. */
void bs_fdtab_name(bs_u32 handle, const char *name);

/* Null for a handle that was never issued, or was issued and closed, or
 * belongs to an older generation of a reused slot. All three are BADF to the
 * program and telling them apart would only help someone probing. */
struct bs_slot *bs_fdtab_get(bs_u32 handle);

/* The same, with a kind requirement, so accept on a regular file is refused
 * before the op runs rather than doing something stranger further down. */
struct bs_slot *bs_fdtab_get_kind(bs_u32 handle, bs_u8 kind);

/* Closes the descriptor and any open directory walk, frees the slot, and
 * increments its generation. Closing a standard handle is permitted and
 * permanent.
 * The generation bump is required for determinism, not an implementation
 * detail: without it a reused slot would hand back a handle the program
 * already holds. */
bs_err bs_fdtab_free(bs_u32 handle);

/* For the hello reply. Iteration is by slot index, so the order is the order
 * the handles were installed in, which is the only order the ABI promises. */
size_t bs_fdtab_count(void);
struct bs_slot *bs_fdtab_slot(size_t i);
bs_u32 bs_fdtab_handle_of(size_t i);

#endif
