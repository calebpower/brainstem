# The Linux half of the fallback, for when the reaper site is out of reach.
#
# reaper is the gate of record and nothing here changes that; .reaper.toml is
# untouched. Note what this image can and cannot be: brainstem's gate is TWO
# sessions, freebsd-15.1 and ubuntu-26.04, and this covers ONE of them -- the
# secondary one. podman on FreeBSD runs jails, not Linux containers, so there
# is no container lane that could cover the primary target, and pretending
# otherwise is not on offer. On a FreeBSD host the fallback is `sh
# tests/run.sh` natively, which is exactly what the gate does there.
#
# ubuntu:26.04 is the guest reaper provisions -- see `guests` in .reaper.toml --
# so both run the same userland with the same compiler, and a result from one
# means the same thing as a result from the other.
FROM ubuntu:26.04

# Only the toolchain half. guest-setup.sh is copied on its own rather than
# with the rest of the tree, so that editing source does not invalidate this
# layer; only a change to the toolchain definition rebuilds it.
#
# There is deliberately no apt-get line here. The phases live in
# guest-setup.sh, which is the file reaper already runs, because a second
# definition of the toolchain is how a fallback starts passing what the gate
# would fail. tests/run.sh greps this file to prove it stays that way.
COPY tools/guest-setup.sh /opt/brainstem/guest-setup.sh
RUN sh /opt/brainstem/guest-setup.sh --toolchain

# The tree under test is not baked in. tools/container-test.sh mounts it at
# run time, so the image outlives any particular state of the working tree and
# what gets tested is whatever is on disk right now -- uncommitted work
# included, which is the whole point of a pre-push loop.
WORKDIR /work
