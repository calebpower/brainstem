# brainstem #

A syscall broker for esoteric languages: sockets, clocks, randomness, files and
processes, reached over stdin and stdout **without extending the language**.

WASI for esolangs. brainfuck is the first client. Yes, really.

## What

An esoteric language's interface to the world is usually one byte in and one
byte out, and often not even that. Brainfuck is the sharp case: eight
instructions, of which `,` reads a byte and `.` writes one. That is the whole
of it, and it is why almost every attempt to make such a language useful has
**extended** it — SystemF adds a `%` instruction to brainfuck, Brainfuck++ and
NetFuck add more. Extending is the easy answer and it gives up the only
interesting property these languages have.

brainstem extends nothing. A program speaks a byte protocol through the I/O it
already has, and a broker on the other end of the pipe turns those bytes into
syscalls. **The program stays in the unextended language**: the same file runs
under any conforming implementation, where it reads end-of-input and does
nothing, because there is nobody there.

The broker is C99 with no dependencies. It spawns an interpreter of your
choosing — it embeds none and requires none in particular — and talks to the
program through it. That indirection is what makes the claim checkable, and it
is also what makes the broker indifferent to which language is on the far end.

## Status

**All twenty three operations are built.** A file containing nothing but the
eight brainfuck instructions creates two pipes, starts an interpreter on a
*second* brainfuck program with those pipes as its stdin and stdout, sends it
two bytes, reads its answer, and collects its exit status:

```
> 0e len=2      pipe
< 00 len=8      OK, handles 4 and 5      (1, 2 and 3 are the broker's stdio)
> 0e len=2      pipe
< 00 len=8      OK, handles 6 and 7
> 0f len=53     spawn "bfi" "echo.bf", child fd 0 <- handle 4, fd 1 <- handle 7
< 00 len=4      OK, process handle 8
> 0b len=8      write handle 5, "hi"
> 0a len=8      read handle 6, one byte
< 00 len=1      68
> 0a len=8      read handle 6, one byte
< 00 len=1      69
> 10 len=6      wait handle 8
< 00 len=4      exited, code 0
```

That is the capability the whole project was for: **brainfuck itself becomes
the harness**, able to chain another program's primitives without a shell
script in the middle.

Milestone M7, and **`ABI.md` is frozen at 1.0** — its version is checked
against the constants the broker is compiled with and against the two bytes
the broker actually puts on the wire, so a document that has drifted from the
binary cannot be committed.

What M7 added was not ops. `--sort-readdir` makes a directory walk
reproducible, `--replay` answers every frame from a recorded trace without
touching the system at all, and three tiers that had been declared and absent
now run: the fixtures' prose is checked against their hex, five metamorphic
relations say what each knob is allowed to change, and a mutation sweep breaks
thirty three things in turn and requires a NAMED check to notice each one.
That sweep found three gaps on its first run — `poll` had no fixture, `PIPE`
was a status nothing produced, and the broker's `SIGPIPE` handling had nothing
standing behind it.

M8 makes `sys_lockdown()` real, and has a decision to make first: see the note
on it in `src/sys.h`.

**It confines nothing, and that is the design.** A brainstem program sees the
system its broker sees: paths are absolute or relative to the broker's working
directory, `connect` reaches whatever the host routes to, and `spawn` runs
what it is told to. There are no flags for reaching things. An earlier version
required every directory to be named on the command line; that was removed,
because a brainfuck program cannot compare strings and so could only reach a
preopen through a handle index agreed out of band — the one kind of
coordination brainfuck is worst at, bought for a containment property the
project never claimed. ABI.md §8.0 has the reasoning.

**So do not run brainfuck you did not write.** It runs with your credentials
and it can start processes.

## The operations

Twenty-three, specified in [ABI.md](ABI.md). All built.

| | |
|---|---|
| `hello` `exit` | handshake, version negotiation, teardown |
| `clock_now` | realtime and monotonic, steerable with `--clock` |
| `random_bytes` | from the kernel, or from a seed with `--seed` |
| `read` `write` `close` `poll` | files, pipes and sockets alike |
| `open` `seek` `stat` `readdir` `unlink` `mkdir` `rename` | ordinary paths, or beneath a directory handle |
| `socket` `connect` `bind` `listen` `accept` | IPv4 and IPv6; Unix is declared and answers NOTSUP |
| `pipe` `spawn` `wait` | **how one program drives another** |

## Other languages

Nothing in the broker knows what brainfuck is. It spawns an interpreter, writes
bytes to its stdin and reads bytes from its stdout; the protocol is a byte
stream and the ABI is flat fixed-width records. **A language qualifies if it
can read a byte, write a byte, and loop** — which is most of them, including
several that have little else.

Two requirements, and only one is about the language:

- It must be able to emit and consume arbitrary bytes, `0x00` included.
- Its **implementation** must not buffer its output — a byte written must reach
  the pipe before the program next blocks on a read. That is [ABI.md](ABI.md)
  requirement I1, it is the single most common way to get a silent deadlock,
  and `brainstem --check-interpreter` tests for it directly.

The honest caveat: the protocol is *portable* to other languages but *tuned* to
brainfuck. A zero byte is free to emit in brainfuck and bit manipulation is
ruinously expensive, so the ABI is full of zero padding and spends a whole byte
where a flag bit would do. Another language would find those choices harmless
rather than helpful, and might reasonably have wanted a denser encoding. The
wire format will not be re-cut per language.

**No work is scheduled for this.** It is a property of the design rather than a
plan, recorded so nobody assumes the opposite.

## Documentation

Five documents, split by who is reading:

- **[GUIDE.md](GUIDE.md)** — start here if you want to *write* a brainfuck
  program that does something. How to run the broker, how to build a request
  and read a reply, worked examples, and the three things that will trip you
  up first.
- **[ABI.md](ABI.md)** — the normative wire specification. Every op, field by
  field. Read this if you are implementing a client in some other language, or
  transcribing frames by hand.
- **[CONVENTIONS.md](CONVENTIONS.md)** — the rulebook, if you are changing
  brainstem. Numbered, normative, with the testing tiers.
- **[HANDOFF.md](HANDOFF.md)** — how it is actually built, what has bitten, and
  what is next.

## Running it

The gate is two platforms and both must be green:

```sh
reaper up && reaper test        # freebsd-15.1 and ubuntu-26.04
```

There are two fallbacks and **each covers one half**. On a FreeBSD host the
suite runs natively, which is exactly what the gate does there. On any host
with a container engine, the container lane covers Linux:

```sh
sh tests/run.sh                 # the FreeBSD half, on a FreeBSD host
sh tools/container-test.sh      # the Linux half, anywhere
```

Neither substitutes for the other, and the container lane covers the platform
that matters less. podman on FreeBSD runs jails, not Linux containers, so there
is no container lane that could cover the primary target.

## Safety

**brainstem is not a sandbox.** It hands the program the filesystem, the
network and process spawn, with the broker's own credentials, and confines
none of it. Do not run a program you did not write.

There was briefly a capability model — nothing reachable that was not named on
the command line — described even then as "a usability feature, not a
containment claim". It was removed at M6, because it restricted a program's
reach for a benefit the project had already disclaimed, and because reaching
things is what brainstem is FOR. ABI.md §8.0 keeps the reasoning.

It is also not fast, by construction. One byte per `.` through two pipes, and
an interpreter that spends millions of instructions between syscalls.

## Name

The brainstem connects the brain to autonomic function — breathing, heartbeat,
the things that happen below thought. A syscall layer is the same joint. The
sibling project is [bfsodium](https://github.com/calebpower/bfsodium), which
briefly went by graymatter.

## License

MIT. See [LICENSE](LICENSE).
