# brainstem — handoff

Written for whoever picks this up next. It assumes you have read `README.md`
and `CONVENTIONS.md`; this is the part those do not say, which is how the thing
is actually built and where it has already bitten.

## State

**Milestone M2 — the thesis holds.** A file containing nothing but the eight
brainfuck instructions, run under a general purpose interpreter, reaches an
operating system and comes back.

    > 01 len=10     hello
    < 00 len=48     OK, the 48 byte record
    > 02 len=1      exit 0
    < 00 len=0      OK

**Gated: 94 pass, 0 fail on `freebsd-15.1` and 94 pass, 0 fail on
`ubuntu-26.04`.** M0 was 22 on both.

Getting there cost three portability defects and they are recorded under
*Traps* below, because all three had the same shape and the next one will
too.

Two of the twenty three ops are built: `ctl.hello` and `ctl.exit`. The other
twenty one are declared in `src/ops.def` with a NULL handler and answer
NOSUCHOP, which is recoverable — the payload is consumed and the stream stays
in step, because that is the forward compatibility path for a program written
against a later minor version.

`ctl` was chosen for M2 precisely because it does not cross the platform
seam. The pipe topology, the half duplex discipline, the deadlock proof and
the buffering diagnosis are all settled now, once, with no platform surface
to confuse the result — and twenty one more ops will inherit a channel that
has already been proved.

That ordering was deliberate — the first commit has to be green on the machine
of record, and you cannot claim that without the lane that runs it. It also
front-loads the discovery that `ubuntu-26.04` ships neither `gcc` nor `make`,
which is a bad thing to find during the first interesting milestone.

**The FreeBSD half was written blind and passed first time.** reaper is
unreachable from the development host — a Windows box on a separate network —
so `guest-setup.sh`'s FreeBSD branch, the `ktrace`/`kdump` presence check and
every `uname`-dependent path were committed unexecuted, with the expectation
that something would break. Nothing did: `freebsd-15.1` came back 22 pass, 0
fail on the first run, as did `ubuntu-26.04`.

The specific predictions that were wrong are worth recording, because they are
the ones to stop worrying about: `-std=c99` setting `__STRICT_ANSI__` and
hiding `<sys/socket.h>` (the feature macros in `build.sh` already cover it),
BSD `sed` differing on the `# GUEST` marker parse, `mktemp -d` with no
template, and `timeout` being absent from base. All four are fine.

**What this does not mean.** M0 compiles two C files and runs an interpreter;
it touches no socket, no clock and no syscall the seam will care about. The
platform divergence this project actually has to survive — `arc4random_buf`
against `getrandom`, `AF_INET6` being 28 here and 10 there, `O_CREAT` being
`0x0200` and `0x0040` — is all still ahead, and lands at M3. Do not read a
green M0 as evidence the seam will be easy.

### Which tiers are actually built

`CONVENTIONS.md` section 8 says which tiers the project **requires** and why.
This table says which of them **exist**, and `tools/bstier.sh` checks it
against `tests/run.sh` rather than anyone typing it.

The hazard is larger here than it was in the sibling, which is why the tool
arrived before the tiers did: brainstem declares eighteen tiers and has built
four. A table that describes what you want and what you have in one column
drifts the moment those differ, and here they differ almost everywhere.

`run.sh lines` counts SOURCE lines, not checks: a loop is one line. `manual`
means a practice rather than an automated check, and such a tier must have no
marker in the suite at all.

| tier | built | run.sh lines | what it is |
|---|---|---|---|
| 0 | yes | 5 | checker self-tests |
| 1 | yes | 17 | interpreter self-test, all three EOF modes |
| 2 | yes | 1 | the program is still brainfuck |
| 3 | yes | 1 | fixture regeneration |
| 3a | yes | 2 | fixture legibility, and the expander knows no ABI |
| 3b | no | 0 | the header does not lie — needs bsframe --decode wiring |
| 4 | yes | 6 | the frame codec in isolation, two implementations |
| 5 | yes | 3 | per-op round trip |
| 6 | yes | 7 | error paths |
| 7 | no | 0 | determinism and replay — needs the clock and rng ops |
| 8 | yes | 1 | interpreter semantics matrix |
| 9 | yes | 4 | deadlock and timeout |
| 10 | no | 0 | platform parity — needs the seam, M3 |
| 10a | no | 0 | per-op syscall surface — needs the seam, M3 |
| 10b | no | 0 | the seam is narrow — needs the seam, M3 |
| 10c | yes | 6 | the tables and the lane definitions agree |
| 11 | manual | 0 | mutation, a discipline rather than a check |
| 12 | no | 0 | purity audit, M8 |

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

- **THE PATTERN, three for three: the development host and the container agree
  with each other, and the primary platform disagrees with both.** Every
  portability defect this project has had took that shape, and two of the three
  were invisible to everything runnable from the development box.

  `strtonum()` in `bfgen` is a gawk extension: Git Bash has gawk and ran it,
  the container has mawk and did not. That one was caught by the two lanes
  disagreeing. BSD `wc` right aligns its output with leading spaces, so
  `"  131076"` was compared against `"131076"` and passed on Linux for the
  wrong reason; `${#a}` has no such opinion. And `bstier` was perl, which
  FreeBSD has not shipped in base for years — the sharpest of the three,
  because the container image turns out to HAVE perl at `/usr/bin/perl`, which
  is exactly why it looked fine on the only lane that could be tested locally.

  The lesson is not "be careful with awk". It is that **a green container run
  is not evidence about FreeBSD**, and the two guest gate earns its keep at
  precisely the moment someone is tempted to skip it. Expect the next one at
  M3, where the seam starts and this class of defect lives by definition.

- **An untested code path stays broken.** `bstier --fix` passed its `-v`
  options after the awk program, so awk read them as filenames. It had been
  that way since it was written, because the suite only ever calls `check`.
  Every checker here self tests; that is no use if the self test covers half
  the tool.

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
  bfsodium at commit `e98794f`, and **no check inside this repository can
  detect divergence from upstream.** The mitigation is a periodic manual diff.
  The suite does pin the one delta that matters — if someone tidies the
  `setvbuf` line away, a check fails rather than a hang appearing.
  `tools/hx.c` is verbatim and can be diffed directly.

  The delta count is **shrinking**. bfsodium has accepted the `setvbuf` fix on
  a branch of its own, with a test that catches the buffering directly: a
  program that writes one byte and then spins forever, killed by `timeout`
  with SIGTERM, which does not flush. Once that lands, this copy's deltas are
  the two test knobs — `BFI_EOF` and `BFI_FLUSH` — which exist for tier 8 and
  have no reason to go upstream. Re-diff after it merges and update this note.
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
