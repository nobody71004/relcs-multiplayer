#!/usr/bin/env bash
# net/fight-smoke.sh — fight scenario end to end: kills, deaths, scores, persistence.
#
# Starts a fresh Release reLCS-server (isolated work dir so profiles start empty),
# runs two fight bots that converge and deal damage, and asserts:
#   - both bots PASS (saw a death and a scoreboard update each)
#   - at least one kill happened (kills=N with N>=1 in a RESULT line)
#   - the killer's score/kills survived a reconnect (score persistence):
#     the rejoining bot's profile loads with kills>=1 (server [PROFILE] log)
# Exit status is 0 only when every check passes.
#
# Usage: net/fight-smoke.sh            (SMOKE_PORT defaults to 7794)
set -u
cd "$(dirname "$0")"

PORT=${SMOKE_PORT:-7794}
BIN="$(pwd)/bin/Release"
OUT="$(pwd)/fight-smoke-out"
mkdir -p "$OUT"
rm -f "$OUT"/*.log
rm -rf "$OUT/srv" && mkdir -p "$OUT/srv"

PIDS=""
FAIL=0
cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null; done
	wait 2>/dev/null
	return 0
}
trap cleanup EXIT INT TERM

check() { # check <desc> <cmd...>
	local desc="$1"; shift
	if "$@"; then echo "PASS: $desc"; else echo "FAIL: $desc"; FAIL=1; fi
}

# wait_for <file> <pattern> <timeout_s> [pid]  -> 0 when pattern appears
wait_for() {
	local file="$1" pat="$2" limit="$3" pid="${4:-}" t=0
	while [ "$t" -lt "$limit" ]; do
		grep -q -a -- "$pat" "$file" 2>/dev/null && return 0
		if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then return 1; fi
		sleep 1; t=$((t+1))
	done
	return 1
}

echo "=== clearing stale test processes (port $PORT / FightSmoke) ==="
# a previous hung run leaves a server holding the port; new server then fails
# to bind and bots silently test against the stale one. Match by command line
# so a real server on another port is never touched.
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='reLCS-server.exe'\" | Where-Object { \$_.CommandLine -like '*FightSmoke*' -or \$_.CommandLine -like '*:$PORT*' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }" > /dev/null 2>&1
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='relcs-netbot.exe'\" | Where-Object { \$_.CommandLine -like '*:$PORT*' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }" > /dev/null 2>&1
sleep 2

echo "=== starting fresh server on UDP $PORT (isolated profiles) ==="
sh -c "cd \"$OUT/srv\" && exec \"$BIN/reLCS-server.exe\" -hostname FightSmoke -port \"$PORT\" -gamemode survivor > \"$OUT/server.log\" 2>&1" &
SERVER_PID=$!
PIDS="$PIDS $SERVER_PID"
check "server listening" wait_for "$OUT/server.log" "listening on UDP $PORT" 20 "$SERVER_PID"
check "server bound the port" bash -c "! grep -q -a 'cannot bind' '$OUT/server.log'"

echo "=== two fight bots (converge + damage, 45s) ==="
"$BIN/relcs-netbot.exe" -name FighterA -password fightpw -server 127.0.0.1:$PORT \
	-scenario fight -duration 45 -seed 11 -verbose > "$OUT/a.log" 2>&1 &
PIDA=$!
"$BIN/relcs-netbot.exe" -name FighterB -password fightpw -server 127.0.0.1:$PORT \
	-scenario fight -duration 45 -seed 22 -verbose > "$OUT/b.log" 2>&1 &
PIDB=$!
PIDS="$PIDS $PIDA $PIDB"
check "FighterA finished" wait_for "$OUT/a.log" "RESULT:" 70 "$PIDA"
check "FighterB finished" wait_for "$OUT/b.log" "RESULT:" 70 "$PIDB"
PIDS="$SERVER_PID"

echo "=== fight checks ==="
check "FighterA PASS" grep -q -a "RESULT: PASS" "$OUT/a.log"
check "FighterB PASS" grep -q -a "RESULT: PASS" "$OUT/b.log"
check "a death was seen" grep -q -a "DEATH victim=" "$OUT/a.log" "$OUT/b.log"
check "a kill was scored" bash -c "grep -h -a -o 'kills=[0-9]*' '$OUT/a.log' '$OUT/b.log' | grep -q -a -v 'kills=0'"
check "scoreboard covered a fighter" grep -q -a "SCORE me=" "$OUT/a.log" "$OUT/b.log"

echo "=== persistence: rejoin FighterA, profile must restore kills ==="
"$BIN/relcs-netbot.exe" -name FighterA -password fightpw -server 127.0.0.1:$PORT \
	-scenario solo -duration 12 -seed 33 > "$OUT/rejoin.log" 2>&1 &
PIDR=$!
PIDS="$PIDS $PIDR"
check "rejoin finished" wait_for "$OUT/rejoin.log" "RESULT:" 30 "$PIDR"
check "rejoin PASS" grep -q -a "RESULT: PASS" "$OUT/rejoin.log"
check "profile restored kills>=1" \
	bash -c "grep -a 'PROFILE.*loaded name=FighterA' '$OUT/server.log' | tail -1 | grep -q -a -E 'kills=[1-9]'"

echo ""
if [ "$FAIL" = 0 ]; then echo "fight smoke: ALL PASS"; else echo "fight smoke: FAILURES (logs in $OUT)"; fi
exit $FAIL
