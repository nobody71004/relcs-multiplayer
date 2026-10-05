#!/usr/bin/env bash
# net/e2e.sh — reLCS multiplayer end-to-end test run book.
#
# Phase 1: base sync — server + 4 bots (combat x3, drive x1)
#   asserts: bot self-assertions, chat delivery, kills+respawn, vehicle flow,
#            cross-bot state convergence (age-bounded drift), join/leave bookkeeping
# Phase 2: LAN discovery — UDP broadcast probe answered by the server
# Phase 3: master server list — server announce -> master -> launcher query
# Phase 4: degradation — same sync test through net-shaper (100ms +/-30ms, 2% loss)
# Phase 5: live game client — real reLCS.exe auto-start + connect, remote ped
#          creation, bot<->game mutual visibility, survival (no crash)
set -u
cd "$(dirname "$0")"

PORT=7790
RCON=e2epw
OUT="$(pwd)/e2e-out"   # absolute: phase 5 redirects after cd'ing into GAME_DIR
BIN=./bin/Release
mkdir -p "$OUT"
rm -f "$OUT"/*.log

# kill leftovers of a previous aborted run (they hold ports and break bind)
taskkill //F //IM reLCS-server.exe //IM reLCS-master.exe //IM net-shaper.exe \
	//IM relcs-netbot.exe > /dev/null 2>&1
sleep 1

SERVER_PID=0; MASTER_PID=0; SHAPER_PID=0
cleanup() {
	[ "$SERVER_PID" != 0 ] && kill $SERVER_PID 2>/dev/null
	[ "$MASTER_PID" != 0 ] && kill $MASTER_PID 2>/dev/null
	[ "$SHAPER_PID" != 0 ] && kill $SHAPER_PID 2>/dev/null
	[ "${AGENT_PID:-0}" != 0 ] && kill $AGENT_PID 2>/dev/null
	taskkill //F //IM relcs-netbot.exe //IM reLCS-agentd.exe > /dev/null 2>&1
	# game clients are spawned via subshell; kill by our unique -nick arg
	powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='reLCS.exe'\" | Where-Object { \$_.CommandLine -like '*GameE2E*' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }" > /dev/null 2>&1
	wait 2>/dev/null
	return 0
}
trap cleanup EXIT INT TERM

FAIL=0
step() { echo ""; echo "--- $1 ---"; }

# Robust against AV/file-lock flakiness in Downloads: retry the spawn until the
# log file materialises and the process is alive (or already finished).
spawn_bot() { # spawn_bot <logfile> <args...>  -> echoes pid or -1
	local log="$1"; shift
	local try pid
	for try in 1 2 3 4 5; do
		rm -f "$log"
		"$@" > "$log" 2>&1 &
		pid=$!
		sleep 0.5
		if kill -0 "$pid" 2>/dev/null; then
			if [ -f "$log" ]; then echo "$pid"; return 0; fi
			kill "$pid" 2>/dev/null   # alive but no log file -> ghost, retry
		else
			if [ -s "$log" ]; then echo "$pid"; return 0; fi   # finished already
		fi
		echo "spawn retry $try for $log" >&2
		sleep 0.5
	done
	echo "-1"
}

wait_result() { # wait_result <logfile> <timeout_s> -> 0 when [BOT] RESULT appears
	local log="$1"
	local t=0
	while [ $t -lt ${2:-30} ]; do
		grep -q "\[BOT\] RESULT:" "$log" 2>/dev/null && return 0
		sleep 1; t=$((t+1))
	done
	return 1
}

check() { # check <desc> <cmd...>
	local desc="$1"; shift
	if "$@"; then echo "PASS: $desc"; else echo "FAIL: $desc"; FAIL=1; fi
}

echo "=== reLCS multiplayer E2E run book ==="

# ---------------------------------------------------------------- phase 0
step "phase 0: unit tests"
if "$BIN/net-tests.exe" | tail -1 | grep -q "0 failures — PASS"; then
	echo "PASS: unit tests"
else
	echo "FAIL: unit tests"; FAIL=1
fi

# ---------------------------------------------------------------- phase 1
step "phase 1: base sync (server + 4 bots)"
$BIN/reLCS-master.exe > "$OUT/master.log" 2>&1 &
MASTER_PID=$!
$BIN/reLCS-server.exe \
	-port $PORT -maxplayers 16 -gamemode survivor \
	-lan_discover 1 -announce 1 -master 127.0.0.1 -rcon_password $RCON \
	-hostname "E2E Server" > "$OUT/server.log" 2>&1 &
SERVER_PID=$!
sleep 1

BOTS=()
BOTS+=($(spawn_bot "$OUT/alice.log"   $BIN/relcs-netbot.exe -name Alice   -server 127.0.0.1:$PORT -scenario combat -duration 25 -expect-chat BOTMSG -rcon $RCON -seed 11))
BOTS+=($(spawn_bot "$OUT/bob.log"     $BIN/relcs-netbot.exe -name Bob     -server 127.0.0.1:$PORT -scenario combat -duration 25 -expect-chat BOTMSG -rcon $RCON -seed 22))
BOTS+=($(spawn_bot "$OUT/charlie.log" $BIN/relcs-netbot.exe -name Charlie -server 127.0.0.1:$PORT -scenario combat -duration 25 -expect-chat BOTMSG -rcon $RCON -seed 33))
BOTS+=($(spawn_bot "$OUT/driver.log"  $BIN/relcs-netbot.exe -name Driver  -server 127.0.0.1:$PORT -scenario drive  -duration 25 -expect-chat BOTMSG -rcon $RCON -seed 44))

for f in alice bob charlie driver; do
	wait_result "$OUT/$f.log" 40 || FAIL=1
done
for pid in "${BOTS[@]}"; do
	[ "$pid" != "-1" ] && wait $pid 2>/dev/null
	done
sleep 1

for f in alice bob charlie driver; do
	RESULT=$(grep -o "RESULT: [A-Z]*" "$OUT/$f.log" | head -1)
	[ "$RESULT" = "RESULT: PASS" ] && echo "PASS: $f" || { echo "FAIL: $f (${RESULT:-none})"; FAIL=1; }
done

JOINS=$(grep -c "\[JOIN\]" "$OUT/server.log")
LEAVES=$(grep -c "\[LEAVE\]" "$OUT/server.log")
KILLS=$(grep -c "\[KILL\]" "$OUT/server.log")
VEHS=$(grep -c "\[VEH\]" "$OUT/server.log")
echo "server: joins=$JOINS leaves=$LEAVES kills=$KILLS veh_spawns=$VEHS"
[ "$JOINS" -ge 4 ] || { echo "FAIL: expected >=4 joins"; FAIL=1; }
[ "$LEAVES" -ge 4 ] || { echo "FAIL: expected >=4 leaves"; FAIL=1; }
[ "$KILLS" -ge 1 ] || { echo "FAIL: expected >=1 kill"; FAIL=1; }
[ "$VEHS" -ge 1 ] || { echo "FAIL: expected >=1 vehicle spawn"; FAIL=1; }

# Time-aligned convergence with snapshot-age awareness: every SEES sample
# carries the age of the relayed snapshot, so the allowed drift is bounded by
# how far the target itself travelled during that stale window. Windows that
# contain a teleport/respawn are skipped as unverifiable; anything beyond the
# age-bounded drift is a genuine relay failure.
awk '
function dist(x1,y1,z1,x2,y2,z2, dx,dy,dz) {
	dx=x1-x2; dy=y1-y2; dz=z1-z2
	return sqrt(dx*dx+dy*dy+dz*dz)
}
function interp(f, tt,   m, s2, w2, nq) {
	nq = nt[f]
	if (nq < 1) { gx = 0; gy = 0; gz = 0; return }
	if (tt <= iat[f SUBSEP 1])  { gx = iax[f SUBSEP 1];  gy = iay[f SUBSEP 1];  gz = iaz[f SUBSEP 1];  return }
	if (tt >= iat[f SUBSEP nq]) { gx = iax[f SUBSEP nq]; gy = iay[f SUBSEP nq]; gz = iaz[f SUBSEP nq]; return }
	for (m = 1; m < nq; m++) {
		if (iat[f SUBSEP m] <= tt && tt <= iat[f SUBSEP (m+1)]) {
			s2 = iat[f SUBSEP (m+1)] - iat[f SUBSEP m]
			w2 = (s2 > 0) ? (tt - iat[f SUBSEP m]) / s2 : 0
			gx = iax[f SUBSEP m] + (iax[f SUBSEP (m+1)] - iax[f SUBSEP m]) * w2
			gy = iay[f SUBSEP m] + (iay[f SUBSEP (m+1)] - iay[f SUBSEP m]) * w2
			gz = iaz[f SUBSEP m] + (iaz[f SUBSEP (m+1)] - iaz[f SUBSEP m]) * w2
			return
		}
	}
}
/WELCOME id=/ { split($3, wp, "="); ids[FILENAME] = wp[2]+0 }
$2 == "IAM"  { t=$1; gsub(/[\[\]]/, "", t); nt[FILENAME] += 1; k = FILENAME SUBSEP nt[FILENAME];
	iat[k]=t+0; iax[k]=$3+0; iay[k]=$4+0; iaz[k]=$5+0 }
$2 == "SEES" { t=$1; gsub(/[\[\]]/, "", t); key = FILENAME "|" $3; ns[key] += 1; k = key SUBSEP ns[key];
	sst[k]=t+0; ssx[k]=$4+0; ssy[k]=$5+0; ssz[k]=$6+0; sage[k]=0
	for (fld = 7; fld <= NF; fld++) if ($fld ~ /^age=/) { split($fld, wa, "="); sage[k]=wa[2]+0 } }
$2 == "JUMP" { t=$1; gsub(/[\[\]]/, "", t); nj[FILENAME] += 1; jt[FILENAME SUBSEP nj[FILENAME]] = t+0 }
END {
	worst = 0; worstratio = 0; n = 0; rej = 0; bad = 0
	for (f1 in ids) for (f2 in ids) {
		if (f1 == f2) continue
		key = f2 "|" ids[f1]
		pairworst = 0; pairn = 0
		for (i = 1; i <= ns[key]; i++) {
			sk = key SUBSEP i
			# target own position at the SEES instant (time-aligned)
			interp(f1, sst[sk]); ix = gx; iy = gy; iz = gz
			d = dist(ssx[sk],ssy[sk],ssz[sk], ix, iy, iz)
			# drift budget = target path over the snapshot stale window
			tlo = sst[sk] - (sage[sk] + 0.3)
			interp(f1, tlo); cx = gx; cy = gy; cz = gz
			path = 0; jump = 0
			for (q = 1; q <= nt[f1]; q++) {
				tq = iat[f1 SUBSEP q]
				if (tq <= tlo || tq >= sst[sk]) continue
				seg = dist(cx,cy,cz, iax[f1 SUBSEP q],iay[f1 SUBSEP q],iaz[f1 SUBSEP q])
				if (seg > 8) jump = 1
				path += seg
				cx = iax[f1 SUBSEP q]; cy = iay[f1 SUBSEP q]; cz = iaz[f1 SUBSEP q]
			}
			seg = dist(cx,cy,cz, ix,iy,iz)
			if (seg > 8) jump = 1
			path += seg
			# samples whose stale window OR interpolation bracket overlaps a
			# logged own position jump (respawn/correction) cannot be verified
			# at 1 Hz telemetry — widen by one bracket (1.2 s) either side
			for (e = 1; e <= nj[f1]; e++) {
				tj = jt[f1 SUBSEP e]
				if (tj >= tlo - 1.2 && tj <= sst[sk] + 1.2) jump = 1
			}
			if (jump) { rej++; continue }
			tol = 2.0 + path
			n++; pairn++
			if (d > pairworst) pairworst = d
			if (d > worst) worst = d
			if (tol > 0 && d / tol > worstratio) worstratio = d / tol
			if (d > tol) {
				bad = 1
				printf "convergence VIOLATION: %s sees %s (id %d) off by %.2f m (tol %.2f, stale %.2f s)\n", f2, f1, ids[f1], d, tol, sage[sk]
			}
		}
		if (pairn > 0)
			printf "convergence: %s sees %s (id %d): %d samples, worst %.2f m\n", f2, f1, ids[f1], pairn, pairworst
	}
	if (n < 6)  { printf "FAIL: only %d verifiable convergence samples (need >= 6)\n", n; exit 1 }
	if (bad)    { printf "FAIL: convergence violated (worst %.2f m, %.0f%% of tolerance)\n", worst, worstratio * 100; exit 1 }
	printf "convergence OK (%d samples, %d stale-jump skipped, worst %.2f m = %.0f%% of tolerance)\n", n, rej, worst, worstratio * 100
	exit 0
}' "$OUT"/alice.log "$OUT"/bob.log "$OUT"/charlie.log "$OUT"/driver.log || FAIL=1

# ---------------------------------------------------------------- phase 2
step "phase 2: LAN discovery"
$BIN/relcs-netbot.exe -discover -name Probe > "$OUT/discover.log" 2>&1
grep -q "RESULT: PASS" "$OUT/discover.log" && echo "PASS: LAN discovery answered" || {
	echo "FAIL: LAN discovery"; cat "$OUT/discover.log"; FAIL=1; }

# ---------------------------------------------------------------- phase 3
step "phase 3: master server list"
$BIN/relcs-netbot.exe -master-query -server 127.0.0.1:7800 -name MasterProbe > "$OUT/masterq.log" 2>&1
grep -q "RESULT: PASS" "$OUT/masterq.log" && echo "PASS: master lists E2E Server" || {
	echo "FAIL: master list"; cat "$OUT/masterq.log"; FAIL=1; }

# ---------------------------------------------------------------- phase 3b
step "phase 3b: launcher GUI (selftest: master + LAN queries)"
(cd . && $BIN/reLCS-launcher.exe -selftest > /dev/null 2>&1)
if grep -q "\[SELFTEST\] RESULT: PASS" launcher.log 2>/dev/null; then
	echo "PASS: launcher sees server via master + LAN"
else
	echo "FAIL: launcher selftest"; cat launcher.log 2>/dev/null; FAIL=1
fi

# ---------------------------------------------------------------- phase 4
step "phase 4: degradation (100ms +/-30ms jitter, 2% loss via net-shaper)"
$BIN/net-shaper.exe -listen 9000 -target 127.0.0.1:$PORT -latency 100 -jitter 30 -loss 2 > "$OUT/shaper.log" 2>&1 &
SHAPER_PID=$!
sleep 1

DBOTS=()
DBOTS+=($(spawn_bot "$OUT/dega.log" $BIN/relcs-netbot.exe -name DegA -server 127.0.0.1:9000 -scenario combat -duration 20 -expect-chat BOTMSG -rcon $RCON -seed 55))
DBOTS+=($(spawn_bot "$OUT/debg.log" $BIN/relcs-netbot.exe -name DegB -server 127.0.0.1:9000 -scenario combat -duration 20 -expect-chat BOTMSG -rcon $RCON -seed 66))
for f in dega debg; do
  wait_result "$OUT/$f.log" 30 || FAIL=1
done
for pid in "${DBOTS[@]}"; do
	[ "$pid" != "-1" ] && wait $pid 2>/dev/null
done

for f in dega debg; do
	RESULT=$(grep -o "RESULT: [A-Z]*" "$OUT/$f.log" 2>/dev/null | head -1)
	[ "$RESULT" = "RESULT: PASS" ] && echo "PASS: $f (degraded)" || { echo "FAIL: $f (${RESULT:-none})"; FAIL=1; }
done
grep -q "forwarded=" "$OUT/shaper.log" && echo "PASS: shaper forwarded traffic" || { echo "FAIL: shaper idle"; FAIL=1; }

# ---------------------------------------------------------------- phase 5
step "phase 5: live game client (reLCS.exe -connect, auto-start, remote ped)"
GAME_DIR="${GAME_DIR:-/c/Users/mattb/Downloads/reLCS}"
if [ -x "$GAME_DIR/reLCS.exe" ]; then
	# clear a stale game client from a previous run (match our -nick arg)
	powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='reLCS.exe'\" | Where-Object { \$_.CommandLine -like '*GameE2E*' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }" > /dev/null 2>&1
	GAME_PID=-1
	(cd "$GAME_DIR" && ./reLCS.exe -connect 127.0.0.1:$PORT -nick GameE2E \
		> "$OUT/game.out.log" 2> "$OUT/game.err.log") &
	GAME_PID=$!
	# long-lived: the game can take >90s to load and join; we kill it later
	GBOT_PID=$(spawn_bot "$OUT/gamebot.log" $BIN/relcs-netbot.exe -name GameBot \
		-server 127.0.0.1:$PORT -scenario roam -duration 600 -verbose -seed 77)

	# the game auto-starts a new game (~40s), connects, then creates the
	# remote ped for GameBot
	gt=0
	while [ $gt -lt 120 ]; do
		grep -q "remote ped id=" "$OUT/game.err.log" 2>/dev/null && break
		kill -0 $GAME_PID 2>/dev/null || break
		sleep 1; gt=$((gt+1))
	done
	check "game auto-started + connected + created remote ped" \
		grep -q "remote ped id=" "$OUT/game.err.log"
	check "game log shows WELCOME" grep -q "\[NET\] WELCOME" "$OUT/game.err.log"
	check "server sees game with requested nick (GameE2E)" \
		grep -q "\[JOIN\].*name=GameE2E" "$OUT/server.log"

	# GameBot must observe the game player's state
	bt=0
	while [ $bt -lt 60 ]; do
		grep -q " SEES " "$OUT/gamebot.log" 2>/dev/null && break
		sleep 1; bt=$((bt+1))
	done
	check "bot sees the game player" grep -q " SEES " "$OUT/gamebot.log"
	check "game survived the sync window" kill -0 $GAME_PID 2>/dev/null

	kill $GAME_PID 2>/dev/null
	# kill $GAME_PID only reaps the subshell; stop the game by our -nick arg
	powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='reLCS.exe'\" | Where-Object { \$_.CommandLine -like '*GameE2E*' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }" > /dev/null 2>&1
	if [ "$GBOT_PID" != "-1" ]; then kill $GBOT_PID 2>/dev/null; wait $GBOT_PID 2>/dev/null; fi
	sleep 1
else
	echo "SKIP: game client not found at $GAME_DIR (set GAME_DIR=...)"
fi

# ---------------------------------------------------------------- phase 6
step "phase 6: LLM agent pipeline (reLCS-agentd bridge + harness)"
AGENT_PID=0
"$BIN/reLCS-agentd.exe" -http 7811 -server 127.0.0.1:$PORT -rcon $RCON \
	-web "$(pwd)/agent/webhud.html" > "$OUT/agentd.log" 2>&1 &
AGENT_PID=$!
sleep 1
check "agent bridge serves LLM tool schema" \
	sh -c "curl -sf http://127.0.0.1:7811/v1/tools | grep -q server_spawn_bot"
check "agent bridge serves web HUD" \
	sh -c "curl -sf http://127.0.0.1:7811/ | grep -q 'LCS'"
check "agent rcon reaches the game server" \
	sh -c "curl -sf -X POST http://127.0.0.1:7811/v1/rcon -H 'X-Rcon: $RCON' -H 'Content-Type: application/json' -d '{\"command\":\"version\"}' | grep -q protocol"
check "agent spawn-bot endpoint" \
	sh -c "curl -sf -X POST http://127.0.0.1:7811/v1/bot -H 'X-Rcon: $RCON' -H 'Content-Type: application/json' -d '{\"name\":\"AgentE2E\",\"scenario\":\"roam\",\"duration\":30}' | grep -q spawned"
# native python needs a windows-style path
HARNESS_PATH=$(cygpath -w "$(pwd)/agent/agent_harness.py")
python "$HARNESS_PATH" --mock --base http://127.0.0.1:7811 --rcon $RCON \
	> "$OUT/harness.log" 2>&1
check "llm harness mock pipeline" grep -q "RESULT: PASS" "$OUT/harness.log"
kill $AGENT_PID 2>/dev/null
AGENT_PID=0

# ---------------------------------------------------------------- teardown
step "teardown"
cleanup
trap - EXIT INT TERM
sleep 1

echo ""
if [ $FAIL -eq 0 ]; then
	echo "=== E2E RUN BOOK: ALL GREEN ==="
else
	echo "=== E2E RUN BOOK: FAILURES PRESENT ==="
fi
exit $FAIL
