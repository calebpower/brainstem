# Writing brainfuck that does something

This is the guide for someone who wants to *use* brainstem: run the broker,
write a program against it, and work out why it is not doing what you meant.

[ABI.md](ABI.md) is the reference you come back to for a field width. This
document is the one you read once, in order.

> **Nothing works yet.** brainstem is at milestone M0 — the test lanes and the
> documents exist, the broker does not. Everything below describes the system
> being built, and the examples are written against a specification rather than
> against a running program. When M2 lands, the first three of them will run.

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

## 2. The three things that will trip you up first

Read these before you write anything. Each of them has cost somebody an
afternoon.

### Your stdin and stdout are gone

They carry the protocol. `.` does not print; it sends a byte to the broker. `,`
does not read the keyboard; it receives a byte from the broker.

If you want to print something, you ask the broker for a handle on a terminal
and you `write` to it:

```sh
brainstem --preopen-fd out=1 -- ./bfi hello.bf
```

That hands you *brainstem's* stdout — the terminal — as a handle. Writing to
it is an op like any other.

**A consequence worth stating plainly: an existing brainfuck program cannot be
run under brainstem unmodified.** A program that prints "Hello World" with `.`
will, under the broker, send eleven bytes of garbage that look like a malformed
frame, and be told so. brainstem is a target you write for, not a wrapper you
put around things.

### Handle 0 is not stdin

There are no implicit handles. Handle numbers start at 1 and they are whatever
your `--preopen-*` flags created, in the order you wrote them. If you passed no
preopen flags, you have no handles at all, and the only ops that work are
`hello`, `exit`, `clock_now` and `random_bytes`.

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
| `--preopen-dir NAME=PATH` | M4 | a directory your relative paths resolve under |
| `--preopen-file NAME=PATH:MODE` | M4 | one file, mode `r`, `w`, `rw` or `a` |
| `--preopen-fd NAME=N` | M4 | inherit one of brainstem's own descriptors — this is how you get the terminal |
| `--sort-readdir` | M4 | normalise directory order |
| `--preopen-listen NAME=ADDR` | M5 | a bound, listening socket, ready to accept |
| `--preopen-connect NAME=ADDR` | M5 | an already-connected socket |
| `--replay FILE` | M7 | re-run against a recorded trace, with no syscalls at all |

**Nothing is reachable that you did not name.** There is no way to open a path
outside a preopened directory, and no ambient network access. That is a
usability and testing property — it makes runs reproducible and failures
local — and it is emphatically **not** a security boundary. brainstem runs with
your credentials. Do not run brainfuck you did not write.

### Making a run repeatable

Two things in this ABI can differ between two runs of the same program, and
both have a knob:

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

## 6. Three worked programs

These are not written out here by hand. **They are the three fixtures the test
suite runs**, quoted verbatim, and `tests/run.sh` checks that what appears
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
#   the reply is status{1} len{2} then the 48 byte record: 51 bytes, and
#   npreopen is 0 here so there is no name tail after it
READ 51
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
READ 51
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
READ 51
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

**Writing to a file or a terminal** needs `write` and a preopen, which arrive
at M4. Until then the broker's own `--trace` is how you see what your program
did.

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

**`DENIED` on something you expected to work.** You did not preopen it. Check
the feature bits in the hello reply — they tell you what is available before
you try.

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
