# reLCS Multiplayer — Internet Test Run-Book (staging)

Status: rehearsed on staging (see `DRYRUN-REPORT.md`). Protocol constants live in
`src/net/rnet_common.h`; all ports are **UDP** and all game traffic (including rcon)
rides the game port — there is no separate TCP game port.

## 1. Topology

```
 home clients (any NAT)              staging VPS (public IPv4)
┌──────────────────────┐   UDP 7777   ┌────────────────────────────┐
│ reLCS.exe / launcher ├─────────────►│ reLCS-server  (game port)  │
│ (outbound UDP only)  │              │   announce every 30 s ──┐  │
└──────────────────────┘              │ reLCS-master  (UDP 7800)◄┘  │
   launcher queries master            │   (same box = loopback src) │
   UDP 7800 ─────────────────────────►│   serves server list        │
                                      └────────────────────────────┘
```

## 2. Port map

| Port | Proto | Purpose | Internet-facing |
|------|-------|---------|-----------------|
| 7777 | UDP | Game traffic: connect, state sync (20 Hz), chat, **rcon (in-band)** | yes |
| 7778 | UDP | LAN discovery (raw-UDP broadcast) — LAN only, useless on the internet | **no — disable** (`lan_discover=0`) |
| 7800 | UDP | Master: `MSG_MASTER_LIST_REQ` from launchers + `MSG_MASTER_ANNOUNCE` from servers | yes |
| 22   | TCP | SSH admin | restrict to admin IPs |

Ports above are defaults (`DEFAULT_PORT`, `DEFAULT_MASTER_PORT`, `DISCOVERY_PORT`).
Staging rehearsal uses **17777 / 17800** so the live LAN box is never disturbed.

## 3. Staging VPS config

Host: `root@88.214.59.166` (xbuniverse box, key `~/.ssh/xbuniverse_vps`).
Binaries: `net/bin/reLCS-server`, `net/bin/reLCS-master`, `net/bin/relcs-netbot`
(Linux x64, built with `net/build-linux.sh`, plain glibc — no root needed at runtime).

Portability: the current build links `libstdc++.so.6` from the build toolchain
(`~/toolchain/lib` on the build VM). Check with `ldd` before deploying; if the
target's system libstdc++ predates gcc 13 (GLIBCXX_3.4.32), ship the toolchain's
`libstdc++.so.6` + `libgcc_s.so.1` next to the binaries and launch with
`LD_LIBRARY_PATH=/opt/relcs-staging`.

Layout on the VPS:

```
/opt/relcs-staging/
  reLCS-server  reLCS-master  relcs-netbot
  server.cfg                # see staging/server.cfg
  profiles.db               # created on first run
  server.log  master.log
```

Config files shipped in this folder:

- `staging/server.cfg` — server config (announce + master + rcon + no LAN discovery)
- `staging/reLCS-master.service`, `staging/reLCS-server.service` — systemd units
- `staging/firewall-staging.sh` — ufw rules (idempotent)

### Launch (nohup rehearsal form; use systemd units for anything long-lived)

```bash
cd /opt/relcs-staging
nohup ./reLCS-master -port 7800  >> master.log 2>&1 < /dev/null &
nohup ./reLCS-server -config server.cfg >> server.log 2>&1 < /dev/null &
```

## 4. Master announce

Server keys (`server.cfg`, or `-key value` on the CLI):

```ini
announce=1              # list on master (bool!)
master=127.0.0.1        # master host (same box -> loopback)
master_port=7800
```

Cadence and expiry:

- server connects to the master (ENet) and announces **every 30 s**
  (`Server::UpdateMasterAnnounce`, retry every 5 s while disconnected);
- master expires entries **90 s** after the last announce (3 missed announces
  drop the server from the list);
- the launcher list = `MSG_MASTER_LIST_REQ` -> `MSG_MASTER_LIST_RESP`
  (hostname, mode, players, gamePort, ipv4).

### How the master learns the server's IP (NAT-critical)

`master/main.cpp` keys each entry by the **observed source IP** of the announce
connection — that is NAT-safe: whatever public IP the announce exits from is what
clients get. One exception: if the observed source is **loopback** (server and
master on the same box), the master substitutes the server's advertised IP
(`LanIPv4()` = first RFC1918 IPv4, else first non-loopback IPv4).

Implications (rehearsed live — see `DRYRUN-REPORT.md`):

1. Same-box master + server: the announce source is loopback and the master
   substitutes `LanIPv4()`. That helper returns the **first RFC1918** address
   it finds — on boxes with docker/libvirt bridges (172.16/12) it can pick the
   bridge IP even though the interface holds the public IP. Observed on the
   staging VPS: the list advertised `172.17.0.1:7777` (docker0) instead of
   `88.214.59.166:7777`.
2. **Verified remedy for same-box deployments**: point the server at the box's
   own **public** address (`master=<public-ip>` in server.cfg). The announce
   then exits with the public IP as its observed source, no substitution runs,
   and the master records the correct endpoint — confirmed live: the entry
   flipped to `88.214.59.166:7777` and the internet list query passed.
3. Alternatives: run the master on a different host (observed IP then always
   wins), or patch in an `advertised_ip` config override.

Verify after every deploy (must show the **public** ip:gamePort):

```bash
grep '[master] announce' master.log     # announces are arriving
./relcs-netbot -master-query -server <master-host>:7800
# -> [MASTER] <public-ip>:<gamePort> "<hostname>" p/max mode=...
# -> [BOT] RESULT: PASS master_entries=N
```

(The master's `list` console command only works when the master runs on an
interactive terminal — under nohup/systemd its stdin is closed and the console
thread exits. The two commands above are the reliable checks.)

### CLI pitfall

`-announce 192.168.0.213:7800` does **not** set the master address: the CLI maps
every `-key value` through the config parser where `announce` is a **bool**
(`atoi("192.168.0.213:7800")` -> 192 -> true) and `master` silently keeps its
default `127.0.0.1`. It only worked on the LAN box because master and server
shared the host. Always pass flags explicitly:

```bash
./reLCS-server -announce 1 -master 127.0.0.1 -master_port 7800 -port 7777 ...
```

## 5. Firewall notes

ufw form (`staging/firewall-staging.sh`):

```bash
ufw allow 22/tcp              # SSH — restrict with 'from <admin-ip>' where possible
ufw allow 7777/udp            # game + rcon (in-band)
ufw allow 7800/udp            # master list queries + announce
# 7778/udp intentionally NOT open — LAN discovery is disabled in server.cfg
```

iptables form:

```bash
iptables -A INPUT -p udp --dport 7777 -j ACCEPT
iptables -A INPUT -p udp --dport 7800 -j ACCEPT
iptables -A INPUT -p tcp --dport 22  -j ACCEPT
```

Notes:

- **No TCP game ports.** Everything (chat, state, admin rcon) is in-band on
  UDP 7777; the rcon password in `server.cfg` is the only admin gate — keep it
  out of the repo and rotate between tests.
- Egress is unrestricted by default; if the VPS restricts egress and the master
  is remote, allow UDP out to `master_port`.
- `nf_conntrack` UDP timeouts: game state at 20 Hz keeps mappings alive; the
  master connection is also chatty (announce 30 s). If a firewall tracks UDP,
  ensure `nf_conntrack_udp_timeout` >= 60 s.
- Cloud security groups (Hetzner/OVH/AWS) are separate from ufw — open the same
  ports in the provider panel.
- The Linux firewall is root-only: `firewall-staging.sh` is applied by an admin,
  not by the run-book dry-run.
- **Some VPS images run raw iptables with `INPUT` policy DROP and a port
  allowlist** (ufw not installed). The staging box silently dropped 7777/7800
  from the internet until two ACCEPT rules were inserted — always check
  `iptables -L INPUT -n` before blaming the app when a query times out.

## 6. NAT traversal checklist

Server side (once):

- [ ] VPS has a public IPv4 **on an interface** (or run the master elsewhere —
      see §4). `ip -4 addr show` must show the public IP, not 10/192.168/172.16.
- [ ] UDP 7777 + 7800 reachable from the internet (test with
      `relcs-netbot -master-query` and a connect from outside).
- [ ] Not behind CGNAT (test: compare `ip -4 addr` with the WAN IP a browser
      reports; mismatch => CGNAT => inbound impossible without a tunnel).
- [ ] `lan_discover=0` on the VPS (broadcasts are pointless there).

Client side (per tester):

- [ ] Outbound UDP is all a client needs — **no port forwarding on home routers**.
- [ ] Client NAT type: any full-cone/restricted NAT works (client initiates);
      symmetric NAT is fine too since the client is the connection originator.
- [ ] Hairpin: a client on the *same LAN as the server* connecting via the
      master list gets the **public** IP and needs router hairpin/NAT-loopback.
      If hairpin is missing, connect directly: `-connect <lan-ip>:7777`.
- [ ] CGNAT home internet (mobile hotspots included) works fine for **clients**.
- [ ] Carrier/ISP UDP throttling: if movement is rubber-banding but LAN play is
      clean, retest with a phone hotspot to isolate the ISP path.
- [ ] MTU: enet reliable packets fragment across UDP; VPN/WireGuard tunnels with
      reduced MTU can drop fragments — keep VPS MTU at 1500 unless tunneling.
- [ ] IPv4 only: the net stack addresses nodes with 32-bit IPv4 (`in_addr`);
      IPv6-only clients/servers are not supported.

## 7. Internet test matrix (run against staging, then prod)

| # | Test | Pass condition |
|---|------|----------------|
| T1 | Master list query from an internet host | `relcs-netbot -master-query` prints `PASS master_entries>=1` and the entry shows the public IP |
| T2 | Direct connect from internet | client joins, server.log shows `[JOIN]`, client log shows `WELCOME` |
| T3 | Gameplay | movement, chat, props (`spawnprop`), NPC (`spawnbot`) work across the WAN |
| T4 | Admin rcon from internet | `give`, `broadcast` (banner on all clients), responses in panel console |
| T5 | Two clients behind different NATs | both see each other's puppets move (state sync 20 Hz) |
| T6 | Hairpin (LAN client via public IP) | works, or documented fallback `-connect lan-ip` |
| T7 | Soak > 10 min | no drop: announce cadence 30 s vs 90 s expiry keeps the listing alive |

## 8. Rollback

```bash
systemctl stop reLCS-server reLCS-master   # or pkill -f 'relcs-staging'
ufw delete allow 7777/udp; ufw delete allow 7800/udp   # if opened for the test
```

Launcher: point `master` in `launcher.ini` back to the previous address.
