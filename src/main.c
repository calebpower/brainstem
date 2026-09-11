/* main.c — argument handling, and nothing else.
 *
 * Manual argv walking rather than getopt, matching the sibling's tools: the
 * option set is small, the parsing is obvious on the page, and getopt's
 * permutation behaviour differs between platforms in ways that would be one
 * more thing the platform parity tier had to account for.
 *
 * Usage:
 *   brainstem [options] -- INTERPRETER PROGRAM.bf
 *
 *   --interp PATH          the interpreter, if not given after --
 *   --hello-timeout MS     default 5000; 0 disables
 *   --op-timeout MS        default 30000; 0 disables
 *   --trace                print every frame to stderr
 *   --check-interpreter P  probe P for the one property the protocol needs
 *   --dump-abi             print the op table, for tools/bsabi
 *   --selftest             the broker's own checks
 *
 * Exit: see the BS_EXIT_* constants in brainstem.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/wait.h>

#include "brainstem.h"
#include "broker.h"
#include "ops.h"

static void usage(const char *me) {
    fprintf(stderr,
        "usage: %s [options] -- INTERPRETER PROGRAM.bf\n"
        "       %s --check-interpreter PATH\n"
        "       %s --dump-abi | --selftest\n", me, me, me);
}

/* --dump-abi: one row per line, for tools that check the table against
 * ABI.md. Deliberately terse and stable; it is an interface. */
static int dump_abi(void) {
    size_t i;
    if (bs_ops_init() != 0) return BS_EXIT_USAGE;
    for (i = 0; i < bs_op_count(); i++) {
        const struct bs_op *o = bs_op_row(i);
        printf("op %02x %s %s req=%u rep=%u\n",
               o->code, o->name, o->fn ? "built" : "declared",
               o->reqlen, o->replen);
    }
    return BS_EXIT_OK;
}

/* --check-interpreter: probe the one property the protocol requires and
 * brainfuck does not mention, ABI.md requirement I1 -- a byte written by '.'
 * must reach the pipe before the program next blocks on ','.
 *
 * The probe is a program that writes one byte and then loops forever. If the
 * byte arrives, output is unbuffered. If nothing arrives within the window,
 * it is sitting in a stdio buffer and this interpreter will deadlock every
 * conversation it is ever given.
 *
 * This exists because the failure it detects has no other symptom. A
 * buffering interpreter does not error; it hangs, silently, with no output
 * and no core, on the very first frame. */
static int check_interpreter(const char *interp) {
    int to_c[2], from_c[2];
    pid_t pid;
    struct pollfd pf;
    int r, ok = 0;
    /* emit 0x41 then spin: 8*8=64, +1 = 65, '.', then a loop that never ends */
    static const char probe[] = "++++++++[>++++++++<-]>+.[]";
    char path[] = "/tmp/bs-probe-XXXXXX";
    int fd = mkstemp(path);

    if (fd < 0) { fprintf(stderr, "brainstem: cannot write a probe program\n"); return BS_EXIT_INTERP; }
    if (write(fd, probe, sizeof probe - 1) != (ssize_t)(sizeof probe - 1)) {
        close(fd); unlink(path); return BS_EXIT_INTERP;
    }
    close(fd);

    if (pipe(to_c) != 0 || pipe(from_c) != 0) { unlink(path); return BS_EXIT_INTERP; }
    pid = fork();
    if (pid < 0) { unlink(path); return BS_EXIT_INTERP; }
    if (pid == 0) {
        dup2(to_c[0], 0); dup2(from_c[1], 1);
        close(to_c[0]); close(to_c[1]); close(from_c[0]); close(from_c[1]);
        execlp(interp, interp, path, (char *)0);
        _exit(127);
    }
    close(to_c[0]); close(from_c[1]);

    pf.fd = from_c[0]; pf.events = POLLIN; pf.revents = 0;
    do { r = poll(&pf, 1, 2000); } while (r < 0 && errno == EINTR);
    if (r > 0) {
        unsigned char b;
        if (read(from_c[0], &b, 1) == 1 && b == 0x41) ok = 1;
    }

    close(to_c[1]); close(from_c[0]);
    kill(pid, SIGKILL);
    { int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { } }
    unlink(path);

    if (ok) {
        printf("brainstem: %s delivers a byte before blocking; it can be brokered\n", interp);
        return BS_EXIT_OK;
    }
    fprintf(stderr,
        "brainstem: %s did NOT deliver a byte before blocking.\n"
        "brainstem: it is buffering its stdout, and every conversation with it\n"
        "brainstem: would deadlock on the first frame with no output and no core.\n"
        "brainstem: fix it with setvbuf(stdout, NULL, _IONBF, 0), or try stdbuf -o0.\n",
        interp);
    return BS_EXIT_INTERP;
}

/* The broker's own checks: the op table must be well formed. The codec has
 * its own self test in tools/bscodec, because it can be tested without any
 * of this. */
static int selftest(void) {
    size_t i, built = 0;
    if (bs_ops_init() != 0) { printf("SELFTEST FAIL: the op table is malformed\n"); return 1; }
    printf("selftest ok: the op table has no duplicate or zero opcode\n");

    for (i = 0; i < bs_op_count(); i++) if (bs_op_row(i)->fn) built++;
    if (bs_op_count() != 23) { printf("SELFTEST FAIL: %u ops, expected 23\n", (unsigned)bs_op_count()); return 1; }
    printf("selftest ok: twenty three ops declared, %u built\n", (unsigned)built);

    if (bs_op_lookup(0x00)) { printf("SELFTEST FAIL: opcode 0 resolves\n"); return 1; }
    printf("selftest ok: opcode 0 resolves to nothing\n");

    if (!bs_op_lookup(BS_OP_HELLO) || !bs_op_lookup(BS_OP_HELLO)->fn) {
        printf("SELFTEST FAIL: hello is not built\n"); return 1;
    }
    {   /* an arity that the table says is exact must be enforced as exact */
        const struct bs_op *h = bs_op_lookup(BS_OP_HELLO);
        if (bs_op_arity_ok(h, 9) || bs_op_arity_ok(h, 11) || !bs_op_arity_ok(h, 10)) {
            printf("SELFTEST FAIL: exact arity is not enforced\n"); return 1;
        }
        printf("selftest ok: an exact arity accepts only its length\n");
    }
    printf("brainstem --selftest: ok\n");
    return 0;
}

int main(int argc, char **argv) {
    struct bs_opts o;
    int i;

    o.interp = 0;
    o.prog = 0;
    o.hello_timeout_ms = BS_HELLO_TIMEOUT_MS;
    o.op_timeout_ms    = BS_OP_TIMEOUT_MS;
    o.trace = 0;

    /* EPIPE is handled as a return value where it happens. A dead peer must
     * not kill the broker before it can say what went wrong. */
    signal(SIGPIPE, SIG_IGN);

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--selftest") == 0) return selftest();
        if (strcmp(a, "--dump-abi") == 0) return dump_abi();
        if (strcmp(a, "--check-interpreter") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return BS_EXIT_USAGE; }
            return check_interpreter(argv[i + 1]);
        }
        if (strcmp(a, "--trace") == 0) { o.trace = 1; continue; }
        if (strcmp(a, "--interp") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return BS_EXIT_USAGE; }
            o.interp = argv[++i]; continue;
        }
        if (strcmp(a, "--hello-timeout") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return BS_EXIT_USAGE; }
            o.hello_timeout_ms = strtol(argv[++i], 0, 10); continue;
        }
        if (strcmp(a, "--op-timeout") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return BS_EXIT_USAGE; }
            o.op_timeout_ms = strtol(argv[++i], 0, 10); continue;
        }
        if (strcmp(a, "--") == 0) {
            /* everything after -- is the command that runs the program */
            if (i + 1 < argc && !o.interp) o.interp = argv[++i];
            else if (i + 1 < argc) i++;
            if (i + 1 < argc) o.prog = argv[++i];
            continue;
        }
        if (a[0] == '-') { usage(argv[0]); return BS_EXIT_USAGE; }
        /* bare words: the interpreter then the program */
        if (!o.interp) o.interp = a;
        else if (!o.prog) o.prog = a;
        else { usage(argv[0]); return BS_EXIT_USAGE; }
    }

    if (!o.interp || !o.prog) { usage(argv[0]); return BS_EXIT_USAGE; }
    return bs_broker_run(&o);
}
