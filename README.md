# sana2loop

`loopback.device` -- a hardware-free SANA-II network device for AmigaOS
providing echo, crossover, and pcap replay/capture modes, plus companion
Shell tools (`SanaInfo`, `SanaDump`, `SanaSend`). No null/dummy SANA-II
device has existed before this, despite being the obvious first tool for
testing any network stack, shim, or driver-consuming software --
deterministically, with zero hardware and zero emulator network support
required.

Targets plain 68000 and Kickstart 1.3 -- a test tool's value is
universality, and nothing here needs more.

**Full user documentation is in [`userdocs/`](userdocs/)** (built as a
MkDocs site, not yet published -- see [Building and Testing](userdocs/Building-and-Testing.md)
to build it locally, or `make guide` for an on-Amiga AmigaGuide version).
See [`docs/PLAN.md`](docs/PLAN.md) for the full milestone plan and
[`docs/sana2-notes.md`](docs/sana2-notes.md) for the SANA-II protocol
reference and implementation notes this project has accumulated, and
`CLAUDE.md` for the fuller day-to-day project state.

## Status

**M0 through M11 done and validated on-target** under the Copperline
emulator against both a real Kickstart 1.3 ROM and a bundled,
redistributable AROS Kickstart replacement: echo and crossover modes, real
SANA-II buffer-management hooks, per-unit configuration files, a fault
injection pipeline (drop/duplicate/truncate/reorder/oversilent/online-flap),
pcap replay (on-demand and real-time-scheduled), pcap recording, and the
`SanaDump`/`SanaSend` companion tools.

Not yet done: Aminet packaging and a first tagged release. See
[`docs/PLAN.md`](docs/PLAN.md)'s milestone table for the exact remaining
scope.

## AI-assisted development

Be aware: **sana2loop was written largely by an AI coding agent**
(Anthropic's Claude, via Claude Code), working under human direction,
review, and on-hardware/on-emulator testing. The entire source is
BSD-licensed and open for review.

## Build

```
make docker   # cross-compile build/loopback.device + SanaInfo/SanaDump/SanaSend
              # (needs Docker; no local toolchain required)
```

See the Makefile for building with a locally-installed
`m68k-amigaos-gcc` instead, and
[`userdocs/Building-and-Testing.md`](userdocs/Building-and-Testing.md) for
running the on-target test suite.

## License

BSD 2-Clause (see `LICENSE`) -- deliberately permissive so the device and
its companion tools are easy to vendor into other projects.
