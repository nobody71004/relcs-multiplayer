#!/usr/bin/env bash
# net/e2e-bots.sh — portable server + bots end-to-end run (Linux, or Git Bash on
# Windows). Covers every e2e.sh phase that needs no game client, discovery,
# master or net-shaper, plus the player/vehicle convergence scenarios:
#
# Phase 0: unit tests
# Phase 1: base sync — server + 4 bots (combat x3, drive x1), cross-bot
#          convergence, vehicle single-entity integrity
# Phase 2: concurrent drivers — 3 bots each driving their own car
# Phase 3: late join — a driver takes a car of its own (VEH_ENTER 0xFFFF, the
#          game's `ride` cheat) and drives; a second client joins 8 s later and
#          must see the player (MSG_SPAWN + state), the car, the occupancy and
#          the car moving, converging with where the driver really is
# Phase 4: lost relocation — the driver's car is warped 150 m and the flagged
#          state packets are lost in transit: the server must re-anchor instead
#          of freezing (or kicking) the driver, and the observer must converge
# Phase 5: out-of-range parked car — the car drives out of the observer's
#          streaming range, parks and is left; the observer then relocates next
#          to it and must see it where it really is (stream-in), not where it
#          was last relayed
#
# Usage: ./e2e-bots.sh            (build first: ./build-linux.sh)
set -u
cd "$(dirname "$0")"

if [ -x ./bin/reLCS-server ]; then
	BIN=./bin; EXE=""
else
	BIN=./bin/Release; EXE=".exe"
fi
SERVER="$BIN/reLCS-server$EXE"
BOT="$BIN/relcs-netbot$EXE"
TESTS="$BIN/relcs-net-tests$EXE"
[ -x "$TESTS" ] || TESTS="$BIN/net-tests$EXE"
PORT=${PORT:-7790}
RCON=e2epw
OUT="$(pwd)/e2e-out"
mkdir -p "$OUT"
rm -f "$OUT"/*.log

SERVER_PID=0
BOT_PIDS=()
cleanup() {
	for pid in "${BOT_PIDS[@]}"; do kill "$pid" 2>/dev/null; done
	[ "$SERVER_PID" != 0 ] && kill "$SERVER_PID" 2>/dev/null
	wait 2>/dev/null
	SERVER_PID=0
	BOT_PIDS=()
	return 0
}
trap cleanup EXIT INT TERM

FAIL=0
step() { echo ""; echo "--- $1 ---"; }
fail() { echo "FAIL: $1"; FAIL=1; }

start_server() { # start_server <log> <extra args...>
	local log="$1"; shift
	# no profile DB / static cars / resources: every run starts from a clean world
	"$SERVER" -port $PORT -maxplayers 16 -lan_discover 0 -announce 0 \
		-rcon_password $RCON -profiles "" -vehicles "" -resources "" \
		-hostname "E2E Server" "$@" > "$log" 2>&1 &
	SERVER_PID=$!
	sleep 1
	kill -0 $SERVER_PID 2>/dev/null || { fail "server did not start ($log)"; SERVER_PID=0; }
}

stop_server() {
	[ "$SERVER_PID" != 0 ] && kill $SERVER_PID 2>/dev/null && wait $SERVER_PID 2>/dev/null
	SERVER_PID=0
}

bot() { # bot <name> <args...>  (log: $OUT/<name>.log)
	local name="$1"; shift
	"$BOT" -name "$name" -server 127.0.0.1:$PORT "$@" > "$OUT/$name.log" 2>&1 &
	BOT_PIDS+=($!)
}

wait_bots() {
	for pid in "${BOT_PIDS[@]}"; do wait "$pid" 2>/dev/null; done
	BOT_PIDS=()
}

result() { # result <name> -> PASS/FAIL line
	local r
	r=$(grep -o "RESULT: [A-Z]*.*" "$OUT/$1.log" | head -1)
	case "$r" in
		"RESULT: PASS"*) echo "PASS: $1" ;;
		*) fail "$1 (${r:-no result})" ;;
	esac
}

veh_integrity() { # veh_integrity <name> [maxlive]
	local S DUP FORCED MAXLIVE
	S=$(grep "\[BOT\] VEHSUMMARY" "$OUT/$1.log" | tail -1)
	echo "veh-integrity $1: ${S#*VEHSUMMARY }"
	DUP=$(echo "$S" | sed -n 's/.*dup_spawns=\([0-9]*\).*/\1/p')
	FORCED=$(echo "$S" | sed -n 's/.*forced_self_enters=\([0-9]*\).*/\1/p')
	MAXLIVE=$(echo "$S" | sed -n 's/.*max_live=\([0-9]*\).*/\1/p')
	[ "${DUP:-1}" = "0" ] || fail "$1 observed ${DUP:-?} duplicate vehicle spawn(s)"
	[ "${FORCED:-1}" = "0" ] || fail "$1 force-seated into ${FORCED:-?} car(s) it never entered"
	if [ -n "${2:-}" ]; then
		[ "${MAXLIVE:-99}" -le "$2" ] || fail "$1 saw ${MAXLIVE:-?} live vehicles (expected <= $2)"
	fi
}

echo "=== reLCS multiplayer E2E (server + bots) ==="

# ---------------------------------------------------------------- phase 0
step "phase 0: unit tests"
if "$TESTS" | tail -1 | grep -q "0 failures — PASS"; then
	echo "PASS: unit tests"
else
	fail "unit tests"
fi

# ---------------------------------------------------------------- phase 1
step "phase 1: base sync (server + 4 bots)"
start_server "$OUT/server1.log" -gamemode survivor
bot Alice   -scenario combat -duration 25 -expect-chat BOTMSG -rcon $RCON -seed 11
bot Bob     -scenario combat -duration 25 -expect-chat BOTMSG -rcon $RCON -seed 22
bot Charlie -scenario combat -duration 25 -expect-chat BOTMSG -rcon $RCON -seed 33
bot Driver  -scenario drive  -duration 25 -expect-chat BOTMSG -rcon $RCON -seed 44
wait_bots
for f in Alice Bob Charlie Driver; do result $f; veh_integrity $f; done
JOINS=$(grep -c "\[JOIN\]" "$OUT/server1.log")
LEAVES=$(grep -c "\[LEAVE\]" "$OUT/server1.log")
KILLS=$(grep -c "\[KILL\]" "$OUT/server1.log")
VEHS=$(grep -c "\[VEH\]" "$OUT/server1.log")
echo "server: joins=$JOINS leaves=$LEAVES kills=$KILLS veh_events=$VEHS"
[ "$JOINS" -ge 4 ] || fail "expected >=4 joins"
[ "$LEAVES" -ge 4 ] || fail "expected >=4 leaves"
[ "$KILLS" -ge 1 ] || fail "expected >=1 kill"
[ "$VEHS" -ge 1 ] || fail "expected >=1 vehicle spawn"
awk -v MODE=player -f e2e-converge.awk "$OUT"/Alice.log "$OUT"/Bob.log "$OUT"/Charlie.log "$OUT"/Driver.log \
	|| fail "phase 1 player convergence"

# ---------------------------------------------------------------- phase 2
step "phase 2: concurrent drivers (single entity + scoped occupancy)"
bot VDrv1 -scenario drive -duration 30 -vehmodel 130 -seed 101
bot VDrv2 -scenario drive -duration 30 -vehmodel 131 -seed 202
bot VDrv3 -scenario drive -duration 30 -vehmodel 132 -seed 303
wait_bots
for f in VDrv1 VDrv2 VDrv3; do
	result $f
	veh_integrity $f 3
	[ "$(grep -c "VEH_ENTER self" "$OUT/$f.log")" -ge 1 ] || fail "$f never entered a vehicle"
done
stop_server

# ---------------------------------------------------------------- phase 3
step "phase 3: late join (driver in a car of its own, observer joins later)"
start_server "$OUT/server3.log" -gamemode freeroam
bot Cruiser -scenario cruise -duration 30 -vehmodel 151 -seed 7
sleep 8
bot LateWatcher -scenario watch -duration 15 -expect-driver -seed 8
wait_bots
result Cruiser; result LateWatcher
veh_integrity LateWatcher 1
grep "\[BOT\] WATCH" "$OUT/LateWatcher.log"
awk -v MODE=player -v MIN=6 -f e2e-converge.awk "$OUT"/Cruiser.log "$OUT"/LateWatcher.log \
	|| fail "phase 3 player convergence"
awk -v MODE=vehicle -v MIN=6 -f e2e-converge.awk "$OUT"/Cruiser.log "$OUT"/LateWatcher.log \
	|| fail "phase 3 vehicle convergence"
# the car must be created where the driver's local car is, not where the
# driver last stood on foot
CAR=$(grep " CAR " "$OUT/Cruiser.log" | head -1 | awk '{print $3, $4, $5}')
SPAWNAT=$(grep "\[VEH\] spawn id=" "$OUT/server3.log" | head -1 | sed -n 's/.* at (\([^)]*\)).*/\1/p' | tr -d ',')
echo "car created locally at ($CAR), server placed it at ($SPAWNAT)"
echo "$CAR $SPAWNAT" | awk '{ d = sqrt(($1-$4)^2 + ($2-$5)^2 + ($3-$6)^2); printf "new-vehicle placement error %.2f m\n", d; exit (d <= 1.0) ? 0 : 1 }' \
	|| fail "phase 3 new vehicle placed away from the local car"
stop_server

# ---------------------------------------------------------------- phase 4
step "phase 4: lost relocation (150 m warp, flagged packets dropped)"
# a wide streaming range keeps both sides of the warp inside the observer's AOI:
# this phase isolates the reference-freeze, not streaming
start_server "$OUT/server4.log" -gamemode freeroam -stream_distance 1000
bot Warper -scenario cruise -duration 30 -vehmodel 151 -jump-at 8 -jump-dist 150 -drop-jump -seed 9
sleep 3
bot WarpWatcher -scenario watch -duration 22 -expect-driver -seed 10
wait_bots
result Warper; result WarpWatcher
grep "\[BOT\] CRUISE" "$OUT/Warper.log"
grep "\[BOT\] WATCH" "$OUT/WarpWatcher.log"
grep -E "\[resync\]" "$OUT/server4.log" | head -4
awk -v MODE=vehicle -v MIN=6 -f e2e-converge.awk "$OUT"/Warper.log "$OUT"/WarpWatcher.log \
	|| fail "phase 4 vehicle convergence after the warp"
awk -v MODE=player -v MIN=6 -f e2e-converge.awk "$OUT"/Warper.log "$OUT"/WarpWatcher.log \
	|| fail "phase 4 player convergence after the warp"
stop_server

# ---------------------------------------------------------------- phase 5
step "phase 5: out-of-range parked car (stream-in on entering range)"
start_server "$OUT/server5.log" -gamemode freeroam
# park ~280+ m from every spawn point (streaming range 200 m). Tolerance 3 m:
# like the game client, the driver streams no state on the tick it gets out,
# so the server holds the car one tick (2 m at 40 m/s) short of where it stops
bot Parker -scenario cruise -duration 30 -vehmodel 151 -speed 40 -drive-to 1133,-980,23 -seed 11
bot ParkWatcher -scenario watch -duration 26 -goto-at 18 -goto 1143,-980,23 \
	-expect-vehicle-near 1133,-980,23,3 -seed 12
wait_bots
result Parker; result ParkWatcher
grep "PARKED" "$OUT/Parker.log"
grep "\[BOT\] WATCH" "$OUT/ParkWatcher.log"
stop_server

# ---------------------------------------------------------------- teardown
step "teardown"
cleanup
trap - EXIT INT TERM
echo ""
if [ $FAIL -eq 0 ]; then
	echo "=== E2E (server + bots): ALL GREEN ==="
else
	echo "=== E2E (server + bots): FAILURES PRESENT ==="
fi
exit $FAIL
