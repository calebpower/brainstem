# brainstem conventions

The rulebook every source file obeys. Normative. If you are changing
brainstem, this is the document you are accountable to; if you are using it,
read [GUIDE.md](GUIDE.md) instead.

*(This file is Markdown, not brainfuck, so it uses normal punctuation freely.)*

---

## 0. The one rule that bites first: the program must stay brainfuck

Everything else here is negotiable. This is not.

A brainstem program uses the eight instructions `><+-.,[]` and nothing else.
No added instruction, no dialect, no debug output, no `;` comments in a
committed fixture — brainfuck does not agree on comments, and the sibling's
convention is not universal.

The temptation is constant and it always looks reasonable. Adding one
instruction would make the protocol cheaper to emit. Depending on `,` at
end-of-input would make the no-broker check shorter. Assuming 16-bit cells
would halve some arithmetic. Every one of those trades away the only
interesting property the project has, which is that **the same file runs
unmodified under an interpreter that has never heard of brainstem**.

This is checked mechanically, on the committed bytes, by `tools/bsbf
--portable`. No other tier can see it — every other tier asks what a fixture
*achieves*, and a fixture achieving it with an extension would pass all of
them.

---

## 1. Machine model (frozen, inherited)

Identical to bfsodium's, deliberately, so a program can move between the two
projects:

- Cells are unsigned 8-bit and wrap mod 256.
- The tape is unbounded to the right and zero-filled on demand.
- Moving left of cell 0 is a hard error, surfaced rather than hidden.
- `.` and `,` are raw bytes. No newline translation, no encoding.
- **`,` at end of input is undefined and a program must not depend on it.**
  Real interpreters leave the cell unchanged, store 0, or store 255. brainstem
  does not pick one; the suite runs every fixture under all three, and a
  fixture that behaves differently across them is broken.

The vendored `tools/bfi` is the oracle of record for the first four. It is not
the oracle for the fifth, because there is no such thing.

---

## 2. Numbers on the wire

- Unsigned, little-endian, 1, 2, 4 or 8 bytes. No varints. No bitfields
  spanning a byte.
- **No signed field exists anywhere in this ABI.** Where a negative value is
  needed, the sign goes into an enum and the magnitude stays unsigned — see
  `seek`'s `whence`. A two's-complement 64-bit value means negating eight
  cells and propagating a carry through an adder that costs tens of thousands
  of instructions. The rule survives because that is unacceptable, not because
  it is tidy.
- Booleans are whole bytes, exactly `0x00` or `0x01`, never bits. Brainfuck
  has no bitwise instruction; bfsodium had to build `XOR8` from eight halvings.
  Four bytes per poll descriptor instead of one is a trade the measured cost
  model says is invisible.
- The one exception is an IP address, which is a byte string in reading order
  because it has no endianness. Stated as: **integers are little-endian, byte
  strings are in reading order.**

**Zero is free.** `.` on an untouched cell emits `0x00`. So every enum's
commonest member is 0, every default flag byte is 0, and all padding is 0. This
is why records are flat and redundant rather than compact — the compact version
would cost the program arithmetic to save bytes that cost it nothing.

---

## 3. Nothing chosen by an OS header crosses the wire

Normative, and the platform-parity tier enforces it.

`AF_INET6` is 28 on FreeBSD and 10 on Linux. `O_CREAT` is `0x0200` and
`0x0040`. `SIGUSR1` is 30 and 10. errno 11 is `EAGAIN` on one and `EDEADLK` on
the other, and 35 is the reverse.

So address families, socket types, open flags, poll events, file kinds, signal
numbers and status codes are **brainstem's own enumerations**, mapped at the
seam and dying there. A `struct sockaddr` never reaches a frame. A `struct
stat` never reaches a frame. The mapping is written in symbolic constants and
compiled per platform, never in numbers.

The single exception is the low nine permission bits in `open` and `mkdir`,
which are identical on both platforms and are what a human writes as `0644`.

---

## 4. The platform seam

`src/sys.h` is the boundary, and **no POSIX type appears in it** — no `struct
stat`, no `sockaddr`, no `mode_t`, no `pid_t`, no `errno`. Only fixed-width
types and brainstem's own records.

That one rule does three jobs. It keeps the wire identical across platforms,
because an op cannot leak a platform value into a frame when the seam never
hands it one. It confines the extension surface: only `sys_freebsd.c` and
`sys_linux.c` may be compiled with a namespace-widening macro. And it makes a
future `sys_win32.c` an implementation rather than a refactor.

Every seam function returns a `bs_err`, never −1.

**`errno` is not confined to the seam, and saying it was would have been the
comfortable lie.** `broker.c` reads it to retry an interrupted pipe read and
`main.c` reads it in the interpreter probe; neither owns any ABI surface, and
pretending otherwise would have meant either a rule that is false or a wrapper
that exists to satisfy a sentence. What is actually enforced, by
`tools/bsaudit.sh` against the objects, is the rule with teeth: **no `op_*`
unit may read `errno`, and `sys_errmap` may be called only from `src/sys_*.c`.**
An errno therefore cannot become a status anywhere except at the seam, which
is the property the confinement was ever for.

FreeBSD is the primary platform and Linux is second. That order is a statement
about which failure is more serious, not about which runs first.

---

## 5. The broker's structure

- **One code path per op.** `src/ops.def` is an X-macro table included three
  times; dispatch is a bounds-checked array index with no switch and no default
  arm that does work. Arity is checked from the table, the handler is called
  exactly once, and the reply is written in exactly one place.
- **An op handler cannot reach the wire.** The channel type is declared in
  `main.c`, not in any header an op file includes, so the inability is
  structural rather than a rule someone remembers.
- **No allocation on the ABI path, and no stdio on it.** Two fixed frame
  buffers, sized by a commented `#define`. Raw `read`/`write` only.
- **One definition of the build.** `tools/build.sh` is the only thing that
  invokes a compiler, and the suite checks that. This is a rule the sibling
  does not have and arguably needs: bfsodium carries five `cc` lines in its
  suite and five more in its guest-setup, which is the same defect its
  Containerfile check exists to forbid, one level down.
- **One definition of the toolchain.** `tools/guest-setup.sh`, called by both
  lanes. A Containerfile carrying its own package list is a second definition,
  and the day it drifts is the day the fallback passes what the gate would
  fail — silently, because a container with a different compiler still runs
  every test and still says PASS.
- Tunables are `#define`s with the reasoning in a comment above them, not
  bare numbers.
- C tools follow the sibling's shape: C99, no dependencies, a header comment
  with a `Usage:` block, manual `argv[1]` dispatch, `--selftest`, exit 0 clean
  / 1 failure / 2 usage.

---

## 6. Fixtures

Fixtures are written as `.poke` skeletons and expanded to `.bf` by
`tools/bfgen.sh`. Both are committed and the suite proves the second is
byte-for-byte the expansion of the first.

**The expander knows nothing about the ABI.** Four directives — `EMIT <hex>`,
`READ n`, `ECHO n`, `LOOP`/`END`. No op names, no length computation, no path
into `ABI.md`. Every hex byte in every skeleton is a number a person read out
of the specification and typed. The suite greps the expander to prove it stays
that way, and that grep is the line between an expander and a compiler.

**Generating frames from the op table is forbidden.** If a fixture built its
request with the same encoder the broker parses with, a byte-order bug would be
invisible to every test in the suite. That is bfsodium's two-oracle rule
wearing new clothes, and it is the reason the transcription is done by hand
even though the typing is not.

**Why this is not the transpiler bfsodium deleted.** That project deleted its
transpiler because a generated library cannot claim to be hand-written
brainfuck — provenance *was* the claim. brainstem's claim is different: that a
fixture is brainfuck *at all*, and that is checked on the committed bytes by a
lint rather than promised by its author. The rule survives, restated for what
it actually protects.

A fixture's header cites the ABI section its hex came from and states the
frame it emits; `tools/bsframe --decode` proves the header describes the bytes
the file actually sends. A comment that lies is the sharpest risk in a
generated corpus, and no output tier can see it.

---

## 7. The I/O contract

Frames are specified normatively in [ABI.md](ABI.md); this section states only
what is binding on the implementation.

- Strictly half duplex: the program emits a whole request before any `,`, the
  broker reads a whole request before any write. **This invariant is the entire
  deadlock proof** — see ABI §10 — so a change that breaks it is not an
  optimisation, it is a defect.
- Exactly one response per request, `exit` included.
- Every frame is length prefixed. A program can always drain what it does not
  understand, and that *is* the forward-compatibility mechanism.
- An error response carries no payload.
- The broker never writes to the protocol channel except as a response.
  Diagnostics go to the broker's own stderr, always.
- A conforming interpreter must deliver each `.` byte before the program next
  blocks on `,`. brainstem diagnoses a violation rather than hanging behind it.

---

## 8. Testing protocol

The non-negotiables first:

- **Both polarities, always.** A checker must be shown catching the defect it
  exists for *and* passing clean input. A checker never observed failing is
  indistinguishable from a clean corpus.
- **Self-test before you check anything real.** Every checker's `--selftest`
  runs before the first real file, so a broken checker aborts the suite rather
  than blessing the tree.
- **Mutation-check every new assertion.** Break the thing it covers, confirm
  the test fails, restore. This has already earned itself here: the first
  version of the "the container lane installs nothing of its own" check read
  the whole Containerfile and failed on its own comment.
- **A property that cannot fail is not evidence.** Where a test asserts that
  something is stable, a companion must assert that the knob is not inert — the
  same seed twice is identical *and* a different seed differs.
- **Never weaken a test to pass.** No skips, no narrowed scopes. Every
  narrowing needs a stated reason naming exactly what it narrows.
- **The suite names the platform it ran on.** A log pasted into a commit body
  with no kernel named will be read as the whole gate, and the gate is two
  platforms.

Tiers, as they apply here. Each earns its place by a defect no cheaper tier can
see.

| Tier | Question it answers | How |
|---|---|---|
| 0 checker self-tests | Would any of these fire if violated? | Every checker's `--selftest`, both polarities, before any real file is examined. |
| 1 interpreter self-test | Is the oracle of record sound? | The vendored interpreter on core ops, nested loops, tape growth, wrap both ways, left-of-zero, binary transparency including `0x00` and `0x0a`, and **all three end-of-input conventions**. Everything runs through it, so it must be trustworthy. |
| 2 the program is still brainfuck | Has a fixture stopped being standard brainfuck? | `bsbf --portable` on every committed `.bf`: only `><+-.,[]` and whitespace. §0's claim, mechanised. |
| 3 fixture regeneration | Is the committed brainfuck what its skeleton says? | Byte-for-byte `bfgen` of the `.poke`. This is what licenses the skeleton to be the review artifact. |
| 3a fixture legibility | Can it be reviewed at all? | A header naming the op, citing the ABI section, and stating the expected reply length. A `.bf` carries no comments by design, so the skeleton is the only place review can happen. |
| 3b the header does not lie | Does it emit what it claims? | `bsframe --decode` renders the skeleton's `EMIT` bytes and the suite compares that to the header. |
| 4 frame codec in isolation | Does the codec hold at its edges? | Driven from pinned vectors with no process, no descriptor and no kernel: zero length, maximum length, one over, a header split across reads, a length that overruns. Cross-checked against `bsframe`, written independently from ABI.md — two encoders, one spec. |
| 5 per-op round trip | Does each op reach the OS and come back? | One fixture per op through a real interpreter over real pipes. |
| 6 error paths | Is every declared status reachable? | A fixture per status, including a **stale** handle whose slot was recycled. The set of statuses declared but never produced is pinned, exactly as the sibling pins the files declaring no interface: a status nobody can reach is either dead spec or an untested branch, and both should be visible. |
| 7 determinism and replay | Do the nondeterministic ops become deterministic on demand? | Same seed byte-identical, different seed different; a trace replays exactly. |
| 8 interpreter semantics matrix | Does the protocol depend on anything brainfuck leaves unspecified? | Every fixture under each end-of-input convention and both buffering modes. The fixture's job is unchanged and the interpreter's legal freedoms vary. **This tier would have caught the buffering deadlock before it shipped.** |
| 9 deadlock and timeout | When the far end stops talking, does the broker say so? | A stalled conversation must exit with a diagnosis inside the deadline. The defect is the project's worst: two processes each waiting for the other produce no output, no core, and a CI line reading "timed out", which points at everything except the cause. *A suite that can hang is a suite nobody will run.* |
| 10 platform parity | Do the two gate platforms produce the same bytes? | The same fixtures, byte-identical on `freebsd-15.1` and `ubuntu-26.04`, against expectations pinned in the repository rather than observed on either. Catches a family constant, an open flag, a `stat` field that is 32 bits in one place, an errno that escaped the map. |
| 10a per-op syscall surface | Does each op do exactly what it claims? | `bscalls` diffs the observed syscall multiset against a pinned one, via `ktrace` on FreeBSD and `strace` on Linux. This **measures** rather than trusting the source, and it works on the primary platform, which hand-written syscall wrappers never would. |
| 10b the seam is narrow | Has the platform surface leaked upward? | `bsaudit`: per-object undefined-symbol allowlists, each op symbol referenced exactly once, `errno` only in the seam files, namespace macros only in two. |
| 10c the tables agree | Do the spec, the code and the configuration describe each other? | `bsabi` diffs `--dump-abi` against ABI.md both directions; the suite checks the toolchain has one definition, the build has one definition, and the guests reaper will run are the ones `guest-setup.sh` knows. |
| 11 mutation | Would the suite catch the bug it claims to? | Break each handler and each rule, confirm the named tier fails, restore. |
| 12 purity audit | Can the broker prove it adds no protocol logic of its own? | `sys_lockdown()` made real: seccomp-notify on Linux, `cap_enter()` on FreeBSD, installed after startup so libc initialisation is out of scope by construction. **Milestone M8, declared here rather than silently absent.** |

### Named non-goals

What brainstem does **not** prove, stated rather than omitted:

- **brainstem is not a sandbox.** It brokers syscalls with its own credentials
  and confines nothing. A program sees the system its broker sees: ordinary
  paths, ordinary sockets, ordinary spawn.

  This used to read "the capability model is a usability and determinism
  feature ... not a containment claim", which was true about the CLAIM and
  false about the MECHANISM -- there was a preopen model doing real
  restricting, for a benefit this line already disclaimed. It was removed at
  M6; ABI.md section 8.0 records why, and the short version is that it cost
  the project its whole purpose. Do not run brainfuck you did not write.
- **No timing or side-channel claim**, for the sibling's reasons and one of our
  own: every op crosses two pipes and a process boundary.
- **Not a performance story.** One byte per `.`, and an interpreter spending
  millions of instructions between syscalls. Making it fast would mean
  batching, which would mean protocol logic in the broker, which is the one
  thing tier 12 exists to forbid.
- **No name resolution.** `getaddrinfo` loads NSS modules at runtime, defeats
  static linking, and would be the sole exemption in both audit tiers, so
  `connect` takes a literal address. This is a decision, not an omission.
- **No `truncate`, `symlink`, `link`, `dup`, `chmod`, `kill`,
  `sendto`/`recvfrom`, `setsockopt`.** The v1 op set is 23 and these are not in
  it. `truncate` is the one most likely to be missed.
- **Windows is out of scope for v1** and named so its absence is a decision.
  The seam accommodates it; nothing has paid for it.
- **Tiers that do not apply**, named rather than skipped: concurrency (the
  broker is single-threaded and serves one program, by construction), and
  anything involving a user interface.

---

## 9. Scope and roadmap

Milestones, each ending in a committable unit green on **both** guests.

| | |
|---|---|
| **M0** | the two lanes, the documents, the vendored interpreter. No broker. |
| **M1** | the frame codec, alone — no process, no descriptor, no platform. |
| **M2** | a standard brainfuck program completes a round trip. Three `ctl` ops, which do not cross the seam, so deadlock and timeout are settled at zero platform cost **before twenty more ops inherit them**. |
| **M3** | **done.** The seam, `clock_now` and `random_bytes`, `--seed` and `--clock`, and the two measured tiers. Four of twenty three. |
| **M4** | **done.** Handles with generations and eleven ops: the filesystem and the bytes that move through it. Fifteen of twenty three. |
| **M5** | **done.** Five net ops, IPv4 and IPv6. No `netecho` helper was needed: the fixture connects to itself, which removes the second process the plan assumed. Twenty of twenty three. |
| **M6** | **done.** `pipe`, `spawn`, `wait`. Twenty three of twenty three, and a brainfuck program that runs a brainfuck program. |
| **M7** | the tiers that need all of it: mutation, cross-op metamorphic. ABI frozen. |
| **M8** | the purity audit. |

M2 is the milestone that validates or kills the idea, and it is cheap. If a
standard brainfuck program cannot complete a round trip through a real
interpreter over real pipes, nothing after it matters.
