# brainstem — handoff

Written for whoever picks this up next. It assumes you have read `README.md`
and `CONVENTIONS.md`; this is the part those do not say, which is how the thing
is actually built and where it has already bitten.

## State

**Milestone M7 — the tiers that needed all of it, and the ABI frozen at 1.0, since raised to 1.1.**
M7 added no opcodes. What it added was the ability to believe the rest: the
fixtures' prose is now checked against their hex, five metamorphic relations
say what each knob is allowed to change, and a mutation sweep breaks thirty
three things in turn and requires a NAMED check to notice each one. Plus
`--sort-readdir`, `--replay`, and a version number that is one number in three
places -- the document's title, the binary's constants, and the two bytes on
the wire.

**The sweep found three gaps on its first run**, and each was a claim this
suite had been making since M4: `poll` had no fixture at all, `PIPE` was a
status nothing produced, and `signal(SIGPIPE, SIG_IGN)` in main.c had nothing
standing behind it -- one line whose absence makes the broker die of a signal
halfway through answering, with no diagnosis. See the tier 11 section below.

**Milestone M6 — brainfuck drives brainfuck. Twenty three of twenty three.**
A file containing nothing but the eight instructions creates two pipes, starts
an interpreter on a *second* brainfuck program with those pipes as its stdin
and stdout, sends it two bytes, reads its answer, and collects its exit
status.

    > 0f len=53     spawn "bfi" "echo.bf", child fd 0 <- handle 4, fd 1 <- handle 7
    < 00 len=4      OK, process handle 8
    > 0a len=8      read handle 6, ONE BYTE -- a read returns up to n, never n
    < 00 len=1      68
    > 10 len=6      wait handle 8
    < 00 len=4      exited, code 0

**Gated: 224 pass, 0 fail on `freebsd-15.1` and 224 pass, 0 fail on
`ubuntu-26.04`.** That covers `bf/net/loopback6` with its two address-record
pins, ABI 1.1 with `bf/proc/interp`, and the `spawn` deadlock's regression
`bf/proc/orphan`. M7 was first gated at 208 and then 212, M6 was 173 on both
guests, M5 was 159, M4 was 148, M3 was 132, M2 was 94, M0 was 22.

**THE IPv6 RUN IS THE ONE WORTH READING.** `AF_INET6` is 28 on FreeBSD and 10
on Linux; it is the example the platform parity tier leads with, the example
CONVENTIONS section 3 was written around, and from M5 to M7 nothing sent
domain 2 at all. This gate is the first time that constant has been exercised
on the platform where it is 28 -- and the address records still read family
`02` in both directions. A green Linux run was the half of that evidence
nobody doubted; this is the other half.

It also settles a question that could only be answered by running it: the
FreeBSD guest has `::1` and will bind it. That was the one way these seven
checks could have gone red for a reason that was not a defect.

**The prediction that the syscall pins would survive held.** Marking the
interpreter's pipes close-on-exec adds two `fcntl` calls, and the first
attempt put them AFTER the fork -- inside the window `bscalls` measures --
which turned all nine pinned multisets red on the development host alone.
Moving them before the fork was the fix, and this gate is what confirms it on
the platform that could have disagreed. FreeBSD had the last word on `bcmp`
and could have had one here.

M7 itself took three gate runs, all three red on the same tier and none of
them on the same cause. See the two traps on the compiler lowerings and on `bcmp`
below; between them they are the best short answer this project has to why the
two-guest gate is not optional.

**M7 raises the gate's cost**, and it is worth knowing why before wondering
about it: tier 11 is about seventy seconds on a quiet Linux container against
ten for everything else, because it rebuilds and re-runs once per mutation.
That is the right trade exactly once per gate, and there is nothing to tune.

M3 through M6 were each written in full before either guest ran them, and each
passed the primary platform first time. M7 did not: it took three gate runs.
**Every FreeBSD failure this project has had has been one of three shapes**,
and they are worth telling apart because they call for different habits.

1. **A fact about the platform written down instead of a mechanism for
   discovering it.** perl is in base, `strtonum` exists, `wc` does not pad, the
   timekeeping page serves `clock_gettime`, `arc4random_buf` costs one syscall.
   All at M2 and M3, all cured by `tests/syscalls/README` labelling unmeasured
   rows as unmeasured and by parsing two conventions instead of assuming one.

   **M7 added a nastier version of this one: a mechanism that existed and was
   not applied.** The optional `?` rows in `tests/audit/allow.txt` were built
   at M3 for exactly the defect that then failed at M7, and the section comment
   in that file explains it at length. I added a unit that prints and did not
   extend the section. *A mechanism nobody is reminded of is a fact*, so R8
   now reminds.

2. **A pin that encoded something the program does not determine.** The
   handle table, at the preopen removal. Cured by masking it -- and see the
   trap below, because it also produced a symptom that looked like a race and
   sent me after the wrong thing.

3. **An equivalence only the other toolchain can teach you.** `bcmp`, at M7.
   clang rewrites `memcmp(a, b, n) != 0` into a different symbol and gcc does
   not, so `normalise()` was incomplete in a way nothing runnable here could
   reveal. This one is different in kind from the first two: it is not a guess
   and not a bad pin, it is a list that can only be finished by running the
   compiler I do not have.

   **It has no cure on this side, and that is the honest statement of what the
   two-guest gate is for.** The nearest thing available is a clang build of
   the objects purely to run the audit against -- it needs no interpreter, no
   fixtures and no kernel, only `nm` -- and it was not taken because it would
   be a second definition of the toolchain. It is written up as an exercise
   in "the lockdown that was removed" below, beside the other one.

Nothing has been guessed since the first of those, and nothing is pinned now
that the program does not determine.

### THE PREOPEN MODEL WAS REMOVED, AFTER M6 AND BEFORE THE FREEZE

The largest design change this project has made, and it came from one sentence
of review: *"I don't really want to preopen anything -- that's not visible by
bf and the whole point of P1 is to make the system visible and viewable by
bf."*

**What was there.** No absolute paths. Every filesystem op resolving beneath a
directory named on the command line with `--preopen-dir`. Handles 1..n handed
out in command-line order, with a table in the hello reply. "Default policy is
deny."

**Why it was wrong, and it is worth being precise.** It was not that it was
insecure. It was that it cost the project its purpose and bought a property
the project had already disclaimed in writing.

A preopen is reachable only through a handle INDEX that the operator and the
program have to agree on out of band. There was deliberately no
name-discovery op, because a brainfuck program cannot usefully compare
strings -- so the program's only interface to the outside world required the
one kind of coordination brainfuck is worst at. Meanwhile a literal path is
the CHEAPEST thing such a program can produce: `/etc/hostname` is fourteen
bytes to emit, and emitting literal bytes is the single thing brainfuck is
good at. The same cost model that made `poll` return one byte per condition
and `readdir` return one entry per call points straight at ordinary paths.

And it never was containment. `CONVENTIONS.md`'s named non-goals have said
"brainstem is not a sandbox" since the first commit, and the approved plan
said the broker "hands a brainfuck program the filesystem, network and process
spawn with its own credentials". The preopen model was doing real restricting
for a benefit two other documents explicitly denied claiming.

**How the mistake happened, because the shape is worth recognising.** The
phrase "no ambient network access" entered `GUIDE.md` at M0, in the scaffolding
commit, when no network op existed. It was *vacuously true* -- there was no
ambient network access because there was no network access -- and it was
written as though it described a policy. Then the world changed under it. At
M5 I read my own M0 sentence as a requirement and escalated it to the user as
an open design question, when the approved plan had already answered it.

**A sentence that describes the current state of the code, written in the
present tense, becomes a requirement the moment somebody else reads it.** The
project already had a defence against this and it was not applied here: fast
facts get checkers. There was no check that the documents' reachability claims
matched the binary's behaviour, because nobody can check prose -- which is the
argument for not writing prose that will expire.

**What replaced it.** `dir = 0xFFFFFFFF` -- "no handle", a value §5 had
specified all along -- means resolve the path the way any other process would,
against the broker's working directory or absolutely. A real directory handle
still means openat-beneath, which `readdir` needs. `src/path.c` now refuses
exactly two things: an empty path, and one containing a NUL.

And every program gets three handles before its first frame: the broker's
stdin, stdout and stderr, as 1, 2 and 3. Writing to handle 2 prints. That
covers the one thing a path cannot portably express -- a descriptor handed
over by a shell -- and it needs no flag, so there is nothing to agree on.

**The trap that came with it, and it took two rounds to see the whole of it.**
The hello reply now reports what those three descriptors actually ARE, so it
depends on how the broker was invoked. Naming `/dev/null` on every invocation
in `tests/run.sh` was necessary and not sufficient: the gate still failed all
seven hello-bearing traces on FreeBSD, because the kinds and rights of a
guest's stdio are not the suite's to control.

So the handle table is **masked** in tier 10 now, the way the platform byte
already was. What it reports is a property of the INVOCATION, not of the
program or the protocol, and a parity tier that pins it is pinning the wrong
thing. The name tail after it stays pinned, and a separate check asserts three
handles numbered 1, 2 and 3 with the right names.

**The same mistake, once more, in the same session.** `bf/fs/refused.poke`
ended with `stat "/"` to show that an absolute path works. A stat of the root
reports a size and a mode belonging to the host, so the pin went red on a
Linux guest that was not my container. It is an `open` of `/` now, which
returns a handle -- a reply that depends only on the program.

**The rule both of those are instances of:** a parity tier may only pin bytes
the PROGRAM determines. Anything the host, the invocation or the C library
decides has to be masked or measured, never pinned. `brk` came off the syscall
baseline for the same reason on the same day: it is glibc's heap growing, it
changed from 2 to 3 between two versions of one fixture, and tier 10b already
proves the broker allocates nothing -- so a `brk` can only ever be libc's.

### The three decisions in M6 worth not relitigating

**fork + fchdir + execve, not posix_spawn, and not fexecve.** The plan called
for `posix_spawn` with explicit file actions. It cannot resolve a path beneath
a directory handle -- there is no `posix_spawnat` -- and every path in this
ABI is relative to a preopen. The obvious repair is `openat` plus `fexecve`,
and **`fexecve` is the trap**: FreeBSD implements it in the kernel, glibc
implements it through `/proc/self/fd`, so on a Linux system without `/proc`
mounted it fails with ENOSYS. An op that worked on the primary platform and
depended on a filesystem being mounted on the secondary one is exactly the
divergence this project refuses.

So the child forks, `fchdir`s to the directory handle, installs the map, and
execs a relative path. `fchdir` is safe there and nowhere else: between fork
and exec the child is single threaded, so nothing can observe the working
directory changing. The inheritance story is not weaker than `posix_spawn`'s,
it is stronger -- everything this broker opens is close-on-exec, so the
descriptor map is not merely the intended set, it is the whole set.

**The descriptor map is applied in two passes.** A map saying "child 0 gets my
fd 5, child 5 gets my fd 0" cannot be applied in either order directly: the
first `dup2` destroys the second's source. Every source is moved above every
target first, then `dup2`'d down. One pass works until the day the numbers
overlap, and the symptom would be a child reading from the wrong end of a pipe.

**wait is idempotent after reaping.** The kernel reports a status once, so it
is cached on the handle and repeated until the handle is closed. Without that,
a program asking twice would get NOCHILD the second time and have no way to
tell that from a handle it invented. Closing a process handle does **not** kill
the child: the program asked to stop tracking it, not to end it.

### The path rule now has one home

`op_fs` and `op_proc` both resolve a path beneath a directory handle, and
`spawn` `fchdir`s and execs a relative path, so `".."` escapes there exactly
as it would in `open`. Writing the check twice would have been writing a
security-relevant rule twice, which is a rule that gets corrected once. It
lives in `src/path.c` and both call it.

### Tier 11 is the tier that audits the others, and it earns its cost

`tools/bsmut.sh` copies the tree, breaks one thing with sed, relinks the
broker alone, and runs `tests/run.sh` with `BS_ONLY` set to the check that
mutation is supposed to break -- which must then go red. Thirty four
mutations: one per built op, generated from the op table, plus nine
invariants.

**Naming the check is the point.** "Mutate, run everything, require any
failure" would be easier and would say much less. The table in bsmut.sh is a
COVERAGE MAP verified by machine -- it states which check stands behind each
op -- and when a row stops holding, the answer is not to widen the row. It is
that the op has lost its cover.

**Three of the first four survivors were the TOOL being imprecise**, and
telling that apart from a real gap is the skill this tier needs. A raw errno
cannot escape through `sys_errmap`'s `default` when `ECONNREFUSED` has a case
of its own; `badarity.bf` fails whether or not the dispatcher checks arity,
because `hello`'s own cursor runs out first. In both the behaviour had two
guards and the mutation removed one. Those rows now break the guard the named
check actually depends on. **A mutation that leaves the behaviour intact is
not a finding**, and a tier that reported it as one would teach people to
widen rows until the map meant nothing.

**It is the most expensive tier here by a wide margin** -- about seventy
seconds against ten for everything else. The first working version took 517,
and three mechanisms bought that down: `build.sh --relink` (one unit, not
twenty one), `BS_ONLY` (one check, not the suite), and lowered timeouts in the
copy. That last is the interesting one: fifteen of the thirty four mutations
SHORTEN A REPLY, which does not produce a wrong answer -- it produces a desync
that sits there until the op timeout fires. 450 of those 517 seconds were
spent waiting for defects that had already been decided.

If tier 11 ever needs to be skipped, `BS_ONLY` skips it by construction, and
that is also how bsmut's own children avoid invoking the tool that invoked
them.

### Tier 10a: nine cases, both platforms, nothing guessed

All nine cases are pinned on both platforms again, every one of them from a
gate measurement rather than an inference. The report list is empty and
`bscalls --report` says so rather than printing nothing.

Two results worth keeping. **`fs.refused`, `proc.drive` and `proc.refused` are
byte-identical on the two platforms**; only `fs.roundtrip` differs, and only by
one `fstat` and the spelling of `getdents64` against `getdirentries`. And
`brk` is not on any of them any more -- see the baseline comment in
`bscalls.sh` for why glibc's heap growth was never this tier's business.

The result worth keeping from the last round: **the proc cases measured
identically on both platforms**, which nothing else non-empty in
`tests/syscalls/` did. Every earlier divergence was libc's rather than the
kernel's -- glibc allocating a `DIR` buffer with `brk`, `arc4random_buf`
allocating its state with `mmap` and `minherit`. Where the broker does the
work itself, the two kernels agree exactly.

### What the gate found that this host could not

M3 reached `freebsd-15.1` with three tiers red across two runs, and every one
of them was the same shape the project has now hit five times: the development
host and the container agree with each other, and the primary platform
disagrees with both.

**Tier 10b, the symbol audit.** Two causes, both toolchain rather than
program. FreeBSD spells `stderr` as `__stderrp`, a pointer behind a macro, and
the undecorator stripped the leading underscores to produce `stderrp`, which
matches nothing. And clang rewrites a call whose format string has no
conversions — `printf("done")` becomes `puts`, `fprintf(stderr, "done")`
becomes `fwrite` — while gcc under `_FORTIFY_SOURCE` does neither. Pinning
either set fails on whichever platform chose the other, so the allowlist now
takes a trailing `?` for a row that is permitted but not required.

**Tier 10a, the syscall surface.** Both remaining failures were expectations I
had written from documentation rather than from a run, and both were wrong;
they are pinned from the gate now and the reasoning is in the traps below and
in `tests/syscalls/README`.

**What none of this was.** Not a bug in the broker, not a bug in the seam, and
not anything a Linux run could have found. The parts written blind and
deliberately defensive all held: `bsaudit.sh` parses both GNU and elftoolchain
`nm` conventions, `bscalls.sh` parses `kdump` and `strace` in one file, and
neither needed touching. What failed was every place I had written down a
specific fact about FreeBSD instead of a mechanism for discovering it.

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
arrived before the tiers did: at M0 brainstem declared nineteen tiers and had
built four. A table that describes what you want and what you have in one
column drifts the moment those differ, and for six milestones they differed
almost everywhere.

They do not any more. All eighteen declared tiers are built as of M7, and the
nineteenth was deleted rather than implemented -- so this table is, for the
first time, a column of yes. That is the point at which such a table stops
earning its keep and starts being a thing nobody reads, so: if a tier is ever
added, add its row as `no` on the same commit, and let the tool nag.

`run.sh lines` counts SOURCE lines, not checks: a loop is one line. `manual`
means a practice rather than an automated check, and such a tier must have no
marker in the suite at all.

| tier | built | run.sh lines | what it is |
|---|---|---|---|
| 0 | yes | 9 | checker self-tests |
| 1 | yes | 17 | interpreter self-test, all three EOF modes |
| 2 | yes | 1 | the program is still brainfuck |
| 3 | yes | 1 | fixture regeneration |
| 3a | yes | 6 | fixture legibility, and the expander knows no ABI |
| 3b | yes | 1 | the header does not lie: tools/bspoke.sh, six rules |
| 4 | yes | 6 | the frame codec in isolation, two implementations |
| 5 | yes | 31 | per-op round trip, all twenty three ops |
| 5a | yes | 6 | metamorphic: change one knob, require the rest unchanged |
| 6 | yes | 19 | error paths, every status reachable |
| 7 | yes | 21 | determinism: seed, clocks, sorted walks, and --replay |
| 8 | yes | 1 | interpreter semantics matrix |
| 9 | yes | 5 | deadlock and timeout, including a spawned child left running |
| 10 | yes | 15 | platform parity, against traces pinned in tests/trace/ |
| 10a | yes | 1 | per-op syscall surface, nine cases, both platforms measured |
| 10b | yes | 1 | the seam is narrow, measured from the objects |
| 10c | yes | 12 | the tables, the three lanes and the frozen ABI version agree |
| 11 | yes | 1 | mutation: 33 defects, each caught by a NAMED check |

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

And since M7 a **third lane**, `.github/workflows/suite.yml`, which runs the
same suite on a pull request inside `ubuntu:26.04`. It is the weakest of the
three and its final step says so in the job log, because a green tick is
exactly the kind of thing that quietly implies more than it proved: it is one
Linux userland, and every portability defect this project has had was
invisible to Linux.

Neither fallback substitutes for the other. On a FreeBSD host the native run *is* the
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

`tools/guest-setup.sh` is **the one definition of the toolchain** and every
lane runs it. reaper calls it with no argument; the `Containerfile` calls it
with `--toolchain` so the slow half is a cached layer; `container-test.sh`
calls it with `--build`; the CI workflow calls it with no argument, exactly as
reaper does. If any lane ever grows its own package line
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

- **"I am getting nondeterministic testing results", and the attribution
  changed twice.** Worth reading as a diagnostic story rather than a bug
  report, because both of my confident answers were wrong in different ways.

  My first answer was the stream read above -- and I could not reproduce it,
  so I downgraded it. My second answer was a PIN rather than a race: for one
  commit tier 10 pinned the hello reply's handle table, which reports what the
  broker's own stdio ARE, so those checks would pass or fail according to how
  the harness wired its descriptors that run. Indistinguishable from a race at
  the report level, and a mistake this project had just made three times in an
  hour.

  Then the timeline settled it against me. The runs that varied were on the
  commit where the table was ALREADY masked, and the only change between there
  and three clean gate runs was the stream read fix. **So the race was the
  cause after all, and "I could not reproduce it" was the weakest part of the
  argument, not the strongest.**

  An idle eight core development host lets the child run to completion before
  the parent reads; a loaded single processor guest interleaves. Forty runs
  under deliberate CPU load on the wrong machine proves very little about a
  scheduling window, and I presented it as though it proved something.

  **Two things to take from it.** "Nondeterministic" is already a hypothesis
  wearing the clothes of an observation -- it names timing as the cause family.
  The observation is "the same commit gives different results on repeated runs
  of the same guest", and the question that follows from it is *what is an
  input to this test that I am not treating as one*. And in a suite built on
  pinned expectations, a varying result should make you suspect a pin is
  claiming invariance for something that is not invariant -- check that before
  hunting for concurrency, and check the dates on both.

- **A SPAWNED CHILD HELD THE CONVERSATION OPEN, AND ALL THREE PROCESSES
  DEADLOCKED.** Found from the far side, in bfsodium's first program, and it
  is the worst failure shape this project has: no output, no core, nobody
  wrong by their own lights.

  `child.c` made the interpreter's two pipes with a plain `pipe()`. So a
  grandchild started by `spawn` inherited the write end of the INTERPRETER's
  stdin across `execve` — and `bs_child_finish`, which closes the broker's
  own copy precisely so a program blocked on `,` can finish, no longer gave
  the interpreter end of input at all. The interpreter blocked on `,`, the
  broker blocked in `waitpid` for the interpreter, and the grandchild blocked
  reading a stdin nothing would ever write to.

  Two lines of `fcntl(F_SETFD, FD_CLOEXEC)` fix it, and they go BEFORE the
  fork for two reasons: the descriptors are then never briefly inheritable,
  and `bscalls` opens its window AT the fork, so setup calls placed after it
  would be counted against every op in the ABI. Putting them before changed no
  pinned multiset on either platform.

  **It also made a comment true that had been false since M6.** `sys_proc.c`
  says apply_map closes only 0, 1 and 2 "because everything this broker opened
  is close-on-exec". These two were the everything else that was not.

  **The regression is `bf/proc/orphan.poke`, and it took two goes to make it
  catch anything.** The child must be deliberately LEFT RUNNING — not written
  to, not closed, never waited on — where `drive.poke` tidily reaps its own,
  and tidy is what hides this. And the program must KEEP READING after its
  exit frame: the first draft stopped there, ran off the end of its
  instructions, exited without ever needing end of input, and passed whether
  the pipes were close-on-exec or not.

  **The diagnosis took four wrong turns and they are worth listing**, because
  each looked settled. It was not the relay being slow: a trace showed all
  131084 frames done inside 55 seconds with the exit frame sent. It was not a
  spin from the EOF convention, though that WAS a real second bug in the
  client and is fixed there. It was not the broker's unbounded `waitpid`,
  which is where I wrote it up first and was wrong. A reproducer without a
  `spawn` in it terminated cleanly every time. What finally located it was
  watching the temporary file and the frame count at intervals: the work was
  finished at t=55s and the processes then sat for another 220 seconds doing
  nothing at all.

- **A read returns UP TO n bytes, and two fixtures assumed exactly n.**
  `bf/proc/drive.poke` asked for eight bytes of the child's output and read a
  reply sized for two. The child is an interpreter with unbuffered output, so
  it emits `h` and `i` as two separate one byte writes; whether both are in
  the pipe when the read happens is a scheduling question. If only one is,
  the reply is a byte shorter than the fixture reads and the conversation
  desyncs -- which presents as a hang, not as a wrong answer.

  Both that fixture and `bf/net/loopback.poke` now read ONE BYTE AT A TIME. A
  read of one blocks until there is a byte and then returns exactly it, which
  is the only size a stream read is deterministic at. A program that wants a
  known number of bytes from a stream has to loop, and the fixtures now show
  the loop instead of getting away without one.

  **This was never reproduced.** Forty runs of the old shape under CPU
  contention got both bytes forty times, on the development host. It is a
  latent defect found by reading rather than by failing, and it is recorded
  here because the NEXT one of these will present the same way: an
  intermittent hang in a fixture that has always worked. Any fixture reading
  from a pipe or a socket should be read with this in mind.

- **THE COMPILER LOWERINGS SECTION IS A THING SOMEBODY HAS TO REMEMBER, AND
  IT WENT FIVE MILESTONES BEFORE ANYBODY DID.** M7 came back from
  `freebsd-15.1` with tier 10b red and one cause: `src/replay.c` prints five
  diagnostics whose format strings have no conversions, clang lowers those to
  `fwrite` and `fputc`, gcc does not, and the allowlist had no optional rows
  for the new unit.

  This is failure shape 1 exactly -- a FACT written down instead of a
  MECHANISM -- and what makes it worth recording is that the mechanism already
  existed. The `?` rows were built at M3 for this precise defect, the section
  comment in `tests/audit/allow.txt` says so at length, and I added a unit that
  prints without extending it. A mechanism nobody is reminded of is a fact.

  So there is a reminder now: **R8** lints the allowlist rather than the
  objects -- a unit allowed `fprintf` must carry `fputs`, `fwrite` and `fputc`
  as optional rows, and one allowed `printf` must carry `puts` and `stdout`.
  It reads no object, so it says the same thing on both platforms, and the
  next unit that learns to print fails on the development host instead of on
  the guest.

  The one-character case is worth knowing on its own: clang lowers a
  conversion-free `fprintf` to `fwrite`, and one whose whole format is a
  single newline to **`fputc`**, a different symbol again. replay.c has both.

  **AND THEN A SECOND GATE RUN FOUND `bcmp`**, which is the more interesting
  half. clang rewrites `memcmp(a, b, n) != 0` into `bcmp` -- the result is
  only compared against zero, so it need not say which way -- and gcc does
  not. `replay.c` compares a recorded request against the sent one exactly
  that way.

  That one is NOT an allowlist row and must not become one. `memcmp` is
  already in IGNORABLE as a pure memory helper; `bcmp` is the same call under
  the spelling the toolchain preferred, so it belongs in `normalise()` beside
  the errno and large-file aliases. `bzero` and `bcopy` went in with it. The
  rule to carry forward: **a symbol that is the compiler saying the same thing
  differently gets normalised; a symbol that is the program asking for
  something new gets an allowlist row.** Put one in the other place and the
  audit stops measuring.

  **This is the one the development host genuinely cannot see.** R8 and R9 are
  mechanisms, and neither would have caught `bcmp`: there is no clang lane
  here, and the container is gcc. The honest mitigation is the two-guest gate
  itself, which is the whole argument for it -- and the cheapest improvement
  available, not taken here, would be a clang build of the objects purely to
  run the audit against. It needs no interpreter, no fixtures and no kernel,
  only `nm`, but it would be a second toolchain definition and that rule is
  load bearing. See "the lockdown that was removed" below.

- **A MALFORMED LINE IN THE ALLOWLIST FAILS NOTHING.** While checking the
  above against the pasted FreeBSD surface, one of my own comments in
  `tests/audit/allow.txt` turned out to have acquired a literal newline in the
  middle of it, so the tail of the sentence -- `") into fputc. ...` -- was
  sitting there as a row. It parses as the pair `") into`, and the reverse
  direction of R1 skips any pair whose unit is not in the listing, so it was
  ignored on both platforms.

  R9 now requires every non-comment line to be a row. It was found by hand
  while looking for something else, which is not a mechanism and does not
  happen twice.

- **A gate you cannot log in to has to carry its own diagnosis.** M3 came back
  from `freebsd-15.1` with two failing tiers, 10a and 10b. Both of them had
  computed the exact answer -- the expected multiset beside the observed one,
  the offending symbol beside the allowlist -- and `tests/run.sh` had thrown it
  all away, because `run()` redirected both streams to `/dev/null` and printed
  the word FAIL.

  reaper's guests are ephemeral. There is no machine left to ask, and the
  development host is a Windows box that cannot reach the primary platform at
  all, so the cost of a discarded diagnosis is a full round trip through
  somebody else's afternoon. `run()` now prints the output of anything that
  fails, indented; success is still silent, because 132 passing checks that
  each print a paragraph is a log nobody reads.

  Two checks had to change shape to have anything to say. The five parity
  comparisons were `cmp -s`, which is silent BY DESIGN, and are now `diff -u`
  against a file in `$BS_TMP`. `bsaudit.sh` now dumps the whole measured
  external surface whenever any rule fails, rather than naming one symbol --
  when a toolchain surprises you, the shape of its entire output is the thing
  worth seeing, not the first row that tripped.

  **Write the diagnosis into the tool, not the postmortem.** Anything that runs
  only on the guest should assume its author will never see the machine.

- **A syscall tier cannot see a syscall that libc does not make — and I
  guessed wrong about which ones those were.** The plan for tier 10a said "one
  fixture per op under `ktrace` or `strace`, diff the observed multiset". Run
  it on `clock_now` and the multiset is EMPTY on Linux: glibc serves
  `clock_gettime` from the vDSO, so no syscall is issued and no tracer can see
  one.

  I then wrote in this file that FreeBSD does the same through its timekeeping
  page, and pinned an empty expectation to match. **It does not.** The gate
  measured two `clock_gettime` calls, one per clock read. The general lesson
  was right and the specific claim was invented, and it sat in a document
  whose whole job is to be the part you can trust. Pins that cannot be measured
  from the host doing the work must say so in the file itself — the first
  version of `tests/syscalls/README` did, which is the only reason this reads
  as a corrected guess rather than a contradicted measurement.

  None of this is a defect and none of it is fixable — it is what the platforms
  do — but it quietly removes a claim the tier looked like it was making. So
  the tier makes the claim it can support: what is pinned is the op specific
  part of the multiset, per platform, and for several fixtures that is empty.
  If you add an op and its pinned file comes back empty, check whether the call
  is real before concluding the op does nothing.

  The same run turned up the other half. `rand.live` on FreeBSD is three calls,
  not one: `arc4random_buf` allocates its per-thread state on first use, with
  an `mmap` and a `minherit` marking the page `INHERIT_ZERO` so a fork cannot
  inherit a generator. A one-time lazy initialisation, landing inside the
  measured window only because the window opens at the fork and the first
  `random_bytes` comes after it. It is pinned as observed, and it STAYS
  pinned as observed: warming the generator at startup was an M8 prerequisite,
  M8 is gone, and moving a real call out of a measured window to make a
  picture tidier is the opposite of what this tier is for. It would still be
  the right thing to do the day somebody builds the filter in "the lockdown
  that was removed".

  And note what FreeBSD's answer costs: once that generator is warm it is pure
  userspace, so `rand.live` and `rand.seeded` measure the same thing there. The
  pair that demonstrates "a seed consults no kernel" only demonstrates it on
  Linux. Tier 7 proves the determinism itself and leans on neither.

- **A normalisation nearly made the parity tier vacuous.** Tier 10 compares a
  trace against a file in `tests/trace/`, and exactly one byte — the platform
  byte in the hello reply — is masked to `%%` first, because it exists to
  differ. The first version of that `sed` masked the wrong offset: it counted
  the 48 byte record's fields wrongly and blanked a byte of `clock_step_ns`
  instead. Every test still passed, because the wrong byte happened to be
  constant across the fixtures.

  That is the shape to fear in any tier built on normalisation: it does not
  fail, it stops asking. The mitigation is in the suite — "the pinned traces
  are not vacuous" runs a fixture with a deliberately different epoch and
  requires the comparison to FAIL. Any future normalisation gets the same
  treatment, and a mask with no such case beside it should not be believed.

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
  precisely the moment someone is tempted to skip it.

  M3 was predicted to produce the next one and did, twice over — see "what the
  gate found that this host could not" above. The seam was written expecting
  it, which is why `bsaudit.sh` parses two `nm` conventions and `bscalls.sh`
  parses both `kdump` and `strace` in one file, and why neither needed
  touching when the guest disagreed.

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
  bfsodium at commit `8edf0f6`, where the file itself last changed in
  `59b45b0`, and **no check inside this repository can detect divergence from
  upstream.** The mitigation is a periodic manual diff. The suite does pin the
  line that matters — if somebody deletes the `setvbuf` call, a check fails
  rather than a hang appearing. `tools/hx.c` is verbatim and can be diffed
  directly.

  **THE FIX WENT HOME.** bfsodium accepted the `setvbuf` one-liner and it
  landed on its `main` in `59b45b0`, with a test that catches the buffering
  directly: a program that writes one byte and then spins forever, killed by
  `timeout` with SIGTERM, which does not flush. Both copies have the line now,
  so it is no longer a difference between them.

  That leaves **two deltas**, and they are the ones that should never go
  upstream: `BFI_EOF` and `BFI_FLUSH`, which exist to test a broker bfsodium
  does not have. The interesting consequence is that the delta count has
  stopped shrinking — it is at its floor, and any future growth in it is a
  thing to argue about rather than a thing to schedule.

  The current difference is 37 lines outside the header comment, all of it
  those two knobs. Re-diff with:

      git -C ../bfsodium show main:tools/bfi.c > /tmp/up.c
      diff /tmp/up.c tools/bfi.c
- **Every tier this project declares is built.** That is true for the first
  time at M7, and it is true partly because one of them was DELETED rather
  than implemented -- see "the lockdown that was removed" below, and do not
  read the absence of a gap as the absence of a decision.

  The remaining thing to know: tier 11's op sweep covers only the ops that are
  BUILT, which is all of them today and would silently stop being a sweep if
  an op were ever declared and not built. bsmut's self test checks exactly
  that correspondence.

  This bullet used to say "every tier past 1 and 10c is declared and absent",
  which was true at M0 and false from M3 onward. It is worth recording as the
  shape this project keeps hitting: a sentence describing the state of the
  tree, written in the present tense, becoming a claim about it.

## What is next

M0 through M7 are done. Every op is built, `ABI.md` is frozen at 1.x and currently 1.1, and
and every tier this project declares is running -- which is true for the
first time, and is true partly because the last one was DELETED rather than
built.

1. **Nothing, in the sense of a planned milestone.** The plan ended at M8 and
   M8 was deleted rather than built; the section below says why, and
   `CONVENTIONS.md` carries the short version beside the milestone table.

2. **THE SEAM WITH bfsodium, WHICH NOTHING TESTS -- AND IS NOT OURS TO
   CLOSE.** Not scheduled here, and the largest real gap either project has.

   `bf/proc/drive.poke` proves a brainfuck program can drive a brainfuck
   program: two pipes, a spawn, two bytes in, two bytes out. The inner program
   is `echo.bf`, which is `,[.,]`. What that does NOT prove is the thing the
   whole three-phase scheme was designed around -- that a brainfuck program
   can take one REAL primitive's output and make it the next primitive's
   input, with no shell in the middle.

   bfsodium gates every routine against Cryptol and the RFCs. brainstem gates
   the broker across two kernels. **Neither gates the join**, and a seam
   between two well tested things is exactly where this project keeps finding
   its defects.

   It is bfsodium's blocker too, and their HANDOFF now says so: v1.0.0 is
   being held until the composition story has run end to end, because a
   library's claim is that its primitives compose and bfsodium's composition
   is currently source-level pasting through `bfexpand`.

   **THE WORK DOES NOT BELONG HERE, and an earlier version of this item said
   it did.** The argument for putting it here was that bfsodium's rulebook
   calls it pure computation with no syscalls and no P1 broker, so a tier
   there that runs this broker would contradict its own boundary. True, and
   beside the point: the fix proposed was to vendor a bfsodium routine into
   this repository's fixtures, which contradicts OURS. brainstem's README says
   the indirection through an external interpreter is what makes the broker
   indifferent to what is on the far end. A crypto routine in `bf/` would make
   this corpus domain-specific for the first time, and the next brainfuck
   library that wants chaining has nothing to do with cryptography.

   **The principle is dependency direction.** Infrastructure must not know its
   consumers. A consumer knowing its infrastructure is ordinary. So the
   chaining proof is a `programs/` directory in bfsodium, their HANDOFF has
   the full reasoning and the routine-versus-program line that goes with it,
   and their `tools/guest-setup.sh` now pins THIS repository by commit.

   **What that leaves us owning is what we already claim**: that a brainfuck
   program can drive a brainfuck program, which `bf/proc/drive.poke` proves.
   Nothing here needs to change, and the reason this item survives at all is
   that "nothing to do" is a conclusion somebody should be able to read rather
   than re-derive.

   **The pin points at us, which is the part to notice.** When brainstem is
   tagged, that pin changes from a SHA to the tag, and bfsodium treats the
   change as its own signal to approach v1.0.0 -- on the grounds that a
   library depending on an untagged commit of its infrastructure is not one
   anybody should depend on either. So tagging here is not a private
   decision any more; something downstream is watching for it.

3. **Smaller things, none of them blocking.**
   - `bf/net/loopback.bf` and `bf/proc/drive.bf` have no pinned trace, because
     an ephemeral port and a child's timing are not the program's to determine.
     A mask could bring the port under tier 10 the way the stat mtime is; the
     mask would need a vacuity check beside it, per the rule below.
   - Tier 10a has no case for a sorted walk or for a replay. Both were
     considered and neither is pinned, deliberately: an empty expectation for
     replay would be a PREDICTION about FreeBSD's libc that this project has
     been wrong about twice, and the behavioural checks in tier 7 prove more
     than a syscall count would.
   - `tools/bfj.c`, the second interpreter the plan wanted for tier 8, was
     never written. Tier 8 runs the EOF and flush matrix against `bfi` alone,
     so what it proves is that the protocol survives every convention, not
     that two interpreters agree.

### The lockdown that was removed, and the one version of it still worth building

`sys_lockdown()` existed from M3 to M7 as a frozen no-op, so that M8 -- the
purity audit, seccomp-notify on Linux and `cap_enter()` on FreeBSD -- would be
an implementation rather than a refactor. It was deleted at M7 without ever
being made real. Six lines of code, one declared-and-absent tier, and a
milestone.

**Every argument for it had been answered by something else.**

*The proof.* Tier 12 asked "can the broker prove it adds no protocol logic of
its own". `bsaudit` answers it by reading the objects and `bscalls` by pinning
each op's syscall multiset on both platforms. A lockdown proves nothing; it
forbids. The decision note below has said "Capsicum cannot do the purity
proof, it confines rather than observes" since M3, and the consequence had
simply not been followed through.

*The window.* The other argument was that a filter installed immediately
before the loop puts libc's startup out of scope by construction. `bscalls`
took that over: its window opens at the fork.

*What was left was confinement*, which the named non-goals refuse, and which
on the primary platform would have cost the project its purpose. After
`cap_enter()` there is no `open()` by path AND no `execve()` by path -- so
`open` and `spawn` both stop working, which is two of the three things
brainstem is for. The ABI would have had to grow the preopen model back.

**AN EXERCISE FOR THE READER, and it is the good version of this idea.**

There is a lockdown worth building, and it is not a confinement:

> A Linux seccomp filter, in kill mode, **generated from the pinned syscall
> multisets in `tests/syscalls/linux/`** -- permitting exactly the calls the
> ABI has been measured to make, and nothing else. `openat` stays permitted,
> so reachability is untouched; what goes away is everything the broker has
> never once been observed doing.

What makes it worth someone's afternoon is the generation step. The filter's
input would be an artifact the suite already produces and already gates on,
so an op that quietly grew a new syscall would fail the FILTER rather than a
document -- the same move as `bsmut`'s coverage map, one level down. The
existing tier 10a pin says "this op asked for exactly these"; the filter would
make the kernel say it too.

Two honest caveats for whoever takes it. It is **Linux only** -- Capsicum is
not a syscall filter and FreeBSD has no equivalent, so the primary platform
gets nothing, and this project has a standing rule against putting the weaker
engineering there. And the baseline calls `bscalls` deliberately DROPS -- the
pipe reads and writes, the poll, the reaping -- have to go back in, because
the filter covers the whole process and not just the op specific part.

Neither is a reason not to do it. They are reasons to write the tier's row
saying what it covers, which is the habit this project has anyway.

**The preopen flags are not on this list and will not be.** `--preopen-listen`
and `--preopen-connect` were recorded here as pending against the
ambient-network question. That question is answered: there is no capability
gate on anything, there is no flag that would add one, and ABI.md §8 says so
in one place instead of implying it in three. Nothing to do.

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

  **This was decided at M3 and not acted on until M7**, which is the
  interesting part: the sentence above is exactly why tier 12 could never have
  done what it was declared to do, and it sat beside a declared tier 12 for
  four milestones. A decision written down is not the same as a consequence
  taken. See "the lockdown that was removed" above for the consequence.
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
