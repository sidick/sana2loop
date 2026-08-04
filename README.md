# sana2loop

`loopback.device` -- a hardware-free SANA-II network device for AmigaOS
providing echo, crossover, and pcap replay/capture modes, plus companion
Shell tools (`SanaInfo`, `SanaDump`, `SanaSend`, `SanaConform`). It
gives any network stack, shim, or driver-consuming software a
deterministic test partner -- with zero hardware and zero emulator
network support required.

(Prior art: [nullsana](https://aminet.net/package/comm/tcp/nullsana), a
binary-only virtual SANA-II device from 2000, was found only after
sana2loop's first release. sana2loop is open source, runs on Kickstart
1.3, and adds pcap replay/record, fault injection, and conformance
tooling.)

Targets plain 68000 and Kickstart 1.3 -- a test tool's value is
universality, and nothing here needs more. 2 MB RAM minimum (see
[Installation](https://sidick.github.io/sana2loop/Installation/)).

**Full user documentation: [sidick.github.io/sana2loop](https://sidick.github.io/sana2loop/)**
(source in [`userdocs/`](userdocs/), built as a MkDocs site -- see
[Building and Testing](userdocs/Building-and-Testing.md) to build it
locally, or `make guide` for an on-Amiga AmigaGuide version of the same
content). See [`docs/PLAN.md`](docs/PLAN.md) for the full milestone plan
and [`docs/sana2-notes.md`](docs/sana2-notes.md) for the SANA-II protocol
reference and implementation notes this project has accumulated, and
`CLAUDE.md` for the fuller day-to-day project state.

## Status

**M0 through M14 done.** M0-M11 validated on-target under the Copperline
emulator against both a real Kickstart 1.3 ROM and a bundled,
redistributable AROS Kickstart replacement: echo and crossover modes, real
SANA-II buffer-management hooks, per-unit configuration files, a fault
injection pipeline (drop/duplicate/truncate/reorder/online-flap, plus an
`oversilent` PMTUD black hole -- a crossover write that exceeds the
*peer's* MTU vanishes silently instead of failing loudly, the exact
failure mode a real PMTUD black hole produces, for testing whether a
stack actually detects and recovers from one), pcap replay (on-demand and
real-time-scheduled), pcap recording, and the
`SanaDump`/`SanaSend` companion tools. M12 built the Aminet packaging +
release pipeline (`make dist`, `.github/workflows/release.yml`) and
verified it locally end-to-end; the repo is now public and the `aminet`
GitHub Environment has its required-reviewer gate in place. M13 added
SANA-II Rev 2/3/4/7 conformance (wider buffer-management hooks, peer/DNS
address queries, `RawMTU`, Rev 7 DMA hook negotiation) plus a new
`SanaConform` probe tool. M14 made the replay-record ceiling a per-unit
config knob (`replaycap=1`..`256`). M15 added wire-type emulation
(`wire=ethernet|slip|cslip|ppp` -- point-to-point serial wires with
4-byte IP-style addressing, so a stack's SLIP/PPP paths are testable
too), a configurable reported link speed (`bps=`), and a minimum-write-
size floor (`mintu=`). M16 added operator-pinned station addresses
(`addr=`), visible TX-error injection (`errors=`, the loud counterpart
to silent drops -- fails writes with `S2ERR_TX_FAILURE` and fires
`S2EVENT_TX|ERROR`), and finer-than-percent silent loss (`loss=N`,
1-in-N). M17 added simulated link latency and jitter (`delay=`/
`deviation=`, VBlank-tick delivery delay with optional symmetric
scatter) -- see [`userdocs/Changelog.md`](userdocs/Changelog.md). **No
release tag has been pushed yet** -- see
[`docs/PLAN.md`](docs/PLAN.md)'s milestone table for what's left.

Not yet done: cutting and publishing a first tagged release.

## Quick start

```
SanaInfo 0                          open unit 0, print DEVICEQUERY info
SanaDump UNIT=0 FILE=capture.pcap   capture unit 0's traffic to a file
SanaSend UNIT=0 FILE=capture.pcap   replay a pcap file's frames onto
                                     unit 0
SanaConform 0                       probe unit 0's SANA-II Rev 2/3/4/7
                                     conformance
```

Example `SanaInfo` output, unit 0's factory defaults:

```
> SanaInfo
MTU: 1500
BPS: 10000000
HardwareType: 1 (1=Ethernet)
AddrFieldSize: 48 bits
Station address: 02:53:32:4c:00:00
```

See [Getting Started](userdocs/Getting-Started.md) for a full walkthrough,
and [Installation](userdocs/Installation.md) for where each file goes.

## AI-assisted development

Be aware: **sana2loop was written largely by an AI coding agent**
(Anthropic's Claude, via Claude Code), working under human direction,
review, and on-hardware/on-emulator testing. The entire source is
BSD-licensed and open for review.

## Build

```
make docker   # cross-compile build/loopback.device + SanaInfo/SanaDump/SanaSend/SanaConform
              # (needs Docker; no local toolchain required)
```

See the Makefile for building with a locally-installed
`m68k-amigaos-gcc` instead, and
[`userdocs/Building-and-Testing.md`](userdocs/Building-and-Testing.md) for
running the on-target test suite.

## Contributing

Issues and pull requests are welcome -- small, focused PRs are easier to
review and land than large ones. `loopback.device` itself targets plain
68000 and Kickstart 1.3 as a deliberate, non-negotiable design constraint
(see `CLAUDE.md` and the [proposal](https://github.com/sidick/project-ideas/blob/main/sana2loop-proposal.md)
for why); the companion Shell tools aren't held to that floor. Check
[`docs/PLAN.md`](docs/PLAN.md) for the current milestone state before
starting anything substantial.

## License

BSD 2-Clause (see `LICENSE`) -- deliberately permissive so the device and
its companion tools are easy to vendor into other projects.
