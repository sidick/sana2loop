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

## What's next

- **Docs pass** (this site, plus a teaching-reference comment pass through
  the source) — in progress.
- `SanaDump`/`SanaSend` — companion Shell tools for capturing and
  sending traffic without writing your own program against the device.
- **Aminet packaging** and a proper first tagged release.
