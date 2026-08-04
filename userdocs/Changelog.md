# Changelog

sana2loop is developed milestone by milestone; each one is validated
on-target under Copperline against both a real Kickstart 1.3 ROM and
bundled AROS before being considered done. This page summarizes what
landed in each, in user-facing terms — see the repository's
[`docs/PLAN.md`](https://github.com/sidick/sana2loop/blob/main/docs/PLAN.md)
for the full engineering detail behind each one.

No numbered release has shipped yet — this project is still pre-1.0.

## Foundational milestones (M0–M4)

- Echo mode (unit 0's default): `S2_DEVICEQUERY`, `S2_ONLINE`/`S2_OFFLINE`,
  `S2_ONEVENT`, `CMD_FLUSH`, `AbortIO`.
- Real SANA-II buffer-management hooks (`S2_CopyToBuff`/`S2_CopyFromBuff`/
  `S2_PacketFilter`).
- The core delivery precedence: typed `CMD_READ` → `S2_READORPHAN` →
  bounded `rxqueue` → drop, with `Sana2DeviceStats.UnknownTypesReceived`
  counting drops.
- `S2_CONFIGINTERFACE`, current-vs-factory station address split,
  `S2_TRACKTYPE`/`S2_GETTYPESTATS`.
- `S2_BROADCAST`/`S2_MULTICAST`, `S2_ADD/DELMULTICASTADDRESS`,
  `SANA2IOF_RAW`.

## M5 — Configuration files

`ENV:sana2loop/unitN.config` (falling back to `S:`), read once at first
open. See [Configuration Reference](Configuration-Reference.md).

## M6 — Crossover mode

Two units can be paired (`mode=crossover`/`peer=`) so writes on one deliver
to the other with real, unswapped addressing and standard 802.3-style
receive filtering — see
[Echo and Crossover Modes](Echo-and-Crossover.md). Units 2 and 3 are
paired this way by default.

## M7 — Fault injection

`droppct`/`duppct`/`truncate`/`reorder`/`oversilent`/`offlineafter`/
`onlineafter` — see [Fault Injection](Fault-Injection.md).

## M8 — pcap replay (consume mode)

`mode=replay`/`replay=<path>` plays back a captured `.pcap` trace,
one record per read, in order. See [Replay and Record](Replay-and-Record.md).

## M9 — Scheduled replay and recording

`replaymode=scheduled` delivers each record on its own real captured
timing instead of on-demand; `record=<path>` captures whatever a unit
actually receives back out to a `.pcap` file. Both are driven by a real
VBlank interrupt, including invoking buffer-management hooks from genuine
interrupt context — a first for this device.

**A note on how this was validated**: this milestone initially shipped
with the bundled AROS Kickstart replacement hanging specifically once a
unit used `replaymode=scheduled` or `record=`, while real Kickstart 1.3
passed every check cleanly. Root cause, found through careful on-target
instrumentation: bundled AROS's `CheckIO()`/`WaitIO()` can falsely report
an I/O request as already complete immediately after submission, before
the device has actually replied to it, specifically for requests destined
to complete via a real hardware interrupt rather than ordinary program
context. The device itself was correct throughout; the fix (waiting on
independent evidence of real delivery, then always safely re-checking
regardless of what `CheckIO()`/`WaitIO()` claimed) landed entirely in the
test suite. This is exactly the kind of platform-specific gap real
hardware testing exists to catch — the bundled AROS Kickstart replacement
is a convenient, license-free stand-in for catching *gross* regressions in
public CI, but it is not a substitute for validating against real
Kickstart 1.3 before trusting a release claim.

## M10 — Documentation

This site (MkDocs Material), plus an on-Amiga AmigaGuide manual built
from the same source (`make guide`) — one docs source feeding both. A
standard AmigaDOS `$VER:` version cookie was also added to the device and
`SanaInfo` along the way (previously missing entirely).

## M11 — SanaDump and SanaSend

Companion Shell tools for capturing and sending traffic through a unit
without writing your own program against the device — see
[SanaDump and SanaSend](SanaDump-and-SanaSend.md).

## M12 — Aminet packaging and release flow

The infrastructure (not yet a shipped release) for packaging and
publishing tagged versions to Aminet, with a human-approval gate before
any actual upload.

## M13 — SANA-II Rev 2/3/4/7 conformance

Support for the post-Commodore SANA-II extensions real-world stacks
probe for: wider buffer-management hooks (`S2_CopyToBuff16/32`),
peer/DNS address queries and `RawMTU` (`S2_GETPEERADDRESS`,
`S2_GETDNSADDRESS`), and DMA-capable hooks (accepted, never invoked —
this device has no real DMA to offer). Plus a new `SanaConform` probe
tool. See [SANA-II Conformance](SANA-II-Conformance.md).

## M14 — Configurable replay capacity

`replaycap=N` (1–256, default 4) lets you raise or lower the record
ceiling on a `mode=replay` unit per-unit, instead of living with a
fixed limit — see [Replay and Record](Replay-and-Record.md#replay). The
default itself hasn't changed; this only adds an escape hatch for
captures bigger than 4 records on targets with the memory to spare.

## M15 — Wire types, reported BPS, minimum write size

Three new config keys, all inspired by reviewing
[nullsana](https://aminet.net/package/comm/tcp/nullsana) (the 2000-era
virtual SANA-II device found as prior art):

- `wire=ethernet|slip|cslip|ppp` — a unit can now pretend to be a
  point-to-point serial wire: 4-byte IP-style addressing, per-wire
  MTU/BPS defaults, no broadcast/multicast/raw. Lets you exercise a
  stack's SLIP/PPP code paths with zero hardware.
- `bps=N` — the link speed `S2_DEVICEQUERY` reports (previously always
  10000000). Reported only, never a throttle.
- `mintu=N` — a minimum write size; shorter writes fail
  `S2ERR_BAD_ARGUMENT` instead of being accepted. Catches stacks that
  forget real Ethernet's minimum-frame padding.

One more unit is pre-allocated (unit numbers now run 0–15). See
[Configuration Reference](Configuration-Reference.md) for all three keys.

## M16 — Pinned addresses, visible error injection, finer loss rates

The second batch of nullsana-inspired config keys:

- `addr=02:11:22:33:44:55` — pin a unit's factory station address from
  config, so recorded fixtures are reproducible and crossover pairs can
  mimic a specific real topology. Works with 4-byte addresses on
  non-Ethernet `wire=` units too.
- `errors=N` — 1 in N writes fails **visibly** (`S2ERR_TX_FAILURE`,
  with `S2EVENT_TX|S2EVENT_ERROR` fired) — the loud counterpart to the
  existing silent-loss knobs, for exercising a stack's error-handling
  and retry paths rather than its timeout recovery.
- `loss=N` — 1-in-N silent packet loss, for rates a whole percent can't
  express (`loss=1000` ≈ 0.1%) — real-world loss is usually much rarer
  than `droppct`'s 1-in-100 floor.

Two more units are pre-allocated (unit numbers now run 0–17). See
[Fault Injection](Fault-Injection.md) for where the new knobs sit in the
pipeline.

## M17 — Simulated link latency and jitter

`delay=N`/`deviation=N` — the last of the nullsana-inspired batch: a
written packet's delivery can now be deferred by a fixed number of
VBlank ticks, with optional symmetric jitter, instead of always
happening synchronously. Runs last in the fault-injection pipeline,
after `reorder`. `CMD_FLUSH`, `S2_OFFLINE`, and last-close all
immediately deliver anything still waiting, same as they already do for
the reorder buffer. See [Fault Injection](Fault-Injection.md#simulating-latency-and-jitter-delay-and-deviation).

One more unit is pre-allocated (unit numbers now run 0–18).

## M18 — Multi-open fan-out, real S2_PacketFilter dispatch, SANA2OPF_MINE

Open a unit more than once — a stack plus a passive monitor, the
`sanamon`-style pattern nullsana's own review surfaced — and every open
with a matching typed `CMD_READ` now gets its own independent copy of
each packet, instead of only whichever opened first. `S2_PacketFilter`
(previously accepted but never actually invoked) is now genuinely
dispatched per candidate, immediately before the copy hook, so a caller
can reject packet types it doesn't want without paying for the copy.
`SANA2OPF_MINE` (exclusive access) is now enforced too. See
[Echo and Crossover: Multiple opens and monitoring](Echo-and-Crossover.md#multiple-opens-and-monitoring).

No new units needed; no config or wire-level changes for existing
single-open usage.

## M19 — Companion tools become general-purpose SANA-II utilities

Prompted by reviewing Aminet's `sanamon` (`comm/net/sanamon`): all four
companion tools (`SanaInfo`, `SanaDump`, `SanaSend`, `SanaConform`)
previously only ever opened sana2loop's own `loopback.device`. Now:

- **`DEVICE=`** on all four -- point any of them at a real SANA-II
  driver, not just this project's own device. `SanaConform` in
  particular is arguably more useful probing an unknown real driver's
  actual conformance than confirming this project's own.
- **`CONFIG`** (opt-in) -- issues `S2_CONFIGINTERFACE` using the
  driver's own factory address, for a real driver with no protocol
  stack already running to configure it. sana2loop's own units never
  need this.
- **`ONLINE`'s default flipped**: `SanaDump`/`SanaSend`/`SanaConform`
  used to force the unit online unconditionally. That's now opt-in --
  the safer default once these tools can target a real driver a stack
  might be actively managing. See
  [SanaDump and SanaSend: Bringing a unit online](SanaDump-and-SanaSend.md#bringing-a-unit-online).
- **`SanaConform` now probes `S2_PacketFilter` dispatch** -- whether the
  driver actually invokes a negotiated filter hook, not just accepts
  the tag. The exact gap Aminet's `sanamon` readme calls out as rare in
  the wild, and the same gap sana2loop's own M18 closed.
- **`SanaDump DUMP`/`LEN`** -- live console packet view (type, length,
  error/wire-error, BCAST/MCAST/HOST, addresses, hex) as packets are
  captured, sanamon-style, alongside the existing `.pcap` file output.

See [SanaDump and SanaSend](SanaDump-and-SanaSend.md),
[SanaInfo Reference](SanaInfo-Reference.md), and
[SANA-II Conformance](SANA-II-Conformance.md) for the full details.

## What's next

- A proper first tagged release (the M12 packaging pipeline is built and
  tested, but no version has been tagged/published yet).
