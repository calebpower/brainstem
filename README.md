# brainstem #

A syscall runtime for **standard** brainfuck: sockets, clocks, randomness,
files and processes, reached over stdin and stdout through a host broker.

WASI for brainfuck. Yes, really.

## What

Brainfuck has eight instructions and two of them are I/O: `,` reads a byte and
`.` writes one. That is the whole interface to the world, and it is why every
attempt to make the language useful has extended it — SystemF adds a `%`
instruction, Brainfuck++ and NetFuck add more. Extending the language is the
easy answer and it gives up the only interesting property brainfuck has.

brainstem does not extend anything. A program speaks a byte protocol through
the `,` and `.` it already has, and a broker on the other end of the pipe
turns those bytes into syscalls. **The program remains standard brainfuck**:
the same file runs unmodified under any conforming interpreter, where it reads
end-of-input and does nothing, because there is nobody there.

The broker is C99 with no dependencies. It spawns an interpreter of your
choosing — it embeds none and requires none in particular — and talks to the
program through it.

## Status

**Nothing works yet.** This is milestone M0: the two test lanes, the vendored
interpreter, the documents, and a suite that proves the ground is solid. There
is no broker. `src/` is empty.

That ordering is deliberate. The first commit has to be green on the machine of
record, and you cannot claim that without the lane that runs it.

What M0 does prove: the toolchain has exactly one definition and both lanes use
it, the build has exactly one definition, the guests the gate will run are the
ones the provisioning script knows, and the pinned interpreter is sound on both
platforms in all three of brainfuck's end-of-input conventions.

It is **gated**: 22 pass, 0 fail on `freebsd-15.1` and 22 pass, 0 fail on
`ubuntu-26.04`. Read that for what it is — M0 compiles two C files and runs an
interpreter. The platform divergence this project has to survive is all still
ahead of it.

## The operations

Twenty-three, specified in [ABI.md](ABI.md) and not yet implemented.

| | |
|---|---|
| `hello` `exit` | handshake, version negotiation, teardown |
| `clock_now` | realtime and monotonic |
| `random_bytes` | |
| `socket` `connect` `bind` `listen` `accept` | |
| `read` `write` `close` `poll` | files, pipes and sockets alike |
| `pipe` `spawn` `wait` | **which is how a brainfuck program drives another one** |
| `open` `seek` `stat` `readdir` `unlink` `mkdir` `rename` | |

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

**brainstem is not a sandbox.** It hands a brainfuck program the filesystem,
the network and process spawn, with the broker's own credentials. The
capability model — nothing is reachable that was not named on the command line
— is a usability feature and a foundation for later enforcement, not a
containment claim. Do not run brainfuck you did not write.

It is also not fast, by construction. One byte per `.` through two pipes, and
an interpreter that spends millions of instructions between syscalls.

## Name

The brainstem connects the brain to autonomic function — breathing, heartbeat,
the things that happen below thought. A syscall layer is the same joint. The
sibling project is [bfsodium](https://github.com/calebpower/bfsodium), which
briefly went by graymatter.

## License

MIT. See [LICENSE](LICENSE).
