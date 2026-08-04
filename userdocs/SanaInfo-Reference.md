# SanaInfo Reference

`SanaInfo` is a small Shell command that opens a `loopback.device` unit,
optionally toggles it online/offline, and prints `S2_DEVICEQUERY` plus its
current station address. It exists to let you check a unit's state from
the Shell without writing a test program.

Requires AmigaOS 2.04 (V37) or later — see
[Installation](Installation.md#requirements).

## Usage

```
SanaInfo [unit] [ONLINE|OFFLINE] [CONFIG] [DEVICE=<name>]
```

- `unit` — a unit number (0–18). Defaults to `0` if omitted.
- `ONLINE` / `OFFLINE` — if given, issues `S2_ONLINE` or
  `S2_OFFLINE` before querying, and reports the result.
- `CONFIG` — issues `S2_CONFIGINTERFACE` first, using whatever address
  `S2_GETSTATIONADDRESS` already reports as the driver's own factory
  default. sana2loop's own units never need this (usable from a factory
  default with zero setup) — it matters for a real driver (see `DEVICE=`
  below) with no protocol stack already running to configure it. A no-op
  (reported, not fatal) if already configured. Independent of
  `ONLINE`/`OFFLINE` — combine freely.
- `DEVICE=<name>` — SANA-II device to open; omitted = `loopback.device`.
  Not limited to this project's own device — see
  [SanaDump and SanaSend: Other drivers](SanaDump-and-SanaSend.md#other-drivers).

Arguments can be given in any order (`SanaInfo ONLINE 3` and
`SanaInfo 3 ONLINE` are equivalent) — anything that parses as a number is
taken as the unit, anything matching `ONLINE`/`OFFLINE`/`CONFIG`
(case-sensitive) is taken as that keyword, anything starting `DEVICE=` is
taken as the device name.

## Examples

Query unit 0 (the default) without changing its state:

```
> SanaInfo
MTU: 1500
BPS: 10000000
HardwareType: 1 (1=Ethernet)
AddrFieldSize: 48 bits
Station address: 02:53:32:4c:00:00
```

Bring unit 3 online, then query it:

```
> SanaInfo 3 ONLINE
SanaInfo: unit 3 set ONLINE (io_Error=0)
MTU: 1500
BPS: 10000000
HardwareType: 1 (1=Ethernet)
AddrFieldSize: 48 bits
Station address: 02:53:32:4c:00:03
```

Take unit 3 back offline:

```
> SanaInfo 3 OFFLINE
SanaInfo: unit 3 set OFFLINE (io_Error=0)
...
```

## Exit codes

| Code | Meaning |
|------|---------|
| `0` | Success. |
| `10` | `S2_DEVICEQUERY` or `S2_GETSTATIONADDRESS` failed (the error and its `io_Error` value are printed to stderr). |
| `20` | Couldn't create a message port/IORequest, or `OpenDevice()` itself failed (e.g. no such unit, or the device isn't in `DEVS:`). |

## What it doesn't do

`SanaInfo` is a query/toggle tool, not a traffic generator or capture tool
— it never issues `CMD_WRITE`/`CMD_READ`. For sending or capturing traffic
from the Shell, see [SanaDump and SanaSend](SanaDump-and-SanaSend.md).
