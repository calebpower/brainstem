# brainstem — handoff

Written for whoever picks this up next. It assumes you have read `README.md`
and `CONVENTIONS.md`; this is the part those do not say, which is how the thing
is actually built and where it has already bitten.

## State

**Milestone M6 — brainfuck drives brainfuck. Twenty three of twenty three.**
A file containing nothing but the eight instructions creates two pipes, starts
an interpreter on a *second* brainfuck program with those pipes as its stdin
and stdout, sends it two bytes, reads its answer, and collects its exit
status.

    > 0f len=53     spawn "bfi" "echo.bf", child fd 0 <- handle 2, fd 1 <- handle 5
    < 00 len=4      OK, process handle 6
    > 0a len=8      read handle 4
    < 00 len=2      6869
    > 10 len=6      wait handle 6
    < 00 len=4      exited, code 0

**Gated: 173 pass, 0 fail on `freebsd-15.1` and 173 pass, 0 fail on
`ubuntu-26.04`.** That covers the removal of the preopen model, which touched
every fixture in the tree, and the stream read that followed it. M6 was 172 on
both guests, M5 was 159, M4 was 148, M3 was 132, M2 was 94, M0 was 22.

M3 through M6 were each written in full before either guest ran them, and each
passed the primary platform first time. **Every FreeBSD failure this project
has had has been one of two shapes**, and they are worth telling apart because
they call for different habits:

1. **A fact about the platform written down instead of a mechanism for
   discovering it.** perl is in base, `strtonum` exists, `wc` does not pad, the
   timekeeping page serves `clock_gettime`, `arc4random_buf` costs one syscall.
   All at M2 and M3, all cured by `tests/syscalls/README` labelling unmeasured
   rows as unmeasured and by parsing two conventions instead of assuming one.
2. **A pin that encoded something the program does not determine.** The
   handle table, at the preopen removal. Cured by masking it -- and see the
   trap below, because it also produced a symptom that looked like a race and
   sent me after the wrong thing.

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

### What is left is tiers, not ops

Every op is built. M7 and M8 add no opcodes. That changes what "done" looks
like from here: the remaining work is mutation testing swept across all
twenty three, the metamorphic checks that span ops, freezing `ABI.md`,
`--replay`, `--sort-readdir`, the capability questions from §8.1, and
`sys_lockdown()` made real.

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
arrived before the tiers did: brainstem declares eighteen tiers and has built
four. A table that describes what you want and what you have in one column
drifts the moment those differ, and here they differ almost everywhere.

`run.sh lines` counts SOURCE lines, not checks: a loop is one line. `manual`
means a practice rather than an automated check, and such a tier must have no
marker in the suite at all.

| tier | built | run.sh lines | what it is |
|---|---|---|---|
| 0 | yes | 8 | checker self-tests |
| 1 | yes | 17 | interpreter self-test, all three EOF modes |
| 2 | yes | 1 | the program is still brainfuck |
| 3 | yes | 1 | fixture regeneration |
| 3a | yes | 3 | fixture legibility, and the expander knows no ABI |
| 3b | yes | 1 | the header does not lie: tools/bspoke.sh, six rules |
| 4 | yes | 6 | the frame codec in isolation, two implementations |
| 5 | yes | 23 | per-op round trip |
| 5a | yes | 6 | metamorphic: change one knob, require the rest unchanged |
| 6 | yes | 17 | error paths |
| 7 | yes | 21 | determinism: seed, clocks, sorted walks, and --replay |
| 8 | yes | 1 | interpreter semantics matrix |
| 9 | yes | 4 | deadlock and timeout |
| 10 | yes | 13 | platform parity, against traces pinned in tests/trace/ |
| 10a | yes | 1 | per-op syscall surface, nine cases, both platforms measured |
| 10b | yes | 1 | the seam is narrow, measured from the objects |
| 10c | yes | 10 | the tables and the lane definitions agree |
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
  `random_bytes` comes after it. It is pinned as observed; moving it out is an
  M8 prerequisite and is listed there.

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
  precisely the moment someone is tempted to skip it. M3 was predicted to produce the next one and has not yet been run on
  FreeBSD, so that prediction is still open; the seam was written to expect
  it, which is why `bsaudit.sh` parses two `nm` conventions and `bscalls.sh`
  parses both `kdump` and `strace` in one file.

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

M0 through M6 are done and every op is built. **What remains adds no
opcodes**, which changes the shape of the work: from here it is tiers,
freezing, and the capability questions.

1. **M7 — the tiers that need all of it.** Tier 11 swept across every op;
   metamorphic checks spanning ops. ABI.md frozen. `--replay`. Also
   `--sort-readdir`, moving the PROCESS cases out of `bscalls`' report list
   and into its pinned list, and `--preopen-listen` /
   `--preopen-connect` -- which should land with whatever answer the
   ambient-network question above gets.
2. **M8 — purity.** `sys_lockdown()` made real: seccomp-notify on Linux,
   `cap_enter()` on FreeBSD. Expect ABI additions, since `cap_enter()` forces
   the preopen model onto `open`.

   **Prerequisite, found at M3 and worth doing first:** draw a few bytes
   through `sys_random` at startup, before the fork, when no seed is in force.
   FreeBSD's `arc4random_buf` allocates its generator state lazily on first
   use — an `mmap` and a `minherit` — and a filter installed before the loop
   would otherwise have to permit both, forever, so that one lazy
   initialisation can happen inside the ABI path. Warming it moves those calls
   outside the window the filter covers and makes every `random_bytes` alike.
   The policy belongs in `det.c` rather than the seam, because the branch it
   needs — warm only when unseeded — is the branch `det.c` already owns. It
   will change `tests/syscalls/freebsd/rand.live.txt` to empty; re-pin with
   `--record` on the guest and say so.

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
