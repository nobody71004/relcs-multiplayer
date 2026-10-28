# relcs-loadgen — 1k / 2k player load tests

One process drives hundreds of ENet peers that behave like players: connect,
`HELLO`, spawn request, 20 Hz state, occasional chat, 5 s keepalive ping. It is
a *load* client (measure the server), not an assertion client — the netbot
covers protocol PASS/FAIL.

## Server side

`net/loadgen/loadtest.cfg` is the 2k profile (run the server with
`-config ../../loadgen/loadtest.cfg`, or copy it next to the binary):

```
maxplayers=2000        # clamped to rnet::MAX_SERVER_PLAYERS (2000, 2048 slots)
tick_rate=20
stream_distance=200
netstats_interval=1    # 1 Hz telemetry line, also written to log_file
log_file=server-load.log
eac=0                  # IMPORTANT: bots have no EOS identity
ac_require=0
```

`eac=0` is not optional. Anti-cheat defaults to on with `ac_require=1`, and
every unauthenticated bot is removed after `ac_grace` (30 s), so a run with
anti-cheat enabled measures the kick path instead of the server.

Start it:

```
cd net/bin/Release
./reLCS-server.exe -config ../../loadgen/loadtest.cfg
```

## Load side

A single process saturates long before 2000 peers (one event loop, one socket),
so shard the fleet: each process owns a slice of one placement grid via
`-grid-base` / `-grid-total`. Placement is a square grid of `-spread` metres
(default 1500) centred on the origin; bots relocate to their home with a
reliable, repeated `MOVEFLAG_TELEPORT` so the server accepts it as a warp.

1000 players, two shards:

```
cd net/bin/Release
for i in 0 1; do
  base=$((i*500))
  ( ./relcs-loadgen.exe -server 127.0.0.1:7900 -count 500 \
      -grid-base $base -grid-total 1000 -spread 1500 \
      -duration 50 -ramp 20 -name-prefix "S${i}_" -seed $((31+i)) \
      -csv load-1000-$i.csv > load-1000-$i.out 2>&1 ) &
done
wait
```

2000 players: same loop over `0 1 2 3`, `-grid-total 2000`, `-duration 55
-ramp 25`. Every process needs its own `-name-prefix`, `-seed` and `-csv`.

Useful flags:

| flag | meaning |
| --- | --- |
| `-count N` | peers in this process |
| `-spread M` | side of the placement square (metres) |
| `-cluster R` | put everyone inside radius `R` instead — worst-case AOI |
| `-ramp S` | spread connections over `S` seconds |
| `-state-hz H` | per-bot state rate (default 20) |
| `-grid-base/-grid-total` | shard placement across processes |
| `-decode 0\|1` | 0 (default) counts relayed states without decoding; 1 parses every state |
| `-chat-pct`, `-chat-sec` | share of bots that chat, and how often |
| `-csv PATH` | per-second CSV beside the `[load]` lines |

`-decode 0` matters: parsing ~1M relayed states/s costs more CPU than the server
spends producing them, and a saturated load host under-sends, which corrupts the
measurement. The server's egress is already visible in `relay_states/s`.

## Reading the server telemetry

One line per second in `server-load.log`:

```
netstats: players=1000/2000 spawned=1000 peers=1000 tick_ms avg=64.11 max=72.96 budget=50.0
  pump_ms 5.88/10.87 relay_ms 39.10/40.65 flush_ms 23.08/29.08
  relay_states/s=606448 relay_batches/s=24525 pkts_out/s=29859 pkts_in/s=24658
  in=1007.3KB/s out=25900.1KB/s rtt_avg=80ms rtt_max=97ms loss_max=1.3%
  joins=1000 leaves=0 kicks=0
```

* `tick_ms avg/max` vs `budget` (1000 / tick_rate) is the headline: over budget
  means the server is not holding its tick rate. `pump_ms`, `relay_ms` and
  `flush_ms` show which phase is responsible (averages/max within the window).
  `pump_ms` is inbound ENet servicing, `relay_ms` AOI batching, `flush_ms` the
  per-tick `enet_host_flush`.
* `relay_states/s` is states copied into batches, `relay_batches/s` packets
  built; `pkts_out/s`/`pkts_in/s` are what ENet actually moved.
* `rtt_avg/max` and `loss_max` come from ENet peer stats; client-side loss also
  shows up here as elevated RTT.
* `kicks` is per window. Any kick reason in the log is a real finding — but
  check the load generator first (see below).

`[teleport]`, `[KICK]`, `[JOIN]`/`[LEAVE]`, violations and refusals are logged
with millisecond timestamps, so a kick can be traced back to the exact tick.

## Wire format: relayed states (protocol v9)

`MSG_STATE_BATCH` no longer carries whole `PlayerState` structs (40 B each). A
relayed record is the subset a remote puppet consumes, delta-coded against the
receiver:

```
count u16, anchor x/y/z i32 (mm)
per record: id 12b | flags 4b | x/y/z i16 (cm from anchor) | heading 8b |
            speed 8b (0.2 m/s) | health 7b | [vehicleId 16b + seat 3b]
```

87 bits (~11 B) on foot, 106 with a seat, +48 when a record is more than
~327 m from the anchor and falls back to absolute mm. Pitch, anim ids,
weapon, ammo, wallet, armour and the velocity vector stay on
`MSG_PLAYER_STATE`; a puppet only used position, facing, speed magnitude,
health and vehicle occupancy. Both codecs are covered by the net-tests round
trip (including the far-position fallback and the teleport/seat flags).

## Measured (2026-10-28, bots on the same host, dense 1500 m placement)

Protocol v9, one 1.2 KB datagram per client per tick (records are ~11 B, so a
200 m AOI of ~110 bodies is ~1.2 KB — the batch splits on encoded size, not on
a fixed record count):

| host | fleet | welcomed / spawned | tick_ms (budget 50) | pump / relay / flush | RTT avg | loss | kicks |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Windows | 1000 | 1000 / 1000 | **18.7** mean (max 30.9) | 5.5 / 5.5 / 11.8 ms | 17 ms | 1.1 % | 0 |
| Windows | 2000 | 2000 / 2000 | **52.3** mean (max 103.3) | 11.1 / 17.7 / 29.5 ms | 38 ms | 9.2 % | 0 |
| Linux (WSL) | 2000 | 2000 / 2000 | **25.6** mean (max 49.1) | 4.3 / 12.9 / 8.6 ms | 8 ms | 1.0 % | 0 |

Against the v8 numbers this is 25.3 → 18.7 ms at 1k, 37.1 → 25.6 ms on Linux
at 2k and 104–121 → 52.3 ms on Windows at 2k. Linux relays 2.09 M states/s in
47 MB/s of egress (was ~90 MB/s) and built 63 k packets/s in total, of which
~40 k are the per-client state datagrams — 1.0 per client per tick, down from
~4. On Windows, `flush_ms` fell from 64–74 ms to 29.5 ms for the same reason:
`sendto` is still ~15 µs here, but a client now needs one call, not four.

The Windows 2k row still sits just over the 50 ms budget and its `loss_max`
is high because the load fleet shares this host with the server; the Linux row
is the deployment target (see `net/build-linux.sh`). Measure on the platform
you deploy on, and run the shards from another machine for a clean number.

### Fixed while measuring

* 1 Hz reconciliation was O(players²) — ~1,000,000 packets in one tick at 1000
  players, a measured 700 ms stall every second. It is now spread over
  `tick_rate` ticks and limited to a 2× stream radius.
* Load bots warped home on the unreliable channel. At 2k (~25 % loss) the flag
  was dropped and the next state looked like a 1.5 km teleport, so the (correct)
  anti-cheat removed 268 bots. Warps now go reliable and repeat for 8 states.
* `Server` was a stack local: at 2048 slots (~2 MB) it overflowed the 1 MB
  thread stack before the first log line. It is heap-allocated now.
* `BitWriter`/`BitReader` wrote and read one bit at a time with a `push_back`
  branch per bit: ~1.2 µs per `PlayerState`, i.e. ~190 ms of relay per tick at
  2 000 players. Both now work a byte-chunk at a time (same wire format); the
  3 477-check codec suite covers the round trip.
* The relay is tiered: players within 64 m are relayed every tick (20 Hz), the
  rest of the AOI every other tick (10 Hz). At 2 000 players in a 1.5 km square
  that is ~110 visible bodies per client, so the far tier halves both the state
  count and the packet count without touching combat-range fidelity.

### Where the remaining time goes (2k)

With v9 in place the split on Linux is relay ~13 ms, flush ~9 ms, pump ~4 ms —
inside budget. On Windows the same tick is ~52 ms because flush alone is
~30 ms: ENet sends one datagram per `enet_peer_send`, and this host charges
~15 µs per `sendto` for 2 000 peers, so one packet per client per tick is the
floor. Remaining levers, in order:

1. Send-side batching (`sendmmsg` on Linux) — ENet has no multi-datagram send
   API, so this means patching the send path. It is the only way to get below
   one syscall per client per tick on Windows.
2. Shared multicast per AOI cell for dense fights (one datagram to many peers).
3. Drop the far tier to 5 Hz for giant crowds (near ring stays 20 Hz).
4. Compress further (bit-pack the position to 2 cm steps, drop health from the
   far tier): ~1-2 bytes per record, marginal next to the syscall cost.
