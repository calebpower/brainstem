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
< 00 len=8      OK, handles 2 and 3
> 0e len=2      pipe
< 00 len=8      OK, handles 4 and 5
> 0f len=53     spawn "bfi" "echo.bf", child fd 0 <- handle 2, fd 1 <- handle 5
< 00 len=4      OK, process handle 6
> 0b len=8      write handle 3, "hi"
> 0a len=8      read handle 4
< 00 len=2      6869
> 10 len=6      wait handle 6
< 00 len=4      exited, code 0
```

That is the capability the whole project was for: **brainfuck itself becomes
the harness**, able to chain another program's primitives without a shell
script in the middle.

Milestone M6, **gated: 172 pass, 0 fail on `freebsd-15.1` and on
`ubuntu-26.04`.** What remains is not ops — it is tiers. M7 sweeps mutation
testing across all twenty three, freezes `ABI.md`, and adds `--replay`. M8
makes `sys_lockdown()` real.

**Two boundaries worth knowing before you run this.** Filesystem access is
bounded by what you preopen, with the gap described in ABI.md §8.0. **Network
access is not bounded at all**, and a directory preopen carries the right to
*run programs out of it* — see ABI.md §8.1. brainstem is not a sandbox, it
runs with your credentials, and it can now start processes. Do not run
brainfuck you did not write.

## The operations

Twenty-three, specified in [ABI.md](ABI.md). All built.

| | |
|---|---|
| `hello` `exit` | handshake, version negotiation, teardown |
| `clock_now` | realtime and monotonic, steerable with `--clock` |
| `random_bytes` | from the kernel, or from a seed with `--seed` |
| `read` `write` `close` `poll` | files, pipes and sockets alike |
| `open` `seek` `stat` `readdir` `unlink` `mkdir` `rename` | beneath a preopened directory |
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
network and process spawn, with the broker's own credentials. The capability
model — nothing is reachable that was not named on the command line — is a
usability feature and a foundation for later enforcement, not a containment
claim. Do not run a program you did not write.

It is also not fast, by construction. One byte per `.` through two pipes, and
an interpreter that spends millions of instructions between syscalls.

## Name

The brainstem connects the brain to autonomic function — breathing, heartbeat,
the things that happen below thought. A syscall layer is the same joint. The
sibling project is [bfsodium](https://github.com/calebpower/bfsodium), which
briefly went by graymatter.

## License

MIT. See [LICENSE](LICENSE).
