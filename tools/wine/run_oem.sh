#!/bin/sh
# Run the unmodified OEM stack (tlx.dll -> TLB.dll) under Wine against the
# SIMULATOR (tests/support/sim_server.cpp) - never a scanner: pkusb.dll
# connects only to 127.0.0.1:5140, where pakon_sim_server listens.
#
# One-time prefix setup (docs/OEM_RE.md §11 lists it step by step):
# OEM "F-X35 COM SERVER" copied to $INSTALL, regsvr32 tlx/TLA/TLB/TLC,
# VERSION.dll -> pkusb.dll import patch in the COPIES of TLB.dll/tlx.dll,
# pkusb.dll (pakon-tlx-macos src/, i686 mingw) and oem_client.exe
# (tools/wine/oem_client.cpp) in $INSTALL, minilab.reg + base-config.reg.
#
#   tools/wine/run_oem.sh NAME init [server args...]
#   tools/wine/run_oem.sh NAME scan RES COLOR CONTROL [server args...]
# writes $RUNS/NAME.jsonl (corpus schema), NAME.out (client), NAME.srv.
set -eu
NAME=$1; shift
MODE=$1; shift
CLIENT_ARGS=$MODE
if [ "$MODE" = scan ]; then CLIENT_ARGS="scan $1 $2 $3"; shift 3; fi
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WINE=${WINE:-$HOME/tools/wine-11.19-amd64-wow64/bin/wine}
export WINEPREFIX="${WINEPREFIX:-$HOME/wine-pakon}" WINEDEBUG="${WINEDEBUG:--all}"
export PAKON_NO_FILM_UNLOCK=1
INSTALL=${INSTALL:-$WINEPREFIX/drive_c/Pakon/F-X35 COM Server}
RUNS=${RUNS:-$HOME/re/wine/runs}
mkdir -p "$RUNS"

"$ROOT/build/tests/pakon_sim_server" --port 5140 --log "$RUNS/$NAME.jsonl" "$@" \
    2>"$RUNS/$NAME.srv" &
SERVER=$!
trap 'kill -INT $SERVER 2>/dev/null; wait $SERVER 2>/dev/null || true' EXIT
sleep 0.5
cd "$INSTALL"
# shellcheck disable=SC2086
timeout 900 "$WINE" oem_client.exe $CLIENT_ARGS >"$RUNS/$NAME.out" 2>"$RUNS/$NAME.err" || true
for log in Main Scan; do
    cp -f "Logs/PakonErrorLog$log.txt" "$RUNS/$NAME.PakonErrorLog$log.txt" 2>/dev/null || true
done
