# brainstem ABI — version 1.0

The normative wire specification. This document is the contract: a client in
any language is conforming if it speaks what is written here, and the broker is
correct if it does the same. Where this document and the implementation
disagree, one of them is a defect — `tools/bsabi` exists to say which.

If you want to *write* a brainfuck program, read [GUIDE.md](GUIDE.md) first;
it covers the same ground with worked examples and far fewer tables. This
document is the reference you come back to for a field width.

*(This file is Markdown, not brainfuck, so it uses normal punctuation freely.)*

---

## 1. Transport

```
broker                                interpreter              program
   |-- write --> [pipe] -----------> stdin -------> ',' reads
   |<-- read --- [pipe] <----------- stdout <------ '.' writes
```

The broker spawns an interpreter (`--interp`, defaulting to `bfi` on `PATH`)
with the program as its argument, and speaks the protocol through the
program's own `,` and `.`.

**The program's stdin and stdout are consumed by the protocol.** There is no
second channel and no escape. A program that wants to write to a terminal asks
for one of the standard handles (§8.1) and uses `write`. The broker's diagnostics
go to the broker's own stderr; the interpreter's stderr is passed through
untouched.

### 1.1 Requirement I1 — the interpreter must not buffer its output

**A conforming interpreter MUST deliver each byte written by `.` to its stdout
file description before the program next blocks on `,`.**

This is not pedantry. An interpreter using default C stdio on a pipe gets full
buffering: the request sits in a 4 KiB `FILE` buffer, the broker blocks reading
a request that was never sent, the program blocks reading a reply that cannot
come, and the only symptom is a hang with no output. bfsodium's `bfi` has
exactly this defect as committed; the vendored copy in `tools/bfi.c` fixes it
with one `setvbuf` call.

The broker diagnoses rather than hangs: `--check-interpreter PATH` probes the
property directly, and `--hello-timeout` (default 5000 ms) reports buffering as
the likely cause when no handshake arrives. `stdbuf -o0` is the workaround for
an interpreter you cannot change.

A pty is **not** a fix. Line buffering flushes on `0x0a`, which in a binary
protocol is an ordinary payload byte, and termios would translate it anyway.

---

## 2. Frames

Request, program to broker:

| off | width | field |
|---|---|---|
| 0 | u8 | `op` — `0x01`–`0x17` |
| 1 | u16 LE | `len` — payload length, 0–65535 |
| 3 | `len` | `payload` |

Response, broker to program:

| off | width | field |
|---|---|---|
| 0 | u8 | `status` — `0x00` is OK |
| 1 | u16 LE | `len` |
| 3 | `len` | `payload` |

**Every integer on the wire is unsigned little-endian**, 1, 2, 4 or 8 bytes
wide. No varints, no signed fields anywhere, no network byte order, no
bitfields spanning a byte boundary. Little-endian matches the sibling: 33 of
bfsodium's primitives declare `len{2} LE` inputs, and a program should never
have to hold two byte orders in its head.

LE is also load-bearing rather than merely consistent. A `u64` size or
timestamp can be read as a `u32` by taking the first four bytes and ignoring
the rest — correct for file sizes below 4 GiB and for epoch seconds until
2106. bfsodium has `add32` and has no `add64`.

The one deliberate exception is an IP address (§6), which is a byte string in
reading order, not an integer.

### 2.1 Invariants

- **I2 — exactly one response per request.** No exceptions, `exit` included.
  This is what makes the deadlock argument in §10 total.
- **I3 — strictly half duplex.** The program emits a whole request before any
  `,`; the broker reads a whole request before any write, and writes a whole
  response before any read. Both sides are never writing at once.
- **I4 — every frame is length prefixed**, so a program can always drain what
  it does not understand. This *is* the forward-compatibility mechanism: read
  `len`, consume the fields you know, count down the remainder. New fields are
  added at the end of a record and old programs skip them.
- **I5 — at most one variable-length field per frame, and it is last.** Two ops
  break this and both hoist their lengths into the fixed prefix: `spawn` (§7.15)
  and `rename` (§7.23).
- **I6 — an error response carries no payload.** `status` above `0x01` always
  implies `len == 0`. After a bad status a program reads exactly two more bytes,
  both zero, and is done. Detail for humans goes to the broker's stderr.

### 2.2 `op = 0x00` is permanently invalid

So is handle `0`. The commonest brainfuck defect is emitting a cell you forgot
to clear or forgot to fill, and a zero byte is what that produces. Reserving
zero at both places where a program's first mistake will land turns a silent
misinterpretation into a loud `PROTO`.

The cost is that `hello` needs one `+`. That is the correct trade.

### 2.3 Length validation is the anti-desync mechanism

The broker knows the exact expected length, or the exact minimum, for every
opcode, and rejects anything else with `PROTO`. This matters more than it
looks. The realistic catastrophic failure of this protocol is not deadlock but
**desynchronisation**: a program that reads only the first four bytes of a
24-byte reply leaves twenty in the pipe, and every exchange after that is
shifted. Strict per-op validation turns that into a loud failure within a frame
or two rather than silent drift.

---

## 3. The handshake

`hello` must be the first frame. Any other op first is `NOHELLO` and fatal; a
second `hello` is `REHELLO` and fatal.

**Request payload — exactly 10 bytes:**

| off | width | field | value |
|---|---|---|---|
| 0 | u8[4] | `magic` | `42 53 54 4D` — `"BSTM"` |
| 4 | u16 LE | `want_major` | 1 |
| 6 | u16 LE | `want_minor` | 0 |
| 8 | u16 LE | `flags` | 0, reserved |

**Response payload — a 48-byte fixed prefix, then two tails:**

| off | width | field |
|---|---|---|
| 0 | u8[4] | `magic` — `"BSTM"` |
| 4 | u16 LE | `major` — 1 |
| 6 | u16 LE | `minor` |
| 8 | u16 LE | `max_payload` — 65535 in v1.0 |
| 10 | u16 LE | `max_op` — highest opcode, `0x17` |
| 12 | u32 LE | `feature_lo` — bit *n* set ⇒ opcode *n* is available |
| 16 | u32 LE | `feature_hi` |
| 20 | u8 | `platform` — 1 FreeBSD, 2 Linux, 3 Windows, 0 other |
| 21 | u8 | `clock_mode` — 0 live, 1 frozen, 2 virtual |
| 22 | u8 | `rng_mode` — 0 live, 1 seeded |
| 23 | u8 | reserved, 0 |
| 24 | u32 LE | `clock_step_ns` — virtual-clock advance per request; 0 otherwise |
| 28 | u8[16] | `seed` — the RNG seed in use, or 16 zero bytes when `rng_mode` is 0 |
| 44 | u16 LE | `nhandle` — how many handles the program starts with |
| 46 | u16 LE | `nametail_len` |
| 48 | `nhandle` × 12 | the handle table (§8.2) |
| … | `nametail_len` | the name tail: `nhandle` × (`namelen{u8}` ‖ `name`) |

A program that does not care about the names reads `48 + 12·nhandle` bytes
and counts down `nametail_len` to drain. One that does care walks the
tail with a `u8` counter per entry. Both are flat loops.

`feature_lo`/`feature_hi` report what is available **before the program tries**,
so a well-written program reports a clear failure instead of stumbling into a
`DENIED`. A bit is clear when the op is disabled by policy, unsupported on this
platform, or compiled out.

### 3.1 Version negotiation

The broker does the comparison, not the program. A numeric comparison is cheap
in C and expensive in brainfuck, and a program that got it wrong would
misbehave in a way nobody could debug.

| condition | broker does |
|---|---|
| `magic` wrong | reply `PROTO`, payload empty; diagnose on stderr, exit 70 |
| `want_major` ≠ 1 | reply `VERSION`, payload empty, exit 70 |
| `want_minor` > broker's | reply `VERSION`, exit 70 |
| otherwise | reply `OK` with the 48-byte record |

A fatal status is still a REPLY. Invariant I2 -- exactly one response per
request -- has no exceptions, including this one: a program that sent a bad
magic is still sitting in a `,` waiting for a status byte, and leaving it
there would turn a diagnosable error into the hang this ABI spends a whole
section avoiding. The broker answers, then stops speaking.

**Major version is never negotiated** — there is no compatible subset. Minor
versions may differ freely; the broker echoes its own, both sides operate at
the lower, and a program discovers what is actually present through
`feature_*` and `NOSUCHOP`, never by inferring from the minor number.

Within major 1 a minor bump may only add opcodes, add status codes, or assign
meaning to bytes already marked reserved. It may never change the width,
offset or meaning of an existing field, nor lengthen an existing fixed record.
That is what makes "fixed offsets" a durable promise, and it is why several
records carry explicit reserved bytes.

### 3.2 Detecting that there is no broker

The same program run under a bare interpreter must degrade, not corrupt.
Interpreters disagree three ways about `,` at end of input: leave the cell
unchanged, store 0, or store 255.

**The discipline:** before reading the reply, set the status cell to `0x7F` and
both length cells to `0x00`. Then:

| interpreter | status reads | caught by |
|---|---|---|
| unchanged | `0x7F` | `0x7F` is reserved and never transmitted |
| stores 0 | `0x00`, which looks like OK | `len` is 0, so the mandatory magic is absent |
| stores 255 | `0xFF` | `0xFF` is reserved and never transmitted |

The load-bearing part is the middle row: **the hello reply must be non-empty
and must contain non-zero bytes**, or an EOF-returns-zero interpreter is
indistinguishable from a successful handshake. No op may be used before hello
for exactly this reason.

---

## 4. Status codes

Never errno. The proof is one line: **errno 11 is `EAGAIN` on Linux and
`EDEADLK` on FreeBSD; errno 35 is `EDEADLK` on Linux and `EAGAIN` on FreeBSD.**
They are transposed. A program branching on the raw number would retry on one
platform and abort on the other from the same byte. `ELOOP` is 40 versus 62,
`ENOTEMPTY` 39 versus 66, `ETIMEDOUT` 110 versus 60.

Codes are ordered so the cheapest to test are the commonest, because testing
`status == N` costs N decrements in brainfuck.

| code | name | meaning |
|---|---|---|
| `0x00` | `OK` | |
| `0x01` | `END` | end of stream, or end of directory |
| `0x02` | `AGAIN` | would block; also connect in progress |
| `0x03` | `BADF` | no such handle, stale handle, or wrong kind of handle |
| `0x04` | `DENIED` | refused by capability policy or by the host |
| `0x05` | `NOENT` | no such file or directory |
| `0x06` | `INVAL` | malformed value, bad flag bit, wrong length for this op |
| `0x07` | `PIPE` | peer is gone |
| `0x08` | `EXIST` | already exists |
| `0x09` | `NOTDIR` | expected a directory |
| `0x0A` | `ISDIR` | expected a non-directory |
| `0x0B` | `NOTEMPTY` | directory not empty |
| `0x0C` | `INTR` | interrupted, retry |
| `0x0D` | `SPIPE` | not seekable |
| `0x0E` | `LOOP` | too many symbolic links |
| `0x0F` | `NAMETOOLONG` | |
| `0x10` | `NOSPC` | out of space or quota |
| `0x11` | `EXHAUSTED` | out of handles, memory, or processes |
| `0x12` | `XDEV` | cross-device operation |
| `0x13` | `ROFS` | read-only filesystem |
| `0x14` | `OVERFLOW` | value or result does not fit |
| `0x15` | `TIMEDOUT` | OS timeout, or the broker's `--op-timeout` |
| `0x16` | `BUSY` | resource busy |
| `0x17` | `NOTSUP` | operation, family or protocol not supported |
| `0x18` | `NOSYS` | the op exists in this ABI but this platform or build cannot provide it |
| `0x19` | `NOSUCHOP` | this broker does not implement this opcode — **recoverable** |
| `0x1A` | `IO` | I/O error, **and every errno with no mapping** |
| `0x20` | `CONNREFUSED` | |
| `0x21` | `CONNRESET` | also connection aborted |
| `0x22` | `ADDRINUSE` | |
| `0x23` | `ADDRNOTAVAIL` | |
| `0x24` | `NETUNREACH` | also host unreachable, network down |
| `0x25` | `NOTCONN` | |
| `0x26` | `ISCONN` | |
| `0x27` | `MSGSIZE` | |
| `0x30` | `NOCHILD` | no such child |
| `0x31` | `NOEXEC` | not executable, or argv/env too large |

Fatal protocol errors. **After sending one of these the broker writes nothing
further and exits 70:**

| code | name | meaning |
|---|---|---|
| `0xE0` | `PROTO` | malformed frame, bad opcode, unrecoverable desync |
| `0xE2` | `BADLEN` | payload length is wrong for this opcode |
| `0xE3` | `VERSION` | hello major mismatch |
| `0xE4` | `NOHELLO` | an op arrived before a successful hello |
| `0xE5` | `REHELLO` | a second hello |
| `0xE6` | `TOOBIG` | frame exceeds `max_payload` |
| `0xE7` | `SHUTDOWN` | the broker is shutting down |

`0x7F` and `0xFF` are **reserved and never transmitted** — they are the
no-broker sentinels of §3.2.

**The error predicate is `status > 0x01`**, not `status != 0`. Giving `END` its
own code rather than signalling it as OK with a zero length costs one concept
and buys a great deal: after a `read` the program branches on one byte without
parsing a length, and "end of stream" is distinguishable from "you asked for
nothing". The same code ends a `readdir`, so end-of-file and end-of-directory
are one idea and one test.

An unknown opcode is deliberately **not** fatal. Because every frame is length
prefixed, the broker resynchronises by consuming `len` bytes and replies
`NOSUCHOP`. That is the forward-compatibility path for a program built against
a newer minor version, and making it fatal would destroy it.

### 4.1 Nothing chosen by an OS header appears in a frame

This is a normative rule, not a style preference, and the platform-parity tier
enforces it. `AF_INET6` is 28 on FreeBSD and 10 on Linux. `O_CREAT` is `0x0200`
and `0x0040`. `SIGUSR1` is 30 and 10. Families, socket types, open flags, poll
events, file kinds, signal numbers and error codes are all brainstem's own,
mapped at the platform seam and dying there.

---

## 5. Handles

A handle is `u32 LE`, and it is **not** an OS file descriptor.

```
handle = (generation << 16) | index
```

`index` is the slot, allocated lowest-free-first; `generation` increments each
time a slot is reused. Handle `0` is never issued, and `0xFFFFFFFF` means
"none" where an op accepts an absence.

The generation counter exists for a specific defect: the program closes a
handle, the OS recycles the underlying descriptor for the next `open`, and a
stale handle silently reads someone else's file. With generations that is
`BADF`. It is also the indirection that lets a Windows `SOCKET` sit behind the
same integer later.

Lowest-free-first allocation is required, not incidental: it makes the handle
sequence a deterministic function of the program's own calls, which is what
lets a trace be replayed and compared across platforms.

Every handle has a **kind** — file, directory, socket, listener, pipe end,
process — checked before the op runs, so `accept` on a regular file is `BADF`
rather than something stranger further down.

---

## 6. The address record — 32 bytes, every family

No `sockaddr` ever crosses the wire. FreeBSD's `sockaddr_in` has a leading
`sin_len` byte and Linux's does not; the address families differ; the union
length differs by family. None of that is a program's problem.

| off | width | field |
|---|---|---|
| 0 | u8 | `family` — 0 none, 1 IPv4, 2 IPv6, 3 Unix |
| 1 | u8 | reserved, 0 |
| 2 | u16 LE | `port` — **host order**; the broker byte-swaps |
| 4 | u8[28] | `body`, family dependent |

For IPv4, bytes 4–7 are the quad **in reading order** — `127.0.0.1` is
`7F 00 00 01` — and 8–31 are zero. For IPv6, bytes 4–19 are the sixteen octets
in reading order and 20–31 are zero. For Unix, byte 4 is a directory
handle index, byte 5 is a path length 1–26, and bytes 6–31 are the relative
path zero-padded.

Two order decisions that look inconsistent and are not. The **port is
little-endian** like every other integer here; mixing byte orders inside one
record is the likeliest bug in the whole protocol, and swapping two bytes costs
the broker nothing. The **address is in reading order** because it is a byte
string, not an integer — `192.168.1.10` has no endianness, and writing it
backwards would be a legibility trap.

Twenty-four zero padding bytes look wasteful until you remember that **a zero
byte is free to emit in brainfuck** — `.` on a cell never touched — and they
buy a constant offset and a constant length in both directions. This is the
clearest case in the document where the brainfuck cost model inverts an
ordinary engineering instinct.

---

## 7. The operations

Shared shapes: `handle{u32 LE}`; `dir{u32 LE}` is a directory handle, or `0xFFFFFFFF` for the broker's working directory (§8); a
relative path resolves beneath; a path is `pathlen{u16 LE} ‖ path` with no NUL
terminator, no embedded NUL, and no `..` component.

| op | name | req | resp |
|---|---|---|---|
| `0x01` | `hello` | = 10 | = 48 + tables |
| `0x02` | `exit` | = 1 | = 0 |
| `0x03` | `clock_now` | = 1 | = 12 |
| `0x04` | `random_bytes` | = 2 | = n |
| `0x05` | `socket` | = 3 | = 4 |
| `0x06` | `connect` | = 38 | = 0 |
| `0x07` | `bind` | = 38 | = 32 |
| `0x08` | `listen` | = 6 | = 0 |
| `0x09` | `accept` | = 6 | = 36 |
| `0x0A` | `read` | = 8 | 0..n |
| `0x0B` | `write` | ≥ 6 | = 2 |
| `0x0C` | `close` | = 4 | = 0 |
| `0x0D` | `poll` | 11..389 | = 2 + 4·n |
| `0x0E` | `pipe` | = 2 | = 8 |
| `0x0F` | `spawn` | ≥ 12 | = 4 |
| `0x10` | `wait` | = 6 | = 4 |
| `0x11` | `open` | ≥ 11 | = 4 |
| `0x12` | `seek` | = 13 | = 8 |
| `0x13` | `stat` | ≥ 8 | = 32 |
| `0x14` | `readdir` | = 6 | 0 or ≥ 4 |
| `0x15` | `unlink` | ≥ 9 | = 0 |
| `0x16` | `mkdir` | ≥ 9 | = 0 |
| `0x17` | `rename` | ≥ 13 | = 0 |

Nineteen requests have an exact length. Anything else is `BADLEN`.

### 7.2 `exit` — `code{u8}`

The broker replies OK **first**, then closes the program's stdin, reaps the
interpreter, kills anything the program spawned, and exits with `code`.
Replying before exiting keeps invariant I2 with no special case, and I2 is the
whole deadlock proof.

### 7.3 `clock_now` — `clock_id{u8}` → `sec{u64} nsec{u32}`

`clock_id` is 0 realtime, 1 monotonic. Realtime is Unix epoch seconds, UTC;
there is no timezone concept in this ABI.

**Monotonic is normalised to zero at broker start.** That is a guarantee, not a
passthrough: raw monotonic time is time-since-boot on both platforms, and its
absolute value is machine state that would make replayed traces differ.

Seconds and nanoseconds are split so that neither side divides, and a program
wanting whole seconds reads eight bytes and discards four.

### 7.4 `random_bytes` — `n{u16}` → n bytes

`n` is 1–65535. Under a seed (§9) no syscall is issued at all.

### 7.5 `socket` — `domain{u8} type{u8} protocol{u8}` → `handle`

`domain` 1 IPv4, 2 IPv6, 3 Unix. `type` 1 stream, 2 datagram. `protocol` must
be 0.

**Family 3 Unix is declared and not built.** It answers `NOTSUP` on both
platforms, which is a different answer from `INVAL` and deliberately so: a
declared-but-absent family tells a program "not here", a family that is not in
the ABI at all tells it "no such thing", and collapsing the two sends someone
looking at their own encoder.

The reason is the same one that keeps `RENAME_NOREPLACE` out of §7.23. A Unix
socket address in this ABI is a directory handle plus a relative
path, because there are no absolute paths here. FreeBSD has `bindat(2)` and
`connectat(2)`, which take exactly that. Linux has neither, and the
workarounds — `fchdir` around the call, or `/proc/self/fd/N` — are
respectively racy and Linux-only. An op needing a different mechanism on the
primary and the secondary platform is the thing this ABI will not ship. The
record layout stays specified so a later minor version can fill it in. brainstem's own numbers, necessarily. Sockets are created blocking and
close-on-exec; blocking is a per-call flag, never socket state, so there is no
`fcntl` op and no hidden mode.

### 7.6–7.9 `connect` `bind` `listen` `accept`

`connect`: `handle ‖ flags{u16} ‖ addr{32}`, flags bit 0 `NOWAIT`.
`bind`: same shape, flags bit 0 `REUSEADDR`; **replies with the address
actually bound**, which is what makes port 0 usable — there is no
`getsockname` op, so without this an ephemeral port could never be discovered.
`listen`: `handle ‖ backlog{u16}`, 0 meaning 128.
`accept`: `handle ‖ flags{u16}` → `handle ‖ peer{32}`, flags bit 0 `NOWAIT`.

Non-blocking connect, since there is no `getsockopt`: issue `connect` with
`NOWAIT`; on `AGAIN`, `poll` for writable; then **re-issue the identical
connect**. The broker recognises the in-progress state and reports the result.

Datagram sockets are usable only after `connect` — there is no `sendto` or
`recvfrom` in v1, so an unconnected receiver cannot learn its peer.

### 7.10 `read` — `handle ‖ n{u16} ‖ flags{u16}` → up to n bytes

Flags bit 0 `NOWAIT`. **`END` means end of stream**; `AGAIN` means nothing was
available. The two being distinct codes is exactly what a poll loop needs.

### 7.11 `write` — `handle ‖ flags{u16} ‖ data` → `nwritten{u16}`

**The broker retries short writes internally**, so on OK without `NOWAIT`,
`nwritten` always equals the data length. This is the single biggest kindness
in the ABI: a partial-write loop would need the program to subtract a 16-bit
count, re-slice its buffer at a computed offset and re-emit a header — and
re-slicing at an offset on a tape means pointer travel proportional to that
offset. `nwritten` is kept for the `NOWAIT` case, where partial is real.

Because I6 says an error carries no payload, a write that transfers some bytes
and then fails reports the failure and loses the count. That is accepted: a
program hitting a dead peer mid-write is abandoning the connection anyway.

### 7.12 `close` — `handle`

Closing a standard handle is permitted and permanent. Closing a closed handle is
`BADF`. The slot returns to the pool immediately and its generation
increments — required for determinism, not an implementation detail.

### 7.13 `poll` — see §7.13 below

Request: `timeout_ms{u32} ‖ nfds{u8} ‖ nfds × (handle{u32} ‖ want_read{u8} ‖
want_write{u8})`. Timeout 0 returns immediately, `0xFFFFFFFF` waits forever;
both extremes are the cheapest possible brainfuck values.

Response: `nready{u8} ‖ reserved{u8} ‖ nfds × (readable ‖ writable ‖ hup ‖
err)`, each byte exactly 0 or 1, **in the same order as the request**.

This is the flagship parseability decision. Brainfuck has no bitwise
instruction — bfsodium had to build `XOR8` out of eight halvings, and its
`rotl32` at 20 million instructions a call was the library's real bottleneck.
A POSIX `revents` bitmask would cost a bit decomposition per descriptor per
poll. **One byte per condition costs a bare loop and nothing else.** Four bytes
per descriptor instead of one is a trade the measured cost table says is not
even visible.

The response is positional, so a program never searches for a handle in the
reply — it walks its own request and the reply in lockstep. `nready` exists so
the common timeout case costs one test rather than `nfds`.

`nfds` is capped at 64. Backing call is `poll(2)` on both platforms,
deliberately not `epoll` or `kqueue`, which are the two most divergent
interfaces the kernels have.

### 7.14–7.16 `pipe` `spawn` `wait`

`pipe`: `flags{u16}` → `read_handle ‖ write_handle`.

`spawn` hoists its counts into a fixed prefix: `dir ‖ flags{u16} ‖ nfdmap{u8} ‖
nargv{u8} ‖ nenv{u8} ‖ reserved{u8}`, then `nfdmap` × (`child_fd{u8} ‖
handle{u32}`), then the path as `u16`-prefixed, then `nargv` then `nenv`
`u16`-prefixed strings. Replies with a **process handle**, never a pid — pids
differ between runs and platforms, and the wire must not.

Any child descriptor not named in the map is closed. The environment is
explicit and never inherited: a child gets exactly `nenv` variables. That is a
capability decision and it removes an obvious replay nondeterminism.

`wait`: `proc ‖ flags{u16}` → `state{u8} ‖ code{u8} ‖ signal{u8} ‖ reserved`.
`state` is 0 running, 1 exited, 2 signalled. **Signal numbers are brainstem's
own** — `SIGUSR1` is 30 on FreeBSD and 10 on Linux. After reaping, the handle
stays valid and repeats the cached status until closed, so `wait` is idempotent.

### 7.17–7.19 `open` `seek` `stat`

`open`: `dir ‖ oflags{u16} ‖ mode{u16} ‖ path` → `handle`. Flag bits are
brainstem's own: 1 READ, 2 WRITE, 4 CREATE, 8 EXCL, 16 TRUNC, 32 APPEND,
64 DIRECTORY, 128 NOFOLLOW, 256 NOWAIT. `mode` is the low nine permission bits,
the one place a raw POSIX number passes through, because those bits are
identical on both platforms and are what a human writes as `0644`.

`seek`: `handle ‖ offset{u64} ‖ whence{u8}` → `pos{u64}`. `whence` is 0 SET,
1 CUR_FWD, 2 CUR_BACK, 3 END_BACK, 4 END_FWD.

**There is no signed field anywhere in this ABI and `seek` is why the rule
survives.** A signed 64-bit offset would make every backwards seek an eight-limb
two's complement in brainfuck — negate eight cells and propagate a carry,
through an adder measured at tens of thousands of instructions. Splitting the
sign into `whence` makes it an unsigned magnitude and a different one-byte
constant, which costs the broker one negation and the program nothing.

`stat`: `dir ‖ flags{u16} ‖ path` → 32 bytes. A zero `pathlen` means stat the
handle itself. Reply:

| off | width | field |
|---|---|---|
| 0 | u8 | `type` — 0 unknown, 1 regular, 2 directory, 3 symlink, 4 fifo, 5 socket, 6 chardev, 7 blockdev |
| 1 | u8 | `readable`, 0 or 1, **advisory** |
| 2 | u8 | `writable`, advisory |
| 3 | u8 | `executable`, advisory |
| 4 | u64 LE | `size` |
| 12 | u64 LE | `mtime_sec` |
| 20 | u32 LE | `mtime_nsec`, may be 0 |
| 24 | u16 LE | `mode` |
| 26 | u8[6] | reserved |

What a program actually asks about a path is: do I read this or enumerate it,
how big a loop do I need, has it changed, is it worth trying. `dev`, `ino`,
`uid`, `gid`, `nlink`, `blocks`, `atime` and `ctime` answer none of those and
are dropped. **Birthtime is dropped for a platform reason**: FreeBSD has it
natively, Linux exposes it only through `statx` and only on some filesystems,
and a field reliable on the primary platform and a coin-flip on the secondary
is worse than no field. The six reserved bytes can restore `ino` later without
changing the record size.

The three access bytes are computed from the mode and the broker's own
identity with **no extra syscall**, which is why they are advisory: ACLs and
MAC labels can still refuse an open. The authoritative answer is to try it.
`faccessat` three times would be exact and would turn one request into four
syscalls, which is the largest audit violation available in this document.

### 7.20 `readdir` — `dir_handle ‖ flags{u16}`

Flags bit 0 is `REWIND`. The reply is either **zero bytes**, meaning `END`, or:

| off | width | field |
|---|---|---|
| 0 | u8 | `type`, the stat enum, **never 0** |
| 1 | u8 | `namelen`, 1–255 |
| 2 | u8[2] | reserved |
| 4 | `namelen` | `name` |

**One entry per call.** A batch would be a run of variable-length records the
consumer walks by repeatedly adding a length to a cursor — precisely the
operation brainfuck cannot do cheaply, and precisely what bfsodium's "conveyors,
not indices" was learned from. Against that, one extra round trip per entry,
measured in microseconds, against an interpreter that spends 10⁶–10⁹
instructions per call. **The round trip is free at this timescale.** There is no
trade-off here, only an apparent one.

`namelen` is `u8` because `NAME_MAX` is 255 on both platforms, so a `u16` would
be a wasted counter.

`.` and `..` are filtered out by the broker. They are unreachable anyway, and a
program that recursed into them would loop forever — the worst failure for
something that cannot be interrupted.

**`type` is never 0 from `readdir`.** Both kernels may report an unknown type;
the broker fills it in with a stat. That is a documented departure from
one-request-one-syscall and it is what makes the two platforms indistinguishable.

Directory order is filesystem dependent and differs across platforms.
`--sort-readdir` will normalise it and **does not exist yet**: sorting means
holding a whole directory at once, and there is no allocation on the ABI path,
so it needs a bounded design rather than an afternoon. It is M7. Until then a
program that cares about order must sort what it reads, and a *test* that
cares must use a directory with one entry in it — which is what
`bf/fs/roundtrip.poke` does, and why.

### 7.21–7.23 `unlink` `mkdir` `rename`

`unlink`: `dir ‖ flags{u16} ‖ path`, flags bit 0 `REMOVEDIR`. **There is no
`rmdir` op and this flag is why** — it is one syscall on both platforms, so a
separate opcode would be ABI surface for a single bit.

`mkdir`: `dir ‖ mode{u16} ‖ path`.

`rename`: `olddir ‖ newdir ‖ oldlen{u16} ‖ newlen{u16} ‖ oldpath ‖ newpath`.
Both lengths are hoisted into the fixed prefix so the program emits a fully
fixed 12-byte header then two flat runs — it knows both lengths before it
starts, so this costs nothing.

**No `NOREPLACE` flag.** Linux has `renameat2`; FreeBSD has no equivalent.
Exposing it would create an op that behaves differently on the primary and the
secondary platform.

---

## 8. Reachability

**A brainstem program sees the system its broker sees.** Paths are ordinary
paths: absolute, or relative to the broker's own working directory. A `dir`
field of `0xFFFFFFFF` — "no handle", §5 — means exactly that, and a real
directory handle means resolve beneath it, which is what `readdir` needs and
what a program walking a tree wants.

There is no capability gate on anything. `socket` succeeds, `connect` reaches
whatever the host routes to, `spawn` runs what it is told to run, and `open`
opens what its path names. **brainstem runs with the credentials of whoever
started it and confines nothing.** CONVENTIONS' named non-goals say the same
thing in one line: this is not a sandbox. Do not run brainfuck you did not
write.

### 8.0 What this replaced, and why

Version 1.0 of this document specified a **preopen model**: no absolute paths,
every filesystem op resolving beneath a directory named on the command line,
and "default policy is deny". It was removed before the ABI was frozen, and
the reasoning is recorded because the design was wrong in an instructive way.

It cost the project's central goal and bought nothing it claimed. brainstem
exists to make an operating system **visible to a brainfuck program**. A
preopen is reachable only through a handle INDEX that the operator and the
program must agree on out of band — and there was deliberately no
name-discovery op, because a brainfuck program cannot usefully compare
strings. So the one interface a program had to the outside world was the one
kind of coordination brainfuck is worst at.

A literal path is the opposite: `/etc/hostname` is fourteen bytes to emit, and
emitting literal bytes is the single thing brainfuck is good at. The cost
model that made `poll` return one byte per condition and `readdir` return one
entry per call points the same way here.

And it was never containment. Both the original §8 and the guide said so
explicitly — "a usability and determinism feature", "emphatically not a
security boundary" — while the mechanism claimed otherwise. Two sentences in
this document had drifted into describing a policy that the project's own
non-goals disclaimed, and one of them ("escape is refused by the kernel")
was never true at all.

What survives from it: rights still narrow through derived handles, so a file
opened read-only cannot be written; handles are still generation-tagged; and
the kernel still decides what the broker's credentials may touch.

### 8.1 The three standard handles

Every program starts with three, before its first frame, with no flag and no
command line:

| handle | is |
|---|---|
| 1 | the broker's stdin |
| 2 | the broker's stdout |
| 3 | the broker's stderr |

Writing to handle 2 prints. That is the whole interface, and it works whether
the broker's stdout is a terminal, a file or a pipe.

They are reported in the hello reply's handle table (§8.2) with their names
and their actual kinds, so a program that wants to know whether it is talking
to a terminal or a pipe can look rather than assume. A program that does not
care can just write.

This is the one thing a path cannot portably express — a descriptor the broker
was handed by a shell — which is why it is the one thing that is handed over.

### 8.2 The handle table — 12 bytes per entry

| off | width | field |
|---|---|---|
| 0 | u32 LE | `handle` |
| 4 | u8 | `kind` — 1 file, 2 dir, 3 pipe-read, 4 pipe-write, 5 listener, 6 socket, 7 tty, 0 other |
| 5 | u8 | `namelen`, or 0 if unnamed |
| 6 | u16 LE | `rights` |
| 8 | u8[4] | reserved |

`rights` bits: 1 READ, 2 WRITE, 4 SEEK, 8 CREATE, 16 DELETE, 32 LIST, 64 EXEC,
128 ACCEPT, 256 CONNECT. A derived handle gets its parent's rights intersected
with what the operation asked for, so **rights only ever narrow** — a file
opened read-only stays read-only however it is passed around. That is a
legibility property, not a containment one: nothing stops the program opening
the same path again with different flags.

---

## 9. Determinism

brainstem introduces the two things bfsodium was built to avoid. The sibling
keeps every run replayable by having neither — "randomness supplied as input,
never generated" — and a syscall broker cannot keep that rule. So it keeps the
**property** a different way: both sources stay, and both become steerable.

| flag | state | effect |
|---|---|---|
| `--seed HEX` | built | 32 hex characters, 16 bytes. Makes `random_bytes` reproducible. |
| `--clock live` | built | the default: the host's clocks |
| `--clock frozen[=EPOCH]` | built | every `clock_now` returns the same instant |
| `--clock virtual[=EPOCH][,step=NS]` | built | advances by `step` **per request**, not per wall-clock second |
| `--trace` | built | prints every frame, both directions, payload in hex, to stderr |
| `--sort-readdir` | built | a directory enumerates in byte order of its names |
| `--replay FILE` | built | answers every frame from a recorded `--trace`, with **no syscall on the ABI path** |

**A directory has no order**, and that is the third source of
nondeterminism after the clock and the generator. ext4 returns entries in
hash order and ufs in roughly creation order; neither is a property of the
program, so `readdir` is not reproducible across two machines without
`--sort-readdir`. The order it imposes is byte order on the name -- not a
collation, which would depend on a locale and so reintroduce exactly what it
removes.

**`--replay` answers from the recording and calls no handler.** Every
request the program emits is compared against the one recorded at that
position, and a mismatch is reported by frame number with both payloads. The
broker installs no standard handles, captures no clock origin and opens
nothing, so a frozen instant replayed under `--clock live` is still the
frozen instant -- which is how the suite checks the claim, rather than by
counting syscalls. Its exit status is about the replay, not about the
program: a recorded `exit 3` does not make a replay exit 3.

A malformed seed or clock spec is **refused, never repaired**. A seed that was
silently truncated or an epoch silently read as zero would make two runs differ
for a reason nothing in the output could show, which is the single failure this
section exists to prevent.

### 9.1 The generator is specified, not merely seeded

Under `--seed` the generator is an explicit construction and **not** a seeded
`arc4random`, because the bytes must be identical on both platforms. It is a
ChaCha20 keystream, stated here exactly so that it can be reproduced
independently:

```
key     = the 16 seed bytes, then 16 zero bytes
nonce   = 12 zero bytes
counter = 0, incrementing per 64 byte block
```

The keystream is consumed sequentially across every `random_bytes` call for the
life of the broker, so the output depends only on the seed and on how many
bytes have been asked for so far. A call for zero bytes consumes none.

This is specified rather than left to the implementation because bfsodium
implements ChaCha20 in brainfuck and verifies it against RFC 8439 with two
independent oracles — so the sibling library is an independent oracle for this
broker's RNG. Nothing else available here has that property.

For seed `000102030405060708090a0b0c0d0e0f`, the first sixteen bytes are
`82233aa0ca0a14573efd34e9a85da697`; for an all-zero seed the first
sixty-four are the published ChaCha20 test vector for a zero key, which is what
`--selftest` checks.

### 9.2 What a program can see

The hello reply (§3) carries `platform`, `clock_mode`, `rng_mode`,
`clock_step_ns` and the sixteen seed bytes, so a program can tell which world
it is in without being told out of band. The seed is echoed rather than hidden:
it is on the command line already, and echoing it is what lets a trace be
replayed from the trace alone.

### 9.3 Under a seed, the kernel is not consulted

`random_bytes` under `--seed` issues **no syscall at all**, and under
`--clock frozen` or `--clock virtual` neither does `clock_now`. This is pinned
per platform in `tests/syscalls/` and diffed by tier 10a on every run, so it is
a measurement rather than an intention.

### 9.4 The three that are easy to miss

Handle numbers are broker-assigned, dense and lowest-free-first; process
handles are dense and never a pid; directory order is normalised by
`--sort-readdir`. All three arrive with the ops that need them.

### 9.5 One honest leak

Under a virtual clock, a `poll` with a non-zero timeout still waits in real
time, because real readiness cannot be virtualised. On expiry the broker
advances the virtual clock by the requested timeout. A poll-heavy program is
therefore not fully time-deterministic; use timeout 0 or infinite where
strictness matters.

---

## 10. Blocking and deadlock

Two pipes, one process each end, both sides sometimes writing more than a pipe
buffer holds. That is the classic deadlock shape, and the protocol is safe only
because of I3.

**The proof.** Given strict half duplex, at every instant exactly one side is
writing and the other is blocked in a read that will drain it. A pipe write
blocks only when the buffer is full *and nobody is reading*; here the reader is
always reading. **Therefore no frame size and no pipe buffer size can deadlock
the protocol.** `max_payload` is a memory-budget decision — two fixed buffers,
no allocation on the ABI path — and not a safety one. Worth stating, because
the natural assumption is the opposite, and because FreeBSD's smaller default
pipe buffer would otherwise look alarming.

**What can still deadlock, and what bounds it:**

- **Interpreter output buffering** — §1.1. Not a protocol flaw, but it has the
  same signature, and it is the one that will actually happen to somebody.
- **Program self-deadlock.** A program that owns both ends of a pipe and writes
  more than the buffer holds blocks the broker's write loop, and can never
  issue the read that would drain it. Unavoidable in any synchronous ABI.
  Bounded by `NOWAIT`, by `poll`, and by `--op-timeout` as the backstop that
  turns a hung CI job into a failed test. FreeBSD's smaller pipe buffer makes
  this *more* reachable, not less.
- **A spawned child filling a pipe nobody drains.** Same shape, same bounds.

The broker ignores `SIGPIPE`, uses raw `read`/`write` with no stdio, and treats
EOF mid-frame as a truncated frame rather than a short one.

**There is deliberately no mid-frame timeout by default.** A program may compute
for a long time between two bytes of one payload — whole bfsodium primitives
run to hundreds of millions of instructions — so a default mid-frame deadline
would kill correct programs. `--frame-timeout` exists and defaults to off.

The broker is single threaded and processes one frame at a time. There is no
pipelining and no request id, which is why the header needs no correlation
field. Adding pipelining would be a major version change.

---

## 11. Where one request is not one syscall

The audit tier whitelists these by name rather than discovering them:

| op | syscalls | why |
|---|---|---|
| `hello` | 0 | pure broker state |
| `exit` | teardown | broker lifecycle |
| `write` | `write` × k | internal retry loop, §7.11 |
| `bind` | `bind` + `getsockname` | so an ephemeral port is discoverable |
| `socket` | + `setsockopt` if `REUSEADDR` | there is no `setsockopt` op |
| `readdir` | + a stat when the kernel says unknown | type normalisation, §7.20 |
| `spawn` | fork + dup × k + exec | irreducible |
| `random_bytes` | × k, or **0** when seeded | short-read loop |
| blocking ops | + `poll` when `--op-timeout` is set | |

`stat` is deliberately one syscall, which is the whole reason its access bytes
are computed rather than fetched.
