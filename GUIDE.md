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

The options you will actually use:

| | |
|---|---|
| `--preopen-dir NAME=PATH` | a directory your relative paths resolve under |
| `--preopen-file NAME=PATH:MODE` | one file, mode `r`, `w`, `rw` or `a` |
| `--preopen-fd NAME=N` | inherit one of brainstem's own descriptors — this is how you get the terminal |
| `--preopen-listen NAME=ADDR` | a bound, listening socket, ready to accept |
| `--preopen-connect NAME=ADDR` | an already-connected socket |
| `--trace FILE` | record every frame in both directions |
| `--seed HEX` | make `random_bytes` reproducible |
| `--clock virtual` | make `clock_now` reproducible |
| `--op-timeout MS` | turn a hang into an error |

**Nothing is reachable that you did not name.** There is no way to open a path
outside a preopened directory, and no ambient network access. That is a
usability and testing property — it makes runs reproducible and failures
local — and it is emphatically **not** a security boundary. brainstem runs with
your credentials. Do not run brainfuck you did not write.

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
`EMIT 01 0a 00 42 53 54 4d 01 00 00 00` and the expander produces the above.

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

### Say hello and exit

The smallest complete brainstem program. Handshake, then exit with status 0.

```
# hello.poke
# Frames transcribed from ABI.md sections 3 and 7.2.
#   hello : op 01, len 10, "BSTM", major 1, minor 0, reserved
EMIT 01 0a 00 42 53 54 4d 01 00 00 00
#   drain the reply: status, two length bytes, then 48 + tables.
#   npreopen is 0 here, so the tail is empty and the record is exactly 48.
READ 3
READ 48
#   exit : op 02, len 1, code 0
EMIT 02 01 00 00
```

Run it:

```sh
brainstem -- ./build/bfi hello.bf ; echo "exit status $?"
```

### Print something

```
# print.poke
EMIT 01 0a 00 42 53 54 4d 01 00 00 00
READ 3
READ 48
#   write : op 0b, len 6 + 5, handle 1, flags 0, "hello"
EMIT 0b 0b 00 01 00 00 00 00 00 68 65 6c 6c 6f
READ 3
READ 2
EMIT 02 01 00 00
```

```sh
brainstem --preopen-fd out=1 -- ./build/bfi print.bf
```

Handle 1 is the first preopen, which is the terminal. The `write` payload is
the handle as four little-endian bytes, two flag bytes, then the data; the
length `0x000b` is 6 + 5.

### Ask for the time

```
# clock.poke
EMIT 01 0a 00 42 53 54 4d 01 00 00 00
READ 3
READ 48
#   clock_now : op 03, len 1, clock_id 1 (monotonic)
EMIT 03 01 00 01
#   reply is a status, a length, then sec{8} nsec{4}
READ 3
READ 12
EMIT 02 01 00 00
```

Monotonic time starts at zero when the broker starts, so the first four bytes
of `sec` are all you need unless your program runs for 136 years.

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
brainstem --trace run.log --preopen-dir work=. -- ./build/bfi prog.bf
```

which records every frame in both directions. Since the frames are the whole
interface, the trace is the whole story of what your program did.

---

## 10. Where to go next

- [ABI.md](ABI.md) — every op, field by field. §7 is the op reference.
- [README.md](README.md) — what state the project is in.
- [CONVENTIONS.md](CONVENTIONS.md) and [HANDOFF.md](HANDOFF.md) — if you want
  to change brainstem rather than use it.
