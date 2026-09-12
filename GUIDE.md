# Writing brainfuck that does something

This is the guide for someone who wants to *use* brainstem: run the broker,
write a program against it, and work out why it is not doing what you meant.

[ABI.md](ABI.md) is the reference you come back to for a field width. This
document is the one you read once, in order.

> **All of this runs.** Every one of the twenty three operations is built and
> `ABI.md` is frozen at 1.0. Every worked example below is a fixture the suite
> executes on `freebsd-15.1` and on `ubuntu-26.04`, and the suite checks that
> the text here is still the text of the fixture — so an example that had
> drifted would fail a build rather than waste your afternoon.

---

## 1. The idea

Brainfuck has two I/O instructions. `,` reads one byte from standard input and
`.` writes one byte to standard output. That is the entire interface to the
world, and it is why almost every attempt to make brainfuck useful has added
an instruction.

brainstem adds nothing. You run your program under a broker, the broker owns
the other end of those two pipes, and you talk to it. Emit the right bytes with
`.` and the broker opens a socket for you; read the reply with `,`.

Your program is still standard brainfuck. Run the same file under any ordinary
interpreter and it reads end-of-input, does nothing, and exits — because there
is nobody on the other end. That property is the point, and §7 is about keeping
it.

---

## 2. The four things that will trip you up first

Read these before you write anything. Each of them has cost somebody an
afternoon, and the fourth cost this project a defect that shipped.

### Your stdin and stdout are gone

They carry the protocol. `.` does not print; it sends a byte to the broker. `,`
does not read the keyboard; it receives a byte from the broker.

If you want to print something, you ask the broker for a handle on a terminal
and you `write` to it:

```sh
brainstem -- ./bfi hello.bf
```

That hands you *brainstem's* stdout — the terminal — as a handle. Writing to
it is an op like any other.

**A consequence worth stating plainly: an existing brainfuck program cannot be
run under brainstem unmodified.** A program that prints "Hello World" with `.`
will, under the broker, send eleven bytes of garbage that look like a malformed
frame, and be told so. brainstem is a target you write for, not a wrapper you
put around things.

### Handle 0 is not stdin

**Handle 0 is never issued at all**, and that is deliberate: a brainfuck cell
you forgot to fill holds zero, and zero being invalid turns the commonest bug
in this whole protocol into a loud refusal instead of an accidental write to
something.

The broker's stdin, stdout and stderr are handles **1, 2 and 3**, always. Your
first `open`, `socket` or `pipe` comes back as 4.

So: to print something, `write` to handle **2**. Not to handle 1, and not to
handle 0.

### Your interpreter must not buffer its output

This is the one that produces a mystery. If your interpreter uses ordinary C
stdio on a pipe, it gets 4 KiB of buffering: your request sits in that buffer,
the broker waits for a request that never arrives, you wait for a reply that
cannot come, and nothing happens, forever, with no output and no error.

brainstem checks rather than hangs:

```sh
brainstem --check-interpreter ./my-interpreter
```

and if a handshake never arrives it says so and names buffering as the likely
cause. The vendored `tools/bfi` is already fixed. For an interpreter you cannot
change, `stdbuf -o0` usually does it.

### A read returns *up to* n bytes

Ask for eight and you may get two. That is not a brainstem rule, it is what
reading from a stream means everywhere — a pipe or a socket hands over
whatever has arrived, and how much has arrived is a scheduling question about
some other process.

**So a read of one byte is the only read that is deterministic.** It blocks
until there is a byte and then returns exactly it. If you want a known number
of bytes from a pipe or a socket, you loop:

```
EMIT 0a 08 00 06 00 00 00 01 00 00 00
READ 4
EMIT 0a 08 00 06 00 00 00 01 00 00 00
READ 4
```

If you ask for eight and then read a reply sized for the two you expected, the
conversation **desyncs**, and a desync does not look like a wrong answer. It
looks like a hang: your program waits for bytes that were never sent, the
broker waits for a request that never comes, and nothing is printed.

This is the fourth item here because both `bf/net/loopback.poke` and
`bf/proc/drive.poke` had it wrong, shipped, and passed every test for two
milestones. Forty runs under deliberate CPU load on a quiet eight-core machine
got the convenient answer forty times. A loaded single-processor guest did not.
It is the hardest bug in this document to find and the easiest to avoid.

A file is different: `read` on a regular file returns what you asked for up to
end of file, so the worked example in §6.4 reads eight and gets two only
because the file is two bytes long.

---

## 3. Running the broker

```
brainstem [options] -- INTERPRETER PROGRAM.bf
```

Everything after `--` is the command that runs your program. brainstem does not
interpret brainfuck itself and does not care which interpreter you use, as long
as it satisfies §2.3.

The options, and **when each one became real**. The `since` column is not
decoration: a guide that documents a flag the binary does not have is worse
than one that documents nothing, because you will spend an afternoon deciding
your quoting is wrong. `tests/run.sh` checks this table against the argument
parser in both directions, so a row marked `now` is a flag that exists and a
row marked for a later milestone is one that is still refused.

| option | since | what it does |
|---|---|---|
| `--trace` | now | print every frame, both directions, payload in hex, to stderr |
| `--seed HEX` | now | 32 hex characters — makes `random_bytes` reproducible |
| `--clock SPEC` | now | `live`, `frozen[=EPOCH]`, or `virtual[=EPOCH][,step=NS]` |
| `--hello-timeout MS` | now | turn a handshake hang into an error; default 5000, 0 disables |
| `--op-timeout MS` | now | the same for every later frame; default 30000 |
| `--interp PATH` | now | the interpreter, if you would rather not write `--` |
| `--check-interpreter PATH` | now | probe an interpreter for the one property §2.3 needs |
| `--dump-abi` | now | print the op table, one row per line |
| `--selftest` | now | the broker's own checks |
| `--sort-readdir` | now | enumerate a directory in byte order of its names |
| `--replay FILE` | now | answer every frame from a recorded `--trace`, touching nothing |

**There are no flags for reaching things, and that is the design.** Your
program sees the system the broker sees: paths are absolute or relative to the
broker's working directory, sockets connect where you tell them to, and
`spawn` runs what you name. An earlier version of brainstem made you preopen
every directory on the command line; that was removed, because it meant your
program could only reach things through a handle index you and the operator
had agreed on out of band — and your program cannot compare strings, so it had
no way to discover anything. ABI.md §8.0 has the full reasoning.

**So: it is not a sandbox.** brainstem runs with your credentials and confines
nothing. Do not run brainfuck you did not write.

### You always have three handles

Before your first frame, whatever the command line said:

| handle | is |
|---|---|
| 1 | the broker's stdin |
| 2 | the broker's stdout |
| 3 | the broker's stderr |

**Write to handle 2 to print something.** No flag, no preopen, no path. The
hello reply names all three and reports what they actually are — a terminal, a
file, a pipe — so you can check if you care.

Your first `open`, `socket` or `pipe` therefore comes back as handle 4.

### Making a run repeatable

Three things in this ABI can differ between two runs of the same program, and
each has a knob:

```sh
brainstem --seed 000102030405060708090a0b0c0d0e0f \
          --clock virtual=1700000000,step=1000000 \
          --trace -- ./build/bfi prog.bf
```

Under those two flags the trace is a fixed string of bytes. `--seed` replaces
the host's randomness with a ChaCha20 keystream computed in the broker — no
syscall is issued at all, which you can see for yourself in
`tests/syscalls/` — and `--clock virtual` advances time by `step` **once per
request**, so elapsed time tracks work done rather than wall time. A loop that
waits for the clock to change still terminates, which is why the virtual clock
is usually the one you want and `frozen` is for when you need an instant that
never moves.

The seed is echoed back in the hello reply, all sixteen bytes, so a program can
see which world it is in without being told out of band.

The third is the one people forget: **a directory has no order.** `readdir`
gives you whatever the filesystem feels like — ext4 hashes the names, ufs
returns roughly creation order — so a program that walks a directory does
something different on two machines through no fault of its own. `--sort-readdir`
makes the order byte order on the name, and nothing else changes:

```sh
brainstem --sort-readdir --trace -- ./build/bfi walk.bf
```

It costs a full rescan of the directory per entry, because the broker
allocates nothing and so holds one name rather than all of them. For the
directory sizes a brainfuck program will walk that is not a cost you can
measure; if it ever is, do not turn it on.

### Running a trace again

Capture a trace and you can replay it:

```sh
brainstem --trace -- ./build/bfi prog.bf 2>run.txt
brainstem --replay run.txt -- ./build/bfi prog.bf
```

The second command answers every frame out of `run.txt` instead of doing any
of it. Nothing is opened, no clock is read, no socket is made — so this is how
you find out whether a change to your program changed the conversation, and
**where**: a divergence is reported by frame number with both payloads in hex,
which is a much better answer than a diff of two trace files.

It is also how you check that your program is deterministic at all. If the
same `.bf` replays against its own recording, it asked for the same things in
the same order.

Two things to know. The exit status is about the replay, not about your
program — a recorded `exit 3` replays as success, because the question is
whether it still happens. And the traces in `tests/trace/` are **normalised**:
they have `%%` and `H` where masked bytes were, so they can be compared across
machines. Those cannot be replayed, and brainstem says so rather than
pretending to.

---

## 4. Emitting a byte

Everything below rests on this. To send the byte `0x05`:

```brainfuck
[-]+++++.
```

Clear the cell, add five, write it. To send `0x00`, you do not even need the
additions — `.` on a cell that has never been touched emits zero.

**That is why the protocol looks the way it does.** Zero is free, so every
default in the ABI is zero: unused flag bytes, padding in the address record,
the commonest member of every enum. A 32-byte address record with 24 zero bytes
of padding costs you 24 characters and buys a constant offset.

For a run of bytes it is cheaper to move along the tape than to clear and
rebuild, so the usual shape is one cell per byte:

```brainfuck
[-]+++++.>[-]++.>.
```

which sends `05 02 00`. In practice you will write this in a skeleton and let
`tools/bfgen` expand it (§8).

---

## 5. A frame, by hand

Every request is three bytes of header and then a payload:

```
op{1}  len{2, little-endian}  payload{len}
```

Every reply has the same shape with a status byte where the op was.

### The handshake

`hello` is op `0x01`. Its payload is exactly ten bytes: the magic `BSTM`, then
the major version, then the minor, then two reserved bytes.

```
01              op = hello
0a 00           len = 10
42 53 54 4d     "BSTM"
01 00           want major 1
00 00           want minor 0
00 00           reserved
```

In brainfuck, emitting that is thirteen literal bytes:

```brainfuck
; op = 0x01 hello
[-]+.
; len = 10, little endian
[-]++++++++++.
[-].
; magic "BSTM"
[-]++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++.
...
```

which is exactly the drudgery §8 exists to remove. You write
`EMIT 01 0a 00 42 53 54 4d 01 00 00 00 00 00` and the expander produces the
above. That line is the one in `bf/ctl/hello.poke`, and the suite checks that
it still is.

### Reading the reply

The reply to `hello` is a status byte, a two-byte length, and a 48-byte record
followed by two tables. The shape of every reply parse is the same:

1. read the status byte
2. read two length bytes
3. read that many bytes

**Before you read the status, set that cell to `0x7F`.** This is the trick that
tells you whether a broker is there at all, and it is worth understanding
because the failure it prevents is silent.

Interpreters disagree about what `,` does when there is no more input: some
leave the cell alone, some store 0, some store 255. If you seeded the cell with
`0x7F` and it is still `0x7F`, nothing was read. If it holds `0xFF`, nothing was
read either — `0x7F` and `0xFF` are both reserved and never sent as a status.
The awkward case is an interpreter that stores 0, because 0 is `OK` — so you
also check that the length is not zero, and that the first four payload bytes
are `BSTM`. The hello reply is guaranteed non-empty precisely so this check
works.

Get this right once, put it in your skeleton library, and never think about it
again.

---

## 6. Eight worked programs

These are not written out here by hand. **They are the fixtures the test suite
runs**, quoted verbatim, and `tests/run.sh` checks that what appears
below is byte for byte what is in `bf/`. A guide whose examples are typed out
separately from the examples that run is a guide with two versions of the
truth, and the printed one is always the one that rots.

### Say hello and exit

The smallest complete brainstem program, and the one that proves the whole
idea: a file containing nothing but the eight brainfuck instructions, running
under a general purpose interpreter, reaching an operating system.

<!-- bf/ctl/hello.poke -->
```
# hello.poke -- shake hands with the broker and exit cleanly.
#
# The smallest complete brainstem program, and the one that proves the whole
# idea: a file containing nothing but the eight brainfuck instructions,
# running under a general purpose interpreter, reaching an operating system.
#
# Frames transcribed by hand from ABI.md sections 3 and 7.2.
#
#   hello  op 01, len 10, magic "BSTM", want major 1, want minor 0, flags 0
EMIT 01 0a 00 42 53 54 4d 01 00 00 00 00 00
#   status{1} len{2} then the 104 byte record: the 48 byte prefix, three
#   twelve byte handle entries, and twenty bytes of name tail
READ 107
#   exit  op 02, len 1, code 0
EMIT 02 01 00 00
#   exit still answers, because every request gets exactly one reply --
#   that invariant is the whole deadlock proof, so it has no exceptions
READ 3
```

Run it:

```sh
sh tools/bfgen.sh bf/ctl/hello.poke > hello.bf
brainstem -- ./build/bfi hello.bf ; echo "exit status $?"
```

### Ask for the time

<!-- bf/time/clock.poke -->
```
# clock.poke -- ask for both clocks, then leave.
#
# The first fixture that crosses the platform seam. Under --clock frozen or
# --clock virtual its trace is a fixed string of bytes, which is what makes
# it the fixture the determinism and platform parity tiers are pinned on:
# the reply to a clock is otherwise the one thing in this protocol that
# cannot be the same twice.
#
# Frames transcribed by hand from ABI.md sections 3, 7.3 and 7.2.
#
#   hello  op 01, len 10, magic "BSTM", want major 1, want minor 0, flags 0
EMIT 01 0a 00 42 53 54 4d 01 00 00 00 00 00
READ 107
#   clock_now  op 03, len 1, clock id 0 = realtime
EMIT 03 01 00 00
#   reply is status{1} len{2} sec{8} nsec{4}: fifteen bytes
READ 15
#   clock_now  op 03, len 1, clock id 1 = monotonic, which is zero at broker
#   start rather than time since boot -- see sys.h
EMIT 03 01 00 01
READ 15
#   exit  op 02, len 1, code 0
EMIT 02 01 00 00
READ 3
```

```sh
brainstem --clock frozen=1700000000 -- ./build/bfi clock.bf
```

Monotonic time starts at zero when the broker starts rather than at boot, so
the first four bytes of `sec` are all you need unless your program runs for
136 years. Seconds and nanoseconds are separate fields so that **neither side
divides** — a brainfuck program dividing a 64 bit value is not a thing anyone
should have to write.

### Ask for random bytes

<!-- bf/rand/bytes.poke -->
```
# bytes.poke -- sixteen random bytes, then none at all.
#
# Under --seed this is the fixture the determinism tier is pinned on, and
# the bytes it receives are a ChaCha20 keystream the sibling library can
# compute in brainfuck -- which is why ABI.md section 9 specifies the
# construction rather than saying "some PRNG".
#
# The second request asks for ZERO bytes. That is legal and returns an empty
# OK: a program computing a length that happens to come out zero should not
# have to special case it, and the frame carries its own length so an empty
# reply is unambiguous. It also must not consume any keystream, which the
# pinned trace is what proves.
#
# Frames transcribed by hand from ABI.md sections 3, 7.4 and 7.2.
#
#   hello  op 01, len 10, magic "BSTM", want major 1, want minor 0, flags 0
EMIT 01 0a 00 42 53 54 4d 01 00 00 00 00 00
READ 107
#   random_bytes  op 04, len 2, n = 0x0010 = 16, little endian
EMIT 04 02 00 10 00
#   reply is status{1} len{2} then sixteen bytes: nineteen
READ 19
#   random_bytes  op 04, len 2, n = 0
EMIT 04 02 00 00 00
#   status 00, len 0, and nothing after it
READ 3
#   exit  op 02, len 1, code 0
EMIT 02 01 00 00
READ 3
```

```sh
brainstem --seed 000102030405060708090a0b0c0d0e0f -- ./build/bfi bytes.bf
```

Under that seed the sixteen bytes are always
`82233aa0ca0a14573efd34e9a85da697`, on either platform, because the generator
is a ChaCha20 keystream rather than the host's. Without `--seed` they come
from the kernel and differ every run.

### Make a file, write it, read it back

The M4 fixture, and the one to read if you are about to write anything that
touches a filesystem. Watch the handles: the first `open` gets handle 2, it is
closed, and the second `open` gets the same slot back with a new generation,
so its handle is `0x00010002`. That is §5 made visible — the old handle is not
merely stale, it is *unforgeable*.

<!-- bf/fs/roundtrip.poke -->
```
# roundtrip.poke -- make a file, write it, read it back, rename it, remove it.
#
# The M4 fixture, rewritten when the preopen model was removed. Every path
# here resolves the way it would for any other process: the directory field is
# ffffffff, "no handle", which means the broker's own working directory. The
# suite runs it with that set to a scratch directory, so the trace is a fixed
# string of bytes without the fixture knowing where it is.
#
# WATCH THE HANDLES. Handles 1, 2 and 3 are always the broker's stdin, stdout
# and stderr, so the first thing this program opens is handle 4. It opens and
# closes that slot three times, and each time the generation advances: 4, then
# 0x00010004, then 0x00020004. That is ABI.md section 5 made visible -- a
# handle the program closed is not merely stale, it is unforgeable.
#
# readdir is the one op that still needs a real directory handle, because the
# walk lives on it. So the program opens "." to get one, which is also the
# shortest demonstration of the new model: a literal path, and no coordination
# with whoever started the broker.
#
# Frames transcribed by hand from ABI.md sections 3, 5, 7.10 to 7.23 and 8.
#
#   hello  op 01, len 10, magic "BSTM", want major 1, want minor 0, flags 0
EMIT 01 0a 00 42 53 54 4d 01 00 00 00 00 00
#   3 header + 48 record + 36 of handle table + 20 of name tail
READ 107
#   mkdir  op 16, dir ffffffff, mode 0755 = 0x01ed, path "sub"
EMIT 16 09 00 ff ff ff ff ed 01 73 75 62
READ 3
#   open  op 11, flags WRITE|CREATE|TRUNC = 0x0016, mode 0644 = 0x01a4,
#   path "f".  The reply is a status, a length, and a four byte handle: 4.
EMIT 11 09 00 ff ff ff ff 16 00 a4 01 66
READ 7
#   write  op 0b, handle 4, flags 0, data "hi".  The reply is nwritten, which
#   on OK without NOWAIT is always the length asked for -- the broker retried
#   any short write itself.
EMIT 0b 08 00 04 00 00 00 00 00 68 69
READ 5
#   close  op 0c, handle 4.  The slot is free and its generation has moved on.
EMIT 0c 04 00 04 00 00 00
READ 3
#   open  op 11, flags READ = 0x0001, path "f".  Same slot, next generation:
#   the handle comes back as 0x00010004.
EMIT 11 09 00 ff ff ff ff 01 00 00 00 66
READ 7
#   read  op 0a, handle 0x00010004, n 8, flags 0.  Two bytes come back.
EMIT 0a 08 00 04 00 01 00 08 00 00 00
READ 5
#   seek  op 12, handle 0x00010004, offset 0, whence 0 SET.  The reply is the
#   new position as eight bytes.
EMIT 12 0d 00 04 00 01 00 00 00 00 00 00 00 00 00 00
READ 11
#   close  op 0c, handle 0x00010004
EMIT 0c 04 00 04 00 01 00
READ 3
#   stat  op 13, dir ffffffff, flags 0, path "f".  Thirty two bytes.
EMIT 13 07 00 ff ff ff ff 00 00 66
READ 35
#   rename  op 17, olddir ffffffff, newdir ffffffff, oldlen 1, newlen 1, "f"
#   then "g".  Both lengths are in the fixed prefix, so this is a twelve byte
#   header and two flat runs.
EMIT 17 0e 00 ff ff ff ff ff ff ff ff 01 00 01 00 66 67
READ 3
#   unlink  op 15, dir ffffffff, flags 0, path "g"
EMIT 15 07 00 ff ff ff ff 00 00 67
READ 3
#   open  op 11, flags READ|DIRECTORY = 0x0041, path ".".  The slot comes back
#   a third time, as 0x00020004, and this one is a directory handle because
#   readdir needs somewhere to keep the walk.
EMIT 11 09 00 ff ff ff ff 41 00 00 00 2e
READ 7
#   readdir  op 14, handle 0x00020004, flags 0.  Only "sub" is here, so this
#   is one entry of type 2 directory, name length 3.
EMIT 14 06 00 04 00 02 00 00 00
READ 10
#   readdir again: the directory is exhausted and the reply is empty, which
#   is status END and a zero length rather than a payload to parse
EMIT 14 06 00 04 00 02 00 00 00
READ 3
#   close  op 0c, handle 0x00020004, which closes the directory walk with it
EMIT 0c 04 00 04 00 02 00
READ 3
#   exit  op 02, len 1, code 0
EMIT 02 01 00 00
READ 3
```

```sh
mkdir -p work && cd work
brainstem -- ../build/bfi ../roundtrip.bf
```

### Talk to the network

The one that surprises people: this program **connects to itself**, with no
second process and no agreed port. `bind` replies with the address it actually
bound, so asking for port 0 and reading the answer is how you get an ephemeral
port — there is no `getsockname` op, and this reply is why you do not need one.

The two port bytes travel from that reply into the `connect` frame in **raw
brainfuck**, because they are a value and not a literal, and `bfgen` only ever
writes literals. That is the seam between what an expander can do for you and
what you have to do yourself, and it is worth reading the two `>,>,<<` and
`>.>.<<` lines carefully.

<!-- bf/net/loopback.poke -->
```
# loopback.poke -- bind an ephemeral port, listen, connect to it, accept,
# and send two bytes through the socket to itself.
#
# THIS FIXTURE NEEDS NO SECOND PROCESS, and that is the whole design. The
# plan for this milestone called for a netecho helper in tools/, so the tier
# would not depend on nc -- whose flags differ between the two guests. A
# program that talks to ITSELF removes the helper as well: no second binary,
# no port to agree on out of band, and nothing to leave running if the suite
# is interrupted.
#
# It is possible because bind REPLIES WITH THE ADDRESS ACTUALLY BOUND. There
# is no getsockname op, so that reply is the only way a program can learn the
# ephemeral port it was given -- and this fixture reads those two bytes and
# emits them straight back in the connect frame.
#
# THE PORT IS CARRIED IN RAW BRAINFUCK, because it is a value and not a
# literal, and bfgen only ever writes literals. Cell 0 is the working cell
# every EMIT and READ uses; the two raw blocks below park the port in cells 1
# and 2 and later read them out. bfgen passes raw brainfuck through untouched
# and forgets what it thought cell 0 held, which is exactly the contract that
# makes this safe.
#
# Frames transcribed by hand from ABI.md sections 3, 6, 7.5 to 7.9 and 7.10.
#
#   hello  op 01, len 10.  This program touches no filesystem at all; the
#   three handles it is given back are the broker's own stdio and it
#   ignores them.
EMIT 01 0a 00 42 53 54 4d 01 00 00 00 00 00
READ 107
#   socket  op 05, domain 1 IPv4, type 1 stream, protocol 0.  Handle 4 --
#   handles 1, 2 and 3 are always the broker's stdin, stdout and stderr.
EMIT 05 03 00 01 01 00
READ 7
#   bind  op 07, handle 1, flags 1 REUSEADDR, then the 32 byte address:
#   family 01, reserved 00, port 0000 meaning "any", then 127.0.0.1 in
#   READING ORDER and twenty four zero bytes.  Those zeros cost nothing to
#   emit -- '.' on a cell never touched -- and buy a constant offset.
EMIT 07 26 00 04 00 00 00 01 00
EMIT 01 00 00 00 7f 00 00 01
EMIT 00 00 00 00 00 00 00 00 00 00 00 00
EMIT 00 00 00 00 00 00 00 00 00 00 00 00
#   the reply is status, two length bytes, then the address bound.  Drain the
#   status, the length, the family and the reserved byte: five bytes.
READ 5
#   now the two port bytes, into cells 1 and 2, little endian as they lie
>,>,<<
#   and the remaining twenty eight bytes of the address record
READ 28
#   listen  op 08, handle 4, backlog 0 meaning 128
EMIT 08 06 00 04 00 00 00 00 00
READ 3
#   socket  op 05 again.  Handle 5, the client.
EMIT 05 03 00 01 01 00
READ 7
#   connect  op 06, handle 5, flags 0, family 01, reserved 00 ...
EMIT 06 26 00 05 00 00 00 00 00 01 00
#   ... then the port this program was given, straight out of cells 1 and 2 ...
>.>.<<
#   ... then 127.0.0.1 and twenty four zeros.
EMIT 7f 00 00 01
EMIT 00 00 00 00 00 00 00 00 00 00 00 00
EMIT 00 00 00 00 00 00 00 00 00 00 00 00
READ 3
#   accept  op 09, handle 4, flags 0.  The reply is a handle and the peer
#   address: 3 header + 4 + 32 = 39 bytes.  Handle 6.
EMIT 09 06 00 04 00 00 00 00 00
READ 39
#   write  op 0b, handle 5 (the client end), flags 0, "hi"
EMIT 0b 08 00 05 00 00 00 00 00 68 69
READ 5
#   read  op 0a, handle 6 (the accepted end), ONE BYTE AT A TIME.  A read
#   returns UP TO n bytes and a stream may deliver them in any grouping it
#   likes, so asking for two and assuming two is a race waiting for a busier
#   machine.  See drive.poke, where exactly that happened.
EMIT 0a 08 00 06 00 00 00 01 00 00 00
READ 4
EMIT 0a 08 00 06 00 00 00 01 00 00 00
READ 4
#   close all three, youngest first
EMIT 0c 04 00 06 00 00 00
READ 3
EMIT 0c 04 00 05 00 00 00
READ 3
EMIT 0c 04 00 04 00 00 00
READ 3
#   exit  op 02, len 1, code 0
EMIT 02 01 00 00
READ 3
```

**There is no capability gate on `socket`.** A program that can reach the
broker can reach the network — see ABI.md §8.1, which says so plainly and
explains why it is an open question rather than a decision.

### Drive another brainfuck program

The one this whole project exists for. Two pipes, a `spawn`, and a second
brainfuck program running under its own interpreter with those pipes as its
stdin and stdout.

**Watch the closes.** After `spawn`, the outer program closes its own copies
of the child's two ends. Without that the child never sees end of input and
the outer program never sees end of file, and the whole thing deadlocks with
no output and no core — the same failure shape §2.3 is about, arrived at from
the other direction.

**The environment is explicit.** A child gets exactly the variables you pass
and nothing else, not even the broker's. Here that is load-bearing rather than
tidy: the inner program is `,[.,]`, and under the interpreter's default `,` at
end of input leaves the cell unchanged, so its loop would never terminate. The
spawn passes `BFI_EOF=zero` and that is the whole of the child's environment.

<!-- bf/proc/drive.poke -->
```
# drive.poke -- brainfuck driving brainfuck.
#
# THE POINT OF THE WHOLE PROJECT. A file containing nothing but the eight
# brainfuck instructions creates two pipes, starts an interpreter on a SECOND
# brainfuck program with those pipes as its stdin and stdout, sends it two
# bytes, reads its answer, and collects its exit status.
#
# That is what makes brainfuck itself the harness: the sibling library's
# primitives can be chained by a brainfuck program rather than by a shell
# script, which is what the BoneMesh keyschedule conformance check needs.
#
# The suite runs it with the broker's working directory set to a scratch
# directory holding the interpreter and echo.bf, so "bfi" resolves there --
# the same way it would for any other process. Handles 1, 2 and 3 are the
# broker's own stdio, so the first pipe comes back as 4 and 5.
#
# WATCH THE CLOSES. After spawn, the outer program closes its own copies of
# the child's two ends. Without that the child never sees end of input on its
# stdin and the outer program never sees end of file on its stdout, and the
# whole thing deadlocks with no output and no core -- the same failure shape
# the hello timeout exists to diagnose, arrived at from the other direction.
#
# Frames transcribed by hand from ABI.md sections 3, 7.10 to 7.16.
#
#   hello  op 01, len 10.  104 bytes of record back.
EMIT 01 0a 00 42 53 54 4d 01 00 00 00 00 00
READ 107
#   pipe  op 0e, flags 0.  Handles 4 (read) and 5 (write): the outer program
#   writes into 5 and the child reads from 4.
EMIT 0e 02 00 00 00
READ 11
#   pipe again.  Handles 6 (read) and 7 (write): the child writes into 7 and
#   the outer program reads from 6.
EMIT 0e 02 00 00 00
READ 11
#   spawn  op 0f, len 0x35 = 53.
#     dir ffffffff meaning the broker's working directory, flags 0,
#     nfdmap 2, nargv 2, nenv 1, reserved 0
EMIT 0f 35 00 ff ff ff ff 00 00 02 02 01 00
#     the descriptor map: child fd 0 gets handle 4, child fd 1 gets handle 7.
#     ANY CHILD DESCRIPTOR NOT NAMED HERE IS CLOSED, so the child gets these
#     two and nothing else -- not even the broker's stderr.
EMIT 00 04 00 00 00
EMIT 01 07 00 00 00
#     the path, u16 prefixed: "bfi"
EMIT 03 00 62 66 69
#     argv: "bfi", "echo.bf"
EMIT 03 00 62 66 69
EMIT 07 00 65 63 68 6f 2e 62 66
#     env: exactly one variable, "BFI_EOF=zero".  The environment is explicit
#     and never inherited, so this is the whole of the child's environment --
#     and echo.bf needs it, because the interpreter's default leaves the cell
#     unchanged at end of input and its loop would never terminate.
EMIT 0c 00 42 46 49 5f 45 4f 46 3d 7a 65 72 6f
#     the reply is a process handle: 8
READ 7
#   close the outer copies of the child's two ends.  This is the step that is
#   easy to forget and impossible to debug.
EMIT 0c 04 00 04 00 00 00
READ 3
EMIT 0c 04 00 07 00 00 00
READ 3
#   write  op 0b, handle 5, flags 0, "hi" -- into the child's stdin
EMIT 0b 08 00 05 00 00 00 00 00 68 69
READ 5
#   close handle 5, so the child sees end of input and stops
EMIT 0c 04 00 05 00 00 00
READ 3
#   read  op 0a, handle 6 -- the child's stdout -- ONE BYTE AT A TIME.
#
#   A read returns UP TO n bytes, never exactly n, and this fixture asked for
#   eight and assumed two until a gate said otherwise. The child is an
#   interpreter with unbuffered output, so it emits 'h' and 'i' as two
#   separate one byte writes; whether both are in the pipe when the read
#   happens is a scheduling question. On a quiet machine the answer was always
#   two, which is the worst kind of always.
#
#   Asking for one byte is deterministic: a read of one blocks until there is
#   a byte and then returns exactly it. A program that wants a known number of
#   bytes from a STREAM has to loop, and this is what the loop looks like.
EMIT 0a 08 00 06 00 00 00 01 00 00 00
READ 4
EMIT 0a 08 00 06 00 00 00 01 00 00 00
READ 4
#   a third read: the child has exited and closed its end, so this is END with
#   an empty payload
EMIT 0a 08 00 06 00 00 00 01 00 00 00
READ 3
#   wait  op 10, handle 8, flags 0 -- blocking.  state 1 exited, code 0.
EMIT 10 06 00 08 00 00 00 00 00
READ 7
#   wait again: idempotent after reaping, from the cached status
EMIT 10 06 00 08 00 00 00 00 00
READ 7
#   close the pipe end and then the process handle.  Closing a process
#   handle stops tracking the child; it does not kill it.
EMIT 0c 04 00 06 00 00 00
READ 3
EMIT 0c 04 00 08 00 00 00
READ 3
#   exit  op 02, len 1, code 0
EMIT 02 01 00 00
READ 3
```

The inner program, which speaks no protocol at all:

<!-- bf/proc/echo.poke -->
```
# echo.poke -- the INNER program. Not a brainstem client at all.
#
# This is the one fixture in the tree that speaks no protocol: it reads bytes
# and writes them back until end of input, which is the classic brainfuck cat.
# It exists to be run BY another brainfuck program, through spawn, with its
# stdin and stdout wired to pipes the outer program created.
#
# It needs BFI_EOF=zero to terminate, and the outer program passes exactly
# that and nothing else -- see drive.poke. Under the interpreter's default,
# ',' at end of input leaves the cell holding the last byte read, and this
# loop would echo it forever.
,[.,]
```

### And every way it can say no

<!-- bf/fs/refused.poke -->
```
# fs/refused.poke -- the ways the filesystem ops say no, and one way they no
# longer do.
#
# The conversation CONTINUES THROUGH ALL OF THEM. Every status here is
# recoverable, so the error reply carries no payload, the program reads
# exactly three bytes, and the stream is still in step for the next request.
# An ABI where a refusal desynced the conversation would be one where a
# program could not afford to try anything.
#
# TWO REFUSALS WERE DELETED FROM THIS FIXTURE and one success was added in
# their place. An earlier version checked that ".." and an absolute path were
# both refused. They are not any more: brainstem exists to make the system
# visible to a brainfuck program, and refusing a literal path only ever cost
# reachability -- it never bought containment, because this was never a
# sandbox. The last case here stats "/" and expects it to WORK.
#
# Run with the broker's working directory set to a scratch directory.
#
# Frames transcribed by hand from ABI.md sections 4, 5, 7.10 to 7.23 and 8.
#
#   hello  op 01, len 10
EMIT 01 0a 00 42 53 54 4d 01 00 00 00 00 00
READ 107
#   open "nope" -- NOENT 05
EMIT 11 0c 00 ff ff ff ff 01 00 00 00 6e 6f 70 65
READ 3
#   open "f" with an undeclared flag bit 0x8000 -- INVAL 06.  A program built
#   against a later minor version must not silently get an open without the
#   flag it asked for.
EMIT 11 09 00 ff ff ff ff 00 80 a4 01 66
READ 3
#   open with an EMPTY path -- INVAL 06.  Silently meaning "." would make a
#   typo succeed.
EMIT 11 08 00 ff ff ff ff 01 00 00 00
READ 3
#   stat with an empty path AND no handle -- INVAL 06.  An empty path stats
#   whatever the handle is, and there is no handle here, so this is a question
#   about nothing.
EMIT 13 06 00 ff ff ff ff 00 00
READ 3
#   open "f" WRITE|CREATE|TRUNC, mode 0644 -- handle 4
EMIT 11 09 00 ff ff ff ff 16 00 a4 01 66
READ 7
#   read from it -- DENIED 04.  It was opened write only, and rights only ever
#   narrow: the handle never had READ to lose.
EMIT 0a 08 00 04 00 00 00 08 00 00 00
READ 3
#   readdir on it -- NOTDIR 09, not DENIED.  The handle is real and the kind is
#   wrong, and the most specific true answer is the useful one.
EMIT 14 06 00 04 00 00 00 00 00
READ 3
#   close it
EMIT 0c 04 00 04 00 00 00
READ 3
#   read handle 4 again -- BADF 03.  THE DEFECT THE GENERATION TAG EXISTS FOR.
#   The slot is free and may already have been handed to something else; the
#   old handle names a generation that no longer exists, so this is a clean
#   refusal instead of a silent read of someone else's file.
EMIT 0a 08 00 04 00 00 00 08 00 00 00
READ 3
#   close it again -- BADF 03
EMIT 0c 04 00 04 00 00 00
READ 3
#   unlink "f", leaving nothing behind for the next fixture that runs here
EMIT 15 07 00 ff ff ff ff 00 00 66
READ 3
#   open "/" READ|DIRECTORY = 0x0041 -- OK.  An ABSOLUTE PATH, which the
#   preopen model refused and this one does not: the system is visible.
#
#   It is an open rather than a stat ON PURPOSE.  The first version of this
#   case stat'd "/" and the tier 10 pin went red on a machine whose root
#   directory had a different size and mode -- a reply that depends on the
#   HOST is not a reply a parity tier can pin.  A handle is a reply that
#   depends only on the program: slot 4 was closed above, so this comes back
#   as 0x00010004.
EMIT 11 09 00 ff ff ff ff 41 00 00 00 2f
READ 7
#   close the root handle
EMIT 0c 04 00 04 00 01 00
READ 3
#   exit  op 02, len 1, code 0
EMIT 02 01 00 00
READ 3
```

Every status in that conversation is **recoverable**: the error reply carries
no payload, you read exactly three bytes, and the stream is still in step for
your next request. You can afford to try things.

---

## 7. Staying portable

brainstem's whole claim is that your program is ordinary brainfuck. Two habits
keep that true, and the test suite enforces both on its own fixtures.

**Use only the eight instructions.** No `#` debug output, no `!` input
separator, no dialect extensions. If your interpreter offers something clever,
it is a trap.

**Do not depend on `,` at end of input.** You cannot know which of the three
behaviours you will get, so never let a read past the end matter. The sentinel
trick in §5 is how you turn the ambiguity into a test instead of a bug. Read
exactly the bytes the length told you to read, and no more — `read exactly what
you were told` is the whole discipline.

You can check both habits with the suite's own tooling once it exists, and you
can check the second by running your program under all three conventions:

```sh
BFI_EOF=unchanged ./build/bfi prog.bf
BFI_EOF=zero      ./build/bfi prog.bf
BFI_EOF=minus1    ./build/bfi prog.bf
```

If the behaviour differs, you have a portability bug, and it is yours rather
than the broker's.

---

## 8. Not writing the plus signs by hand

Emitting `0x8f` means typing 143 `+` characters and counting them correctly.
Miscount by one and you have sent a *wrong byte*, which at the far end is
indistinguishable from a broker bug — and a raw `.bf` cannot carry a comment
explaining what it meant, because comments are not portable.

So brainstem's own fixtures are written as skeletons and expanded:

| directive | becomes |
|---|---|
| `EMIT <hex…>` | a delta-encoded run of `+`/`-` and `.` per byte |
| `READ n` | `,>` n times |
| `ECHO n` | `,.` n times |
| `LOOP` / `END` | `[` / `]` |

The expander **knows nothing about the ABI**. It has no op names, computes no
lengths, and cannot see ABI.md. Every hex byte in every skeleton is a number a
person read out of the specification and typed. That is deliberate: if the
fixtures were generated from the same table the broker parses with, a byte-order
bug would be invisible to every test.

You are welcome to write raw brainfuck instead. The expander is a convenience,
not a requirement, and the broker cannot tell the difference.

---

## 9. When it does not work

**Nothing happens at all.** Almost certainly output buffering (§2.3). Run
`--check-interpreter`.

**`PROTO` on your first frame.** Your opcode byte is probably zero — a cell you
forgot to set. Opcode 0 is permanently invalid precisely so this fails loudly
instead of being misread as a valid call.

**`BADLEN`.** Your length field disagrees with what you actually sent. Remember
it is little-endian: length 10 is `0a 00`, not `00 0a`.

**Everything after some point is wrong.** You under-read a reply. Whatever you
left in the pipe is being interpreted as the start of your next frame. The ABI
validates lengths strictly so this surfaces within a frame or two, but the
*cause* is always earlier than the symptom. Check that every reply you parse
consumes exactly the number of bytes its length announced.

**`DENIED` on something you expected to work.** The handle does not carry the
right you need — most often you opened a file read-only and then wrote to it.
Rights only ever narrow, so open it again with the flags you meant. Check the
feature bits in the hello reply too: they tell you which ops exist at all.

**The real debugging tool** is `--trace`:

```sh
brainstem --trace -- ./build/bfi prog.bf 2> run.log
```

which prints every frame in both directions, with its payload in hex. Since
the frames are the whole interface, the trace is the whole story of what your
program did -- which is also why the suite pins traces rather than output.

---

## 10. Where to go next

- [ABI.md](ABI.md) — every op, field by field. §7 is the op reference.
- [README.md](README.md) — what state the project is in.
- [CONVENTIONS.md](CONVENTIONS.md) and [HANDOFF.md](HANDOFF.md) — if you want
  to change brainstem rather than use it.
