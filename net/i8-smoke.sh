#!/usr/bin/env bash
# net/i8-smoke.sh — i8 (model 295) smoke test: server + live client + bot.
#
# Starts a fresh Release reLCS-server, launches the real reLCS.exe from GAME_DIR
# with -connect, has a bot spawn the i8 (-vehmodel 295), and asserts:
#   - the server came up listening on the test port
#   - the client joined (WELCOME), with no protocol-version mismatch on either side
#   - the bot's i8 spawn was accepted by the server (VEH_SPAWN model=295)
#   - the client survived the spawn window
# Exit status is 0 only when every check passes.
#
# Usage: net/i8-smoke.sh            (GAME_DIR defaults to the reLCS game folder; SMOKE_PORT defaults to 7793)
#        GAME_DIR=/path/to/reLCS net/i8-smoke.sh
set -u
cd "$(dirname "$0")"

PORT=${SMOKE_PORT:-7793}
BIN="$(pwd)/bin/Release"
GAME_DIR="${GAME_DIR:-/c/Users/mattb/Downloads/reLCS}"
OUT="$(pwd)/i8-smoke-out"
NICK=I8Smoke
mkdir -p "$OUT"
rm -f "$OUT"/*.log

SERVER_PID=""
BOT_PID=""
FAIL=0

stop_game() {
	# the game is started from a subshell, so stop it by our unique -nick argument
	powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='reLCS.exe'\" | Where-Object { \$_.CommandLine -like '*$NICK*' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }" > /dev/null 2>&1
}
cleanup() {
	[ -n "$BOT_PID" ] && kill "$BOT_PID" 2>/dev/null
	[ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
	stop_game
	wait 2>/dev/null
	return 0
}
trap cleanup EXIT INT TERM

check() { # check <desc> <cmd...>
	local desc="$1"; shift
	if "$@"; then echo "PASS: $desc"; else echo "FAIL: $desc"; FAIL=1; fi
}

# wait_for <file> <pattern> <timeout_s> [pid]  -> 0 when pattern appears; stops early if pid dies
wait_for() {
	local file="$1" pat="$2" limit="$3" pid="${4:-}" t=0
	while [ "$t" -lt "$limit" ]; do
		grep -q -a -- "$pat" "$file" 2>/dev/null && return 0
		if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then return 1; fi
		sleep 1; t=$((t+1))
	done
	return 1
}

echo "=== starting fresh server on UDP $PORT ==="
"${SERVER_EXE:-$BIN/reLCS-server.exe}" -hostname I8Smoke -port "$PORT" -gamemode survivor \
	> "$OUT/server.log" 2>&1 &
SERVER_PID=$!
check "server listening" wait_for "$OUT/server.log" "listening on UDP $PORT" 20 "$SERVER_PID"

echo "=== launching live client (-connect 127.0.0.1:$PORT) ==="
stop_game
(cd "$GAME_DIR" && ./reLCS.exe -connect 127.0.0.1:$PORT -nick "$NICK" \
	> "$OUT/game.out.log" 2> "$OUT/game.err.log") &
# the game can take ~90s to load the world and join
check "client joined (WELCOME)" wait_for "$OUT/game.err.log" "\[NET\] WELCOME" 150
check "server accepted client nick $NICK" grep -q -a "\[JOIN\].*name=$NICK" "$OUT/server.log"

echo "=== bot spawns the i8 (model 295) ==="
"$BIN/relcs-netbot.exe" -name I8Bot -server 127.0.0.1:$PORT -vehmodel 295 \
	-scenario drive -duration 60 -seed 11 -verbose > "$OUT/bot.log" 2>&1 &
BOT_PID=$!
check "server accepted i8 spawn (VEH_SPAWN model=295)" \
	wait_for "$OUT/bot.log" "VEH_SPAWN id=[0-9]* model=295" 60 "$BOT_PID"

echo "=== protocol and stability checks ==="
check "no protocol mismatch in client log" \
	bash -c "! grep -q -a -i 'protocol version mismatch' '$OUT/game.err.log' '$OUT/game.out.log'"
check "no protocol refusal in server log" \
	bash -c "! grep -q -a -i 'protocol version mismatch' '$OUT/server.log'"
sleep 10
game_alive() { # only our client, matched by its unique -nick argument
	powershell -NoProfile -Command "(Get-CimInstance Win32_Process -Filter \"Name='reLCS.exe'\" | Where-Object { \$_.CommandLine -like '*$NICK*' } | Measure-Object).Count" 2>/dev/null | tr -d '\r' | grep -q '^[1-9]'
}
check "client still running after the spawn window" game_alive

echo ""
if [ "$FAIL" = 0 ]; then echo "i8 smoke: ALL PASS"; else echo "i8 smoke: FAILURES (logs in $OUT)"; fi
exit $FAIL
