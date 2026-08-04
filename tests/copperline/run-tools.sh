#!/bin/sh
# run-tools.sh -- headless Copperline on-target test for SanaDump/SanaSend.
#
# A DELIBERATELY SEPARATE boot session from run.sh's own device smoke test
# -- see machine-tools.toml's own comment for why (short version: the
# device's own test run leaves ~500 KB of chip memory permanently
# allocated by design, which starves any clib2 program trying to load
# afterward in the same session; a fresh boot has the full budget).
#
# Boots a minimal ADF with just DEVS:loopback.device, C:SanaDump,
# C:SanaSend, unit 12's config fixture (rxqueue=5, echo mode), and
# fixtures/arp.pcap (the project's own canonical corpus, host/make_fixtures.py)
# for SanaSend to replay.
#
# tools-startup-sequence.txt runs SanaDump FIRST, backgrounded via `Run`,
# so unit 12's open count never drops back to 0 between it and SanaSend's
# later write -- a real gap an earlier version of this test hit: closing
# SanaSend's own open (dropping the count to 0) before SanaDump opened
# fresh triggered reset_unit()'s documented "reset at first-open"
# behavior, silently discarding whatever SanaSend had just buffered via
# rxqueue. Keeping SanaDump's own open alive across both sidesteps that
# entirely -- rxqueue=5 (unit12.config) then just means SanaDump doesn't
# need its own queued read to already be in place before SanaSend writes,
# no finer-grained timing coordination needed.
#
# There is no serial-visible pass/fail marker to wait for, unlike run.sh:
# SanaDump/SanaSend are normal AmigaDOS CLI programs, and their own stdout
# does NOT reach Copperline's `--serial stdout` capture (confirmed
# empirically -- only direct hardware serial writes like sana2test.c's
# RawPutChar-based output do). Verification is entirely post-run: extract
# the produced captured.pcap via xdftool and check it with
# verify_sanadump.py, exactly like run.sh's own M9 record= check does.
#
# SanaDump/SanaSend are V36+ (2.0+) tools by design (see their own source
# comments) -- unlike run.sh, KICK= here should point at a real 2.0+-or-
# later ROM if you have one; a real Kickstart 1.3 ROM cannot run these at
# all (lacks CreateMsgPort()/CreateIORequest()/ReadArgs()). Default (no
# KICK=) boots Copperline's bundled AROS replacement, confirmed to report
# Exec lib_Version 51 -- comfortably V36+.
#
# Prereqs: same as run.sh (xdftool, copperline, the cross-built tools).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

COPPERLINE=${COPPERLINE:-copperline}
KICK=${KICK:-}
XDFTOOL=${XDFTOOL:-xdftool}
DEVICE=${SANA2LOOP_DEVICE:-$ROOT/build/loopback.device}
SANAINFO=${SANAINFO_M68K:-$ROOT/build/SanaInfo}
SANADUMP=${SANADUMP_M68K:-$ROOT/build/SanaDump}
SANASEND=${SANASEND_M68K:-$ROOT/build/SanaSend}
SANACONFORM=${SANACONFORM_M68K:-$ROOT/build/SanaConform}
# Raised from 40 by the pre-launch-review pass (adding SanaInfo + the
# machine-tools.toml speed=100 fix below both need more wall-clock time --
# see that file's own comment): 75 passed 3/3 clean locally, 90 gives
# comfortable CI margin.
BENCH=${BENCH:-90}
ADF="$HERE/tools-boot.adf"

[ -e "$DEVICE" ] || { echo "FAIL: missing $DEVICE (run make device)" >&2; exit 2; }
[ -e "$SANAINFO" ] || { echo "FAIL: missing $SANAINFO (run make tools)" >&2; exit 2; }
[ -e "$SANADUMP" ] || { echo "FAIL: missing $SANADUMP (run make tools)" >&2; exit 2; }
[ -e "$SANASEND" ] || { echo "FAIL: missing $SANASEND (run make tools)" >&2; exit 2; }
[ -e "$SANACONFORM" ] || { echo "FAIL: missing $SANACONFORM (run make tools)" >&2; exit 2; }
[ -z "$KICK" ] || [ -e "$KICK" ] || { echo "FAIL: KICK set but missing: $KICK" >&2; exit 2; }
command -v "$COPPERLINE" >/dev/null || { echo "FAIL: $COPPERLINE not found" >&2; exit 2; }
command -v "$XDFTOOL" >/dev/null || { echo "FAIL: $XDFTOOL not found (pip install amitools)" >&2; exit 2; }
[ -n "$KICK" ] && echo "ROM: $KICK" || echo "ROM: bundled AROS"

rm -f "$ADF"
"$XDFTOOL" "$ADF" format 'S2LTools' ofs + \
    boot install boot1x + \
    makedir c + makedir s + makedir devs + makedir s/sana2loop + \
    write "$DEVICE" devs/loopback.device + \
    write "$SANAINFO" c/SanaInfo + \
    write "$SANADUMP" c/SanaDump + \
    write "$SANASEND" c/SanaSend + \
    write "$SANACONFORM" c/SanaConform + \
    write "$HERE/fixtures/unit12.config" s/sana2loop/unit12.config + \
    write "$HERE/tools-startup-sequence.txt" s/startup-sequence \
    > /dev/null

# Reuses the project's top-level canonical corpus (host/make_fixtures.py)
# rather than a copy under tests/copperline/fixtures/, since this is
# exactly the kind of real-shaped traffic that corpus exists to provide.
"$XDFTOOL" "$ADF" write "$ROOT/fixtures/arp.pcap" "s/sana2loop/arp.pcap" > /dev/null

set -- --config machine-tools.toml --noaudio --serial stdout --benchmark-until "$BENCH"
[ -n "$KICK" ] && set -- "$@" "$KICK"
( cd "$HERE" && "$COPPERLINE" "$@" ) >/dev/null 2>&1 \
    || { echo "FAIL: $COPPERLINE exited non-zero" >&2; exit 3; }

CAPTURED=$(mktemp)
fails=0
if "$XDFTOOL" "$ADF" type s/sana2loop/captured.pcap > "$CAPTURED" 2>/dev/null; then
    python3 "$HERE/verify_sanadump.py" "$CAPTURED" "$ROOT/fixtures/arp.pcap" || fails=$((fails + 1))
else
    echo "FAIL: s/sana2loop/captured.pcap not found on the ADF post-run (raise BENCH?)" >&2
    fails=$((fails + 1))
fi
rm -f "$CAPTURED"

# M19: DUMP LEN=16 (#37) is exercised in tools-startup-sequence.txt (it
# runs without error, alongside the rest of SanaDump's own session, or
# SANADUMP_VERIFY above would already be failing) but its own printed
# TEXT isn't independently checked here -- see that file's own comment
# for why: a backgrounded Run's stdout redirected to a real file only
# ever captures AmigaDOS's "[CLI N]" banner in this environment, never
# the child clib2 program's actual output, on both bundled AROS and real
# Kickstart 3.1. Not a viable on-target content-verification path;
# formatting logic reviewed by hand instead, operating on io fields
# verify_sanadump.py already proves correct via captured.pcap.

# M13: SanaConform's own stdout doesn't reach Copperline's serial capture
# either (same as SanaDump/SanaSend -- see tools-startup-sequence.txt's own
# comment), so its redirected output file is the only way to check it
# actually ran and probed unit 0 correctly. Every marker below must be a
# literal substring SanaConform's own printf calls produce for a fresh,
# unconfigured echo unit (see sanaconform.c) -- not a full byte-for-byte
# transcript compare, since exact hook-invocation counts/instance
# addresses aren't worth pinning down here. The DMA-hooks marker is
# deliberately the SUCCESS-branch-specific text ("open(s) on this unit
# have offered a DMA hook"), not the shared "sana2loop quirk stat:"
# prefix both the success and failure branches print -- the shared
# prefix alone would make this check pass even if the special-stats
# path regressed to always reporting failure (pre-launch-review fix).
CONFORM=$(mktemp)
if "$XDFTOOL" "$ADF" type s/sana2loop/sanaconform.out > "$CONFORM" 2>/dev/null; then
    for marker in \
        'OpenDevice: OK' \
        'S2_ONLINE: OK' \
        'Self-echo round trip: OK' \
        'S2_PacketFilter invoked: YES' \
        'open(s) on this unit have offered a DMA hook'
    do
        grep -qF "$marker" "$CONFORM" || {
            echo "FAIL: SanaConform output missing expected marker: $marker" >&2
            fails=$((fails + 1))
        }
    done
else
    echo "FAIL: s/sana2loop/sanaconform.out not found on the ADF post-run (raise BENCH?)" >&2
    fails=$((fails + 1))
fi
rm -f "$CONFORM"

# Pre-launch-review addition: SanaInfo had never been executed by any
# automated test before this (see tools-startup-sequence.txt's own
# comment). Same stdout-doesn't-reach-serial reasoning as SanaConform
# above -- checked via its redirected output file instead. Markers match
# src/sanainfo.c's own printf calls for a fresh, unconfigured unit 0.
INFO=$(mktemp)
if "$XDFTOOL" "$ADF" type s/sana2loop/sanainfo.out > "$INFO" 2>/dev/null; then
    for marker in \
        'MTU: 1500' \
        'BPS: 10000000' \
        'HardwareType: 1 (1=Ethernet)' \
        'Station address:'
    do
        grep -qF "$marker" "$INFO" || {
            echo "FAIL: SanaInfo output missing expected marker: $marker" >&2
            fails=$((fails + 1))
        }
    done
else
    echo "FAIL: s/sana2loop/sanainfo.out not found on the ADF post-run (raise BENCH?)" >&2
    fails=$((fails + 1))
fi
rm -f "$INFO"

[ "$fails" -eq 0 ] || exit 1

echo "PASS: SanaDump/SanaSend/SanaConform on-target round-trip test"
