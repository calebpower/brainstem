# brainstem — handoff

Written for whoever picks this up next. It assumes you have read `README.md`
and `CONVENTIONS.md`; this is the part those do not say, which is how the thing
is actually built and where it has already bitten.

## State

**Milestone M0. The suite is 22 pass, 0 fail on the Linux half. The FreeBSD
half has never been run.**

There is no broker. `src/` is empty. What exists is the ground: two lanes, one
toolchain definition, one build definition, the vendored interpreter, the five
documents, and a suite that proves those things about themselves.

That ordering was deliberate — the first commit has to be green on the machine
of record, and you cannot claim that without the lane that runs it. It also
front-loads the discovery that `ubuntu-26.04` ships neither `gcc` nor `make`,
which is a bad thing to find during the first interesting milestone.

**The FreeBSD half is the honest gap in this commit.** reaper was unreachable
from the development host — a Windows box on a separate network — so
`guest-setup.sh`'s FreeBSD branch, the `ktrace`/`kdump` presence check, and
every `uname`-dependent path have been written but never executed. Whoever
first runs `reaper test` should treat a failure on `freebsd-15.1` as this
commit's bug rather than theirs.

## Why this project exists, since the name is not obvious

brainstem is **P1** of three. P2 is [bfsodium](https://github.com/calebpower/bfsodium),
the pure-computation crypto library, and P3 is importing bfsodium into BoneMesh
as a gated tier. The only written trace of that scheme anywhere was one line in
bfsodium's rulebook:

> bfsodium is pure computation: **plain stdin/stdout, no syscalls, no P1 broker.**

That sentence is a *boundary between two planned components*, not the absence
of one, and it was read as the latter for a while. The vocabulary and the
design had to be recovered from a previous session's recollection, which is
most of the reason this section exists. Write the phase down.

The near-term payoff is `spawn`: once a brainfuck program can drive another
brainfuck program, **brainfuck itself becomes the harness**, and the BoneMesh
keyschedule conformance check — nine sequenced calls through bfsodium's
SHA-256, HKDF and ChaCha20-Poly1305 — becomes something a `.bf` can do.

## The defect that shaped everything

`bfsodium/tools/bfi.c` calls `putchar()` with no `setvbuf` and `fflush()`es
once, after the program has ended. Over a pipe that is full buffering: **no
request reaches the broker until the program terminates**, by which time the
program is blocked on `,` awaiting a reply that cannot come. Silent, total
deadlock whose only symptom is a hang with no output.

Found by reading, before a line of broker existed, which is the only reason it
is not still ahead of us.

Three consequences, all of them now load-bearing:

1. `tools/bfi.c` here is vendored **with the fix**, documented in its header as
   the single intentional delta. The same one-liner is proposed upstream
   separately; when it lands the copies match again.
2. It is promoted to an ABI requirement rather than left as folklore, and the
   broker will diagnose a violation (`--check-interpreter`, `--hello-timeout`)
   instead of hanging behind one.
3. `BFI_FLUSH=block` puts the defect **back** on demand, because a deadlock the
   suite cannot reproduce is a deadlock that comes back.

A pty is not a fix and the suite should never grow one: line buffering flushes
on `0x0a`, which here is an ordinary payload byte, and termios would translate
it anyway.

## Running the suite

There is **one gate and it has two halves**. `reaper test` runs `freebsd-15.1`
and `ubuntu-26.04`, and a change is not judged until both are green.

    reaper up && reaper test        # the gate, both platforms

There are **two fallbacks and each covers one half**:

    sh tests/run.sh                 # the FreeBSD half, on a FreeBSD host
    sh tools/container-test.sh      # the Linux half, anywhere with podman

Neither substitutes for the other. On a FreeBSD host the native run *is* the
gate's own procedure, because the tenant is `exec = "host"`. podman on FreeBSD
runs jails, not Linux containers, so there is no container lane that could
cover the primary target, and the container lane covers the platform that
matters less. **A green container run against a change touching the seam has
proved approximately nothing.**

Because prose does not fail builds, the suite's summary names the platform it
ran on and `container-test.sh` says which half it just ran. Commit trailers
should name both guests; a commit that can only name one should say which is
missing and why.

## How the lanes are wired

`tools/guest-setup.sh` is **the one definition of the toolchain** and both
lanes run it. reaper calls it with no argument; the `Containerfile` calls it
with `--toolchain` so the slow half is a cached layer; `container-test.sh`
calls it with `--build`. If the Containerfile ever grows its own package line
there are two definitions, and the fallback begins passing what the gate would
fail — silently, since a container with a different compiler still runs every
test and still says PASS. `tests/run.sh` checks for that.

`tools/build.sh` is **the one definition of the build**, and nothing else in
the tree invokes a compiler. This is a check bfsodium does not have and
arguably needs: it carries five `cc` lines in `tests/run.sh` and five more in
`tools/guest-setup.sh`, which is exactly the shape of defect its own
Containerfile check exists to forbid, one level down.

The guest-to-uname correspondence is **declared, not inferred** — see the
`# GUEST` markers in `guest-setup.sh`. reaper says `ubuntu-26.04` and `uname`
says `Linux`; those vocabularies do not line up on their own. The suite proves
the declared set matches `.reaper.toml` exactly and that every declared OS has
a branch. Without it, adding a guest silently takes whichever branch matches by
accident, and the tree then claims coverage of a platform nobody provisioned
for.

## Traps that have actually bitten

- **A check that forbids a token will trip over the comment explaining the
  ban.** The first version of "the container lane installs nothing of its own"
  grepped the whole `Containerfile` for `apt-get` and failed, because the
  comment saying *there is deliberately no apt-get line here* contains the
  words. The fix is to strip comments and check what the file **does**, not
  what it says — a check that punishes its own documentation teaches the next
  person to delete the documentation.
- **MSYS rewrites container arguments on a Windows host.** `-v $repo:/src:ro`
  arrives as `\Program Files\Git\src;ro` because MSYS reads the whole argument
  as a POSIX path list. Turning the rewriting off is necessary and not
  sufficient: podman then reads the *host* half and the build context as
  Windows paths and resolves `/d/projects` to `D:\d\projects`. The halves must
  be handled separately. This cost an afternoon in the sibling and the fix is
  carried here already.
- **`-w /work` is refused by a podman 4.5 client against a 4.9 server**, for a
  directory that exists and is the image's own `WORKDIR`. Upgrading the client
  fixes it; the flag is redundant anyway and the `cd` in the body says the same
  thing where no engine can misread it.
- **`-std=c99` hides different things on each platform.** On glibc it hides
  `mkstemp` and `fdopen`; on FreeBSD it sets `__STRICT_ANSI__` and hides
  `arc4random_buf` and most of `<sys/socket.h>`. That is why the feature macros
  live in `tools/build.sh` and why only the seam files may widen them further.
- **A `.bf` is never hand-edited.** The suite regenerates every one from its
  skeleton and compares byte for byte. If you find yourself editing a `.bf`,
  you are editing the wrong file.
- **`sh` has no locals.** A helper using `$i` silently truncated a caller's
  loop in the sibling. Prefix helper variables; `guest-setup.sh` uses `_gs_`.

## Known soft spots

- **Two copies of the interpreter can drift.** `tools/bfi.c` is vendored from
  bfsodium at commit `e98794f` with three documented deltas, and **no check
  inside this repository can detect divergence from upstream.** The mitigation
  is a periodic manual diff. The suite does pin the one delta that matters — if
  someone tidies the `setvbuf` line away, a check fails rather than a hang
  appearing. `tools/hx.c` is verbatim and can be diffed directly.
- **The FreeBSD lane is unexecuted**, as above.
- **Every tier past 1 and 10c is declared and absent.** That is expected at M0
  and it is written into `CONVENTIONS.md` §8 rather than left implicit, but do
  not let the declaration pass as coverage.

## What is next

1. **M1, the frame codec alone.** No process, no descriptor, no platform —
   nothing in it can fail for a platform reason, which is exactly why it comes
   before the seam. It also gets `bsframe`, the independently written second
   encoder, which every later tier leans on.
2. **M2 is the milestone that matters.** A standard brainfuck program
   completing a round trip through a real interpreter over real pipes either
   validates the whole idea or kills it. It uses only the three `ctl` ops,
   which do not cross the seam, so it settles deadlock, timeout and the
   buffering diagnosis at zero platform cost **before twenty more ops inherit
   them**.
3. **Then the seam, with time and rand first**, because `arc4random_buf`
   against `getrandom` is a real divergence and the seam should take its shape
   from one rather than from a hypothesis.
4. The remaining op families, `proc` last because it is the hairiest: fd
   inheritance, zombies, and `SIGCHLD` racing the broker's own reaping of the
   interpreter.

## Decisions worth not relitigating

Each of these looked like a close call and is not.

- **POSIX/libc, not hand-written syscall wrappers.** On FreeBSD the stable
  contract is libc; the raw syscall table is explicitly not guaranteed across
  major versions. Wrappers would have put the weaker engineering on the primary
  platform to serve an audit that only runs on the secondary one. What replaces
  them is stronger: per-object symbol allowlists plus pinned per-op syscall
  multisets, which measure the binary rather than trusting the source and work
  on both platforms.
- **Capsicum cannot do the purity proof.** It confines rather than observes,
  has no supervisor channel, and its capability mode denies exactly the global
  namespaces a broker exists to use. The proof wants a *tracer* — `ktrace` or
  DTrace on FreeBSD, `strace` or seccomp-notify on Linux — which is portable in
  a way seccomp-notify alone never was.
- **Little-endian**, matching bfsodium's `len{2} LE` convention across 33
  primitives, over the legibility argument for big-endian in hand-written hex.
  A program should never hold two byte orders in its head.
- **One `readdir` entry per call.** A batch means cursor arithmetic over
  variable-length records, which is the one thing brainfuck cannot do cheaply.
  The extra round trip is microseconds against an interpreter spending 10⁶–10⁹
  instructions per call. There is no trade-off here, only an apparent one.
- **The fixture expander must stay ignorant of the ABI.** If fixtures were
  generated from the op table, a byte-order bug would be invisible to every
  test in the suite.
