# Internet-test run-book — dry-run report

Date: 2026-10-06. Run-book: `RUNBOOK.md`. Staging config: `staging/`.
Both stages completed; the staging VPS was rolled back to its prior state.

## Stage 1 — rehearsal on the LAN stand-in (`xerox@192.168.0.213`, ports 17777/17800)

| # | Run-book step | Result |
|---|---------------|--------|
| 1 | Deploy binaries + `staging/server.cfg` | PASS |
| 2 | Start master + server (nohup form) | PASS — `[master] listening on UDP 17800`, server on UDP 17777 |
| 3 | Master announce cycle (30 s cadence) | PASS — `[master] announce reLCS Staging (0/32) mode=survivor` |
| 4 | Observed/advertised-IP rule | PASS — loopback source substituted `LanIPv4()` -> entry `192.168.0.213:17777` |
| 5 | Master list query (`relcs-netbot -master-query`) | PASS — `master_entries=1` |
| 6 | Game-port connect + state/chat | PASS — `WELCOME id=1 host=reLCS Staging`, `JUMP`/`IAM` sync, CHAT relay |
| 7 | Teardown | PASS — live services untouched |

Findings folded into `RUNBOOK.md`: the `-announce <addr>` CLI pitfall (bool key,
address silently ignored), the `libstdc++` toolchain dependency (`ldd`), and the
`pkill -f` self-match trap (use `pkill -xf` or bracket patterns).

## Stage 2 — real staging VPS (`root@88.214.59.166`, `Ubuntu2404Temp-EN`)

Access: `free-buff.pri` (RSA) — the earlier fleet keys are stale on all known
hosts and were abandoned after 11 rejected attempts.

| # | Step | Result |
|---|------|--------|
| 1 | Recon: Ubuntu 24.04.1, `88.214.59.166/24` on eth0, 7777/7800 free, ufw absent | PASS |
| 2 | Deploy `reLCS-server`/`reLCS-master`/`relcs-netbot` + `staging/server.cfg` to `/opt/relcs-staging` (two-hop scp via Windows) | PASS — `ldd` resolves against system libstdc++ |
| 3 | Start master (UDP 7800) + server (UDP 7777); announce registered | PASS |
| 4 | **T1** master list query **from the internet** (Windows -> 88.214.59.166:7800) | **PASS** — `master_entries=1` (after two fixes below) |
| 5 | **T2** game connect **from the internet** (Windows -> :7777) | **PASS** — `WELCOME id=1 host=reLCS Staging`, spawn + live `IAM` state sync |
| 6 | **T4** in-band rcon over the WAN | **PASS** — server log: `[RCON] auth OK from id=1`, `[RCON] id=1: status` |
| 7 | Rollback (run-book §8) | PASS — processes stopped, both iptables rules deleted; `/opt/relcs-staging` kept intact so the 3-command re-enable below works |

### Findings (folded into RUNBOOK §4/§5)

1. **Host firewall, not the app**: `iptables` `INPUT` policy **DROP** with a port
   allowlist (no ufw) silently ate the internet queries — tcpdump showed zero
   inbound packets. Two `ACCEPT` rules for 7777/udp + 7800/udp fixed T1/T2.
   (Rules are not persistent on that box; re-apply per §5 for a live test.)
2. **docker0 hijacks the advertised IP**: with `master=127.0.0.1` (same-box), the
   master substituted `LanIPv4()` and listed **`172.17.0.1:7777`** — the docker
   bridge — instead of the public IP, even though eth0 holds `88.214.59.166`.
   Internet clients would follow an unreachable address.
3. **Verified remedy**: `master=88.214.59.166` (the box's own public address) —
   the announce then has a non-loopback observed source, substitution is
   skipped, and the list flipped to `88.214.59.166:7777`. T1 passed after this.

## Test matrix status (RUNBOOK §7)

| Test | Status |
|------|--------|
| T1 master query from internet | PASS (after firewall + master-address fixes) |
| T2 direct connect from internet | PASS |
| T3 gameplay across WAN (props/NPC/chat) | partial — chat + state sync PASS via bot; needs a real game client for props/NPC feel |
| T4 admin rcon from internet | PASS (auth + command in-band) |
| T5 two clients behind different NATs | not run — needs two external testers |
| T6 hairpin (LAN client via public IP) | not run — needs a client on the server's LAN |
| T7 soak > 10 min | not run — announce cadence (30 s vs 90 s expiry) validated in stage 1 only |

## Verdict

The run-book works against a real staging VPS over the internet: master query,
game connect, and in-band rcon all pass from an external host once the two
firewall/config findings above are applied. The staging box was rolled back
cleanly; re-enabling the staging instance for live testers is three commands:

```bash
ssh -i free-buff.pri root@88.214.59.166
iptables -I INPUT -p udp --dport 7777 -j ACCEPT; iptables -I INPUT -p udp --dport 7800 -j ACCEPT
cd /opt/relcs-staging
nohup ./reLCS-master -port 7800 >> master.log 2>&1 < /dev/null &
nohup ./reLCS-server -config server.cfg >> server.log 2>&1 < /dev/null &
```

(`server.cfg` already carries `master=88.214.59.166`, the verified remedy.)
