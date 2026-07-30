#!/bin/sh
# run.sh -- headless Copperline on-target smoke test for loopback.device.
#
# Builds a minimal bootable ADF (xdftool) with just DEVS:loopback.device,
# C:sana2test, and a one-line Startup-Sequence, then boots it. sana2test
# opens DEVS:loopback.device, exercises S2_DEVICEQUERY/S2_ONLINE/echo, and
# emits PASS/FAIL over serial via RawPutChar. Copperline forwards serial to
# its stdout (`--serial stdout`); we assert the result.
#
# Two modes:
#   KICK=path/to/real-1.3.rom  -- validates actual Kickstart 1.3 fidelity,
#                                  the point of this project. Use this
#                                  locally before releases; the ROM is
#                                  licensed and can't live in public CI
#                                  (see CLAUDE.md's "Testing under
#                                  Copperline" section).
#   KICK unset                 -- boots Copperline's bundled, redistributable
#                                  AROS Kickstart replacement instead. Weaker
#                                  (can't validate 1.3-specific behavior) but
#                                  needs no licensed asset, so this is what
#                                  public CI runs. AROS boots slower, hence
#                                  the bigger default BENCH below (real 1.3's
#                                  own dos.library file I/O -- M5's config
#                                  read -- is measurably SLOWER than AROS's,
#                                  which is why the KICK= default needed
#                                  raising too once that landed).
#
# A plain released `copperline` (e.g. `brew install copperline`, or the
# AppImage in CI) is sufficient for both modes -- this test only ever boots
# from a floppy image, never a [[filesys]] host-directory mount, so it does
# not hit the real 68000+[[filesys]] doorbell bug documented in CLAUDE.md
# (confirmed empirically: released 0.13.0 passes both modes here).
#
# Prereqs:
#   - xdftool on PATH (amitools: `pip install amitools`, or
#     XDFTOOL=/path/to/xdftool)
#   - copperline on PATH, or COPPERLINE=/path/to/copperline
#   - the cross-built device + harness: SANA2LOOP_DEVICE= / SANA2TEST_M68K=
#     (defaults: ../../build/loopback.device, ../../build/sana2test)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

COPPERLINE=${COPPERLINE:-copperline}
KICK=${KICK:-}
XDFTOOL=${XDFTOOL:-xdftool}
DEVICE=${SANA2LOOP_DEVICE:-$ROOT/build/loopback.device}
BIN=${SANA2TEST_M68K:-$ROOT/build/sana2test}
BENCH=${BENCH:-$([ -n "$KICK" ] && echo 30 || echo 40)}
ADF="$HERE/minimal-boot.adf"

[ -e "$DEVICE" ] || { echo "FAIL: missing $DEVICE (run make device)" >&2; exit 2; }
[ -e "$BIN" ] || { echo "FAIL: missing $BIN (run make test-harness)" >&2; exit 2; }
[ -z "$KICK" ] || [ -e "$KICK" ] || { echo "FAIL: KICK set but missing: $KICK" >&2; exit 2; }
command -v "$COPPERLINE" >/dev/null || { echo "FAIL: $COPPERLINE not found" >&2; exit 2; }
command -v "$XDFTOOL" >/dev/null || { echo "FAIL: $XDFTOOL not found (pip install amitools)" >&2; exit 2; }
[ -n "$KICK" ] && echo "ROM: $KICK" || echo "ROM: bundled AROS"

rm -f "$ADF"
"$XDFTOOL" "$ADF" format 'S2LTest' ofs + \
    boot install boot1x + \
    makedir c + makedir s + makedir devs + makedir s/sana2loop + \
    write "$DEVICE" devs/loopback.device + \
    write "$BIN" c/sana2test + \
    write "$HERE/adf-startup-sequence.txt" s/startup-sequence + \
    write "$HERE/fixtures/unit1.config" s/sana2loop/unit1.config + \
    write "$HERE/fixtures/unit2.config" s/sana2loop/unit2.config \
    > /dev/null

OUT=$(mktemp)
cleanup() { rm -f "$OUT"; }
trap cleanup EXIT INT TERM

set -- --config machine.toml --noaudio --serial stdout --benchmark-until "$BENCH"
[ -n "$KICK" ] && set -- "$@" "$KICK"
( cd "$HERE" && "$COPPERLINE" "$@" ) >"$OUT" 2>/dev/null \
    || { echo "FAIL: $COPPERLINE exited non-zero" >&2; cat "$OUT" >&2; exit 3; }

tr -d '\r' <"$OUT" >"$OUT.n" && mv "$OUT.n" "$OUT"
echo "----- serial capture -----"; cat "$OUT"; echo "--------------------------"
grep -q '^END' "$OUT" 2>/dev/null || { echo "FAIL: no END marker (raise BENCH?)" >&2; exit 1; }

# Every named sub-test from sana2test.c must show PASS individually -- not
# just the aggregate RESULT= -- so a regression in one milestone's check
# names itself instead of being buried in one pass/fail bit. Keep this list
# in sync with the NAME_* constants in sana2test.c as milestones add more.
SUBTESTS="online devicequery stationaddress echo offline onevent oneventnow abortio cmdflush bufferhooks delivery configinterface tracktype broadcast multicast multicastaddress raw config"
fails=0
for s in $SUBTESTS; do
    grep -q "^SUB=${s}=PASS$" "$OUT" 2>/dev/null || {
        echo "FAIL: SUB=${s} did not report PASS" >&2
        fails=$((fails + 1))
    }
done
grep -q '^RESULT=PASS' "$OUT" 2>/dev/null || { echo "FAIL: RESULT=PASS not found" >&2; fails=$((fails + 1)); }
[ "$fails" -eq 0 ] || exit 1

echo "PASS: loopback.device on-target smoke test (via serial)"
