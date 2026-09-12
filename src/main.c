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
 *   --seed HEX             32 hex characters; makes random_bytes deterministic
 *   --clock SPEC           live | frozen[=EPOCH] | virtual[=EPOCH][,step=NS]
 *   --sort-readdir         enumerate a directory in byte order of its names
 *   --replay FILE          answer every frame from a recorded --trace
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
#include <fcntl.h>

#include "brainstem.h"
#include "broker.h"
#include "ops.h"
#include "det.h"
#include "sys.h"
#include "replay.h"

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

/* --replay FILE: load a recording made with --trace.
 *
 * The file is read HERE rather than in replay.c, and before the interpreter
 * is started, for two reasons that are really one. main.c is the unit that
 * talks to the operator -- argv, and the probe program --check-interpreter
 * writes -- so a path from the command line is its business, and replay.c
 * stays a parser with no external surface at all. And loading before the
 * fork puts these reads outside the window tools/bscalls.sh measures, which
 * is what lets a replay's measured syscall surface be the empty set.
 *
 * Fed as raw bytes: a trace line can be 131 kilobytes, because a payload can
 * be 65535 bytes and --trace does not truncate, so the buffer that assembles
 * a line belongs with the parser and not here. */
static int load_replay(const char *path) {
    unsigned char chunk[4096];
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "brainstem: --replay: cannot read %s\n", path);
        return BS_EXIT_USAGE;
    }
    bs_replay_begin();
    for (;;) {
        ssize_t k = read(fd, chunk, sizeof chunk);
        if (k < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "brainstem: --replay: cannot read %s\n", path);
            close(fd);
            return BS_EXIT_USAGE;
        }
        if (k == 0) break;
        if (bs_replay_push(chunk, (size_t)k) != BS_OK) { close(fd); return BS_EXIT_USAGE; }
    }
    close(fd);
    if (bs_replay_end() != BS_OK) return BS_EXIT_USAGE;
    return BS_EXIT_OK;
}

/* The determinism knobs, checked against vectors computed OUTSIDE this
 * program.
 *
 * The all-zero-seed row is the published ChaCha20 test vector for a 32 byte
 * zero key, zero nonce and counter 0 -- which is exactly what a zero seed
 * expands to under the construction det.c documents. That is the row that
 * proves the construction is the one ABI.md section 9 describes and not
 * merely self-consistent: a transposed rotation or a wrong constant would
 * still agree with itself, and would not agree with this.
 *
 * The other two rows were produced by an independent implementation and are
 * here so the check has something to say about a seed that is not all zeros,
 * where a key-loading bug would hide.
 */
static int det_selftest(void) {
    static const struct { const char *seed; const char *hex; } vec[] = {
        { "00000000000000000000000000000000",
          "76b8e0ada0f13d90405d6ae55386bd28bdd219b8a08ded1aa836efcc8b770dc7"
          "da41597c5157488d7724e03fb8d84a376a43b8f41518a11cc387b669b2ee6586" },
        { "000102030405060708090a0b0c0d0e0f",
          "82233aa0ca0a14573efd34e9a85da6974427bd504b666b21640b9bcadbb23bc6" },
        { "deadbeefcafebabe0123456789abcdef",
          "52c78613402d35ad798554b19998529f4f9939586efec1cd41d75ac53edb8fea"
          "7dd149153f52ebc4" }
    };
    static const char *bad[] = {
        "", "0", "0000000000000000000000000000000",      /* 31: short */
        "000000000000000000000000000000000",             /* 33: long */
        "0000000000000000000000000000000g",              /* not hex */
        "0000000000000000 000000000000000"               /* a space is not hex */
    };
    unsigned char got[96];
    size_t i, j, n;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        if (det_set_seed(bad[i]) == BS_OK) {
            printf("SELFTEST FAIL: --seed accepted \"%s\"\n", bad[i]); return 1;
        }
    }
    if (det_set_seed(0) == BS_OK) { printf("SELFTEST FAIL: --seed accepted nothing\n"); return 1; }
    printf("selftest ok: a malformed seed is refused rather than padded\n");

    for (i = 0; i < sizeof vec / sizeof vec[0]; i++) {
        n = strlen(vec[i].hex) / 2;
        if (det_set_seed(vec[i].seed) != BS_OK) {
            printf("SELFTEST FAIL: seed %s refused\n", vec[i].seed); return 1;
        }
        if (!det_rng_seeded()) { printf("SELFTEST FAIL: seeded but not reported\n"); return 1; }
        if (det_random(got, (unsigned int)n) != BS_OK) {
            printf("SELFTEST FAIL: det_random failed\n"); return 1;
        }
        for (j = 0; j < n; j++) {
            unsigned int want;
            if (sscanf(vec[i].hex + 2 * j, "%2x", &want) != 1) return 1;
            if (got[j] != (unsigned char)want) {
                printf("SELFTEST FAIL: seed %s byte %u is %02x, expected %02x\n",
                       vec[i].seed, (unsigned)j, got[j], want);
                return 1;
            }
        }
    }
    printf("selftest ok: the seeded keystream matches three external vectors\n");

    /* The knob must not be inert. A generator that ignored the seed entirely
     * would pass none of the above -- but one that hashed the seed into a
     * fixed state would pass the first row and fail here, and that is a real
     * mistake somebody could make. */
    {
        unsigned char a[32], b[32];
        det_set_seed("00000000000000000000000000000001");
        det_random(a, 32);
        det_set_seed("00000000000000000000000000000002");
        det_random(b, 32);
        if (memcmp(a, b, 32) == 0) {
            printf("SELFTEST FAIL: two different seeds gave the same bytes\n"); return 1;
        }
        /* and re-seeding must REWIND, not continue: a replay depends on it */
        det_set_seed("00000000000000000000000000000001");
        det_random(b, 32);
        if (memcmp(a, b, 32) != 0) {
            printf("SELFTEST FAIL: the same seed did not repeat itself\n"); return 1;
        }
    }
    printf("selftest ok: the seed changes the bytes, and repeats them\n");

    {   /* the clock spec grammar, both polarities */
        static const char *ok[] = { "live", "frozen", "frozen=0", "frozen=1700000000",
                                    "virtual", "virtual=5", "virtual=5,step=1",
                                    "virtual,step=1000000" };
        static const char *no[] = { "", "LIVE", "fixed", "frozen=", "frozen ", "virtualx",
                                    "virtual,", "virtual,step", "virtual,nope=1" };
        for (i = 0; i < sizeof ok / sizeof ok[0]; i++)
            if (det_set_clock(ok[i]) != BS_OK) {
                printf("SELFTEST FAIL: --clock refused \"%s\"\n", ok[i]); return 1;
            }
        for (i = 0; i < sizeof no / sizeof no[0]; i++)
            if (det_set_clock(no[i]) == BS_OK) {
                printf("SELFTEST FAIL: --clock accepted \"%s\"\n", no[i]); return 1;
            }
        if (det_set_clock(0) == BS_OK) { printf("SELFTEST FAIL: --clock accepted nothing\n"); return 1; }
    }
    printf("selftest ok: the clock spec grammar accepts and refuses as written\n");

    {   /* a virtual clock must advance with requests and be strictly
         * monotonic, because "loop until the clock changes" has to terminate */
        bs_time t0, t1;
        det_set_clock("virtual=100,step=1000000");
        det_clock_real(&t0);
        det_tick();
        det_clock_real(&t1);
        if (!(t1.sec > t0.sec || (t1.sec == t0.sec && t1.nsec > t0.nsec))) {
            printf("SELFTEST FAIL: the virtual clock did not advance\n"); return 1;
        }
        det_set_clock("frozen=100");
        det_clock_real(&t0);
        det_tick();
        det_clock_real(&t1);
        if (t0.sec != 100 || t1.sec != 100 || t0.nsec != 0 || t1.nsec != 0) {
            printf("SELFTEST FAIL: the frozen clock moved\n"); return 1;
        }
        det_set_clock("live");
    }
    printf("selftest ok: virtual advances per request, frozen does not\n");

    /* The seam reports a platform this build knows. 0 would mean the seam
     * compiled but nobody claimed it. */
    if (sys_platform() == 0) { printf("SELFTEST FAIL: the seam reports no platform\n"); return 1; }
    printf("selftest ok: the seam reports platform %u\n", (unsigned)sys_platform());

    return 0;
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
    if (det_selftest() != 0) return 1;

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
        if (strcmp(a, "--sort-readdir") == 0) { det_set_sort_readdir(); continue; }
        if (strcmp(a, "--replay") == 0) {
            int rc;
            if (i + 1 >= argc) { usage(argv[0]); return BS_EXIT_USAGE; }
            rc = load_replay(argv[++i]);
            if (rc != BS_EXIT_OK) return rc;
            continue;
        }
        if (strcmp(a, "--seed") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return BS_EXIT_USAGE; }
            if (det_set_seed(argv[++i]) != BS_OK) {
                fprintf(stderr,
                    "brainstem: --seed takes exactly 32 hex characters.\n"
                    "brainstem: a short seed is refused rather than padded, because a\n"
                    "brainstem: silently truncated seed makes two runs differ for a\n"
                    "brainstem: reason nothing in the output could show.\n");
                return BS_EXIT_USAGE;
            }
            continue;
        }
        if (strcmp(a, "--clock") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return BS_EXIT_USAGE; }
            if (det_set_clock(argv[++i]) != BS_OK) {
                fprintf(stderr,
                    "brainstem: --clock takes live, frozen[=EPOCH], or\n"
                    "brainstem: virtual[=EPOCH][,step=NS].\n");
                return BS_EXIT_USAGE;
            }
            continue;
        }
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
