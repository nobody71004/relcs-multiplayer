#!/bin/bash
# net/deploy/proxmox/stop-servers.sh — stop the master + instances started by
# start-servers.sh. Uses the pid files in $ROOT/run and verifies the pid is
# really one of our binaries before signalling it (never a bare `pkill -f`:
# that pattern also matches the shell running the command).
# no `set -e`: a missing pid file is a normal condition here and must not abort
# the remaining stops
set -uo pipefail

ROOT=${ROOT:-$HOME/relcs}
RUN="$ROOT/run"

stop_one() { # stop_one <pidfile>
  local pf=$1 pid name
  name=$(basename "$pf" .pid)
  [ -f "$pf" ] || { echo "== $name: no pid file"; return 0; }
  pid=$(cat "$pf")
  if [ -z "$pid" ] || [ ! -d "/proc/$pid" ] || ! grep -qa "reLCS" "/proc/$pid/cmdline"; then
    echo "== $name: stale pid file, removing"
    rm -f "$pf"
    return 0
  fi
  kill "$pid"
  for _ in $(seq 1 20); do
    [ -d "/proc/$pid" ] || break
    sleep 0.25
  done
  if [ -d "/proc/$pid" ]; then
    echo "== $name (pid $pid) did not exit, sending SIGKILL"
    kill -9 "$pid" || true
  else
    echo "== $name (pid $pid) stopped"
  fi
  rm -f "$pf"
}

# instances first so they disconnect from the master cleanly, then the master
for pf in "$RUN"/server-*.pid; do
  [ -e "$pf" ] || continue      # unmatched glob = nothing to stop
  stop_one "$pf"
done
if [ -e "$RUN/master.pid" ]; then
  stop_one "$RUN/master.pid"
fi
echo "== done"
