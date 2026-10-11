# Local Proxmox deployment — master + 5 instances (32 players each)

Target host: `xerox@192.168.0.213` — "ubuntu-cynosure", the Proxmox-hosted Ubuntu
24.04 VM on the local network (16 vCPU, 6 GB RAM). Everything is UDP; there is no
TCP game port.

## What runs where

```
/opt (unrelated services live here too — do not touch)
/home/xerox/relcs/
  net/bin/            reLCS-server  reLCS-master  relcs-netbot  relcs-net-tests
  net/bin.bak-*/      previous binaries, kept per switchover
  deploy/             this folder (start-servers.sh, stop-servers.sh)
  servers/N.cfg       per-instance configs (generated once, then hand-edited)
  resources/          resource packs, shared read-only by all instances
  logs/               master.log, server-N.log, boot.log
  run/                master.pid, server-N.pid
  master.db           master-side favorites (SQLite)
  profiles*.db        per-instance player profiles (instance 1 keeps profiles.db)
  relcs-backup-*.tgz  pre-update backup of the previous deployment
```

| Instance | Port | Hostname | Profiles DB | LAN discovery |
|---|---|---|---|---|
| master | 7800 | — | `master.db` (favorites) | — |
| 1 | 7777 | reLCS Proxmox | `profiles.db` | yes (7778) |
| 2 | 7781 | reLCS Proxmox #2 | `profiles-2.db` | no |
| 3 | 7782 | reLCS Proxmox #3 | `profiles-3.db` | no |
| 4 | 7783 | reLCS Proxmox #4 | `profiles-4.db` | no |
| 5 | 7784 | reLCS Proxmox #5 | `profiles-5.db` | no |

`maxplayers=32` on all five. Port 7778 (LAN discovery) is a single fixed listener,
so only instance 1 enables it; the others are found through the master.

Each instance keeps its own profiles database — five processes sharing one SQLite
file would contend on the write lock.

## Update / (re)start

```bash
# from the repo: build Linux binaries, then copy them over
wsl -e bash -lc 'cd /mnt/c/.../net && ./build-linux.sh'
scp -i <key> net/bin/reLCS-server net/bin/reLCS-master net/bin/relcs-netbot \
    net/bin/relcs-net-tests xerox@192.168.0.213:~/relcs/net/bin.new/
ssh xerox@192.168.0.213 '
  cd ~/relcs
  tar czf relcs-backup-$(date +%Y%m%d-%H%M%S).tgz net/bin servers profiles.db resources
  ~/relcs/deploy/stop-servers.sh
  mv net/bin net/bin.bak-$(date +%Y%m%d-%H%M%S); mv net/bin.new net/bin
  chmod +x net/bin/*; RCON=<pw> ~/relcs/deploy/start-servers.sh'
```

`start-servers.sh` is idempotent: an already-running master/instance is detected
by process name **and** its own command line (`-port 7800`, `-config <path>`), so a
stale pid file can never start a duplicate listener. `stop-servers.sh` only signals
pids from `run/*.pid` after checking they are ours — never a bare `pkill -f`
pattern, which also matches the shell running the command.

A `@reboot` cron entry (user crontab) runs `start-servers.sh` 20 s after boot, so
the master and all five instances come back automatically:

```bash
crontab -l      # @reboot sleep 20 && RCON=<pw> /bin/bash /home/xerox/relcs/deploy/start-servers.sh >> ...
```

## Announcing through the LAN IP (not loopback)

The instance configs use `master=192.168.0.213`, not `127.0.0.1`. Reason: the
master only trusts the IP a server announces *from* when it is not loopback. With
a loopback source it falls back to the address the server advertises
(`LanIPv4()` = first RFC1918 address it finds), and on a host with container
bridges that can be a bridge address (e.g. `172.17.0.1`) that LAN clients cannot
reach — the same failure that was diagnosed on the staging VPS
(`net/internet-test/RUNBOOK.md` §4).

## Verify

```bash
# server list as seen by a LAN client (run from another machine):
relcs-netbot -master-query -server 192.168.0.213:7800
#  -> [MASTER] 192.168.0.213:<port> "reLCS Proxmox #N" 0/32 mode=survivor
#  -> [BOT] RESULT: PASS master_entries=5

# one client per instance:
for p in 7777 7781 7782 7783 7784; do
  relcs-netbot -name Probe$p -server 192.168.0.213:$p -scenario solo -duration 7
done   # -> WELCOME host=reLCS Proxmox #N   [BOT] RESULT: PASS
```

Player-limit gate: verified with a throwaway instance (`maxplayers=2` on 7785) —
two bots filled the slots (`RESULT: PASS`), the third was disconnected with
`KICKED/DISC: server full`. The five live instances use the same code path with
`maxplayers=32`.

## Notes

- The VM also runs unrelated services on UDP/TCP 11780-11800 and the container
  runtime; leave those alone. No host firewall rule was needed for the LAN — the
  new ports are plain `0.0.0.0` UDP listeners and were reachable from the LAN
  immediately. Exposing the host beyond the LAN (port-forward on the Proxmox
  host's NAT) needs the same allowlist treatment as the staging VPS.
- `rcon_password` lives in `servers/N.cfg` (currently the historical `admin`).
  Rotate it before exposing the host outside the LAN, and keep those files out of
  any public repo.
- `vehicles=vehicles.cfg` is configured but the file does not exist yet, so no
  static vehicle spawns are created (one harmless log line per instance).
- Proxmox guests: update the VM from inside (`stop/start` scripts above); no
  change is required on the Proxmox host itself. Before a manual switchover, a
  snapshot of the VM is the cheap rollback (the `relcs-backup-*.tgz` above only
  covers the game deployment).
