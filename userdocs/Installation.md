# Installation

sana2loop has **no installer**: copy two files into place and it works.

## Requirements

`loopback.device` (the device itself):

- Any Amiga from a stock 68000 upwards.
- Kickstart 1.3 (V34) or later — deliberately the lowest floor this
  project could target, since a test tool's whole value is universality.
  No `utility.library`, no `dos.library` calls off the device's own
  BeginIO path (only its optional config-file read touches `dos.library`,
  and only from a real Process opener — see
  [Configuration Reference](Configuration-Reference.md)).

`SanaInfo` (the companion CLI tool):

- AmigaOS 2.04 (V37) or later (`CreateMsgPort()`/`CreateIORequest()` are
  V36+). Companion Shell tools aren't held to the device's own 1.3 floor.

## Installing

1. Copy `loopback.device` to `DEVS:`.
2. Copy `SanaInfo` to `C:` (optional — only needed if you want to query the
   device from the Shell rather than through your own program's
   `OpenDevice()` calls).

That's it — no reboot, no `LoadModule`/`RemDevice` dance needed for a
freshly-copied device file. A program opens it the normal SANA-II way:

```
OpenDevice("loopback.device", unit, (struct IORequest *)io, 0)
```

Unit numbers **0 through 11** are available (12 units are pre-allocated).
Unit 0 defaults to standalone **echo** mode; units 2 and 3 default to a
**crossover** pair; every other unit defaults to standalone echo unless you
give it its own config file. See
[Echo and Crossover Modes](Echo-and-Crossover.md).

## Per-unit configuration (optional)

Each unit can be configured before first use — see the
[Configuration Reference](Configuration-Reference.md) for the full key
list. Configuration is entirely optional: an unconfigured unit works out of
the box as a standalone echo (or, for units 2/3, a crossover pair).

## Checking which version you have

Both the device and `SanaInfo` carry a standard AmigaDOS `$VER:` cookie, so
the Shell `Version` command reports it:

```
> Version DEVS:loopback.device
sana2loop 1.0 (30.07.2026)

> Version SanaInfo
SanaInfo 1.0 (30.07.2026)
```

## Next steps

Continue with [Getting Started](Getting-Started.md) for a first
`OpenDevice()`/echo walkthrough using `SanaInfo`.
