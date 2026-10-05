# reLCS Multiplayer — Architecture Map (SA-MP style)

Goal: SA-MP-style multiplayer for **Liberty City Stories on PC (reLCS)** — external launcher
with server browser → join → play, a dedicated backend server with a full console, complete
player/vehicle sync **tested end-to-end**, working both **on LAN and over the internet**.

---

## 0. Verdict up front

| Question | Answer |
|---|---|
| Is there an existing LCS online stack to reuse? | **No usable code.** The only real LCS multiplayer is the **official PSP ad-hoc Wi-Fi mode** (7 modes, ≤6 players) — binary-only, PSP, never in the PC/PS2 codebase. Reuse its **game-mode designs**, not code. |
| GTA Connected? | Wrong target: GTA III/VC/SA/IV+EFLC only — **no LCS support**, closed-source. Use as UX/design reference only. |
| SA-MP / open.mp / MTA? | GTA:SA only. open.mp (open source) and MTA:SA (GPL — *reference only, do not copy code*) are the best **architecture** references. |
| Recommendation | **Build the netcode into the reLCS source tree** (we own the whole engine — the decisive advantage over every closed-source mod), vendor **ENet** (MIT) as transport, model the product surface on SA-MP (launcher + browser + server console + RCON) and the gamemodes on the official PSP modes. |
| Legal | `_relcs-src` ships **no LICENSE** (re3/reLCS was pulled by Rockstar). Keep this fork **private**; never publish binaries or the netcode publicly. |

---

## 1. The "Liberty City regular online stack" — findings

### 1.1 Official LCS multiplayer (the only LCS-native online stack)
The PSP release had **wireless ad-hoc multiplayer, up to 6 players, 7 modes, 60+ character
models, power-ups** (Radar Invisibility, Mega Damage, Frenzy, Health Boost, Sticky Tires,
Instant Repair). Today people still play it online via **PPSSPP + ad-hoc tunneling**
(pro-adhoc server / XLink Kai / Radmin VPN). Key facts for us:

* The multiplayer code exists **only in the PSP binary** — reLCS (based on the PS2-era
  sources) contains **none of it**. Nothing to link or port directly.
* The **rules of the 7 modes are fully documented** (see §7) and map cleanly onto
  server-side gamemodes — this is the content we reuse.
* Emulated ad-hoc (PPSSPP) is not a stack we can attach to reLCS; it is an alternative
  product, not a dependency.

### 1.2 GTA Connected / Liberty Unleashed (the "Liberty City" mods)
* **GTA Connected** (gtaconnected.com): scriptable MP for III/VC/SA/IV+EFLC. No LCS.
  Client and server are **closed source** (only community *scripts* are public JS).
  → Not reusable as code. Reuse: launcher/server-list UX, server console/CVar model,
  "script-once" gamemode concept.
* **Liberty Unleashed**: GTA III only, closed source, dormant. Nothing to reuse.

### 1.3 GTA:SA multiplayer (design references)
* **SA-MP / open.mp**: product surface we're copying (launcher → server list → join;
  `server.cfg`; console + RCON; Pawn-style gamemode API). open.mp is open source and its
  server component/console design is worth reading.
* **MTA:SA**: GPL — look at its sync *architecture* (BitStream, interpolation, anti-cheat
  layering) but **do not copy code** (GPL contamination risk).

**Conclusion:** build it ourselves inside reLCS. There is no LCS online code to reuse —
only designs, and those designs are good.

---

## 2. Component overview

```
                    ┌────────────────────┐   HTTPS JSON    ┌───────────────┐
                    │  reLCS Launcher    │ ──────────────► │ Master server │
                    │  (browser/faves/   │                 │ (list+heartbeat│
                    │   nick/settings)   │                 │  +version chk) │
                    └─────────┬──────────┘                 └───────▲───────┘
                              │ launch reLCS.exe                    │ announce (UDP/HTTP)
                              │ -connect ip:port -nick X            │ every 30 s
                              ▼                                     │
┌──────────────┐  ENet/UDP  ┌──────────────────────────┐   ┌────────┴────────┐
│ reLCS client │ ◄─────────►│   reLCS-server.exe       │◄──┤  LAN clients    │
│ (game + net) │  ch0/1/2   │   (headless, console,    │   │ (UDP broadcast  │
└──────────────┘            │    gamemodes, RCON)      │   │  discovery)     │
       ▲                    └──────────────────────────┘   └─────────────────┘
       │  same binary works for LAN (direct IP / broadcast) and internet
       │  (master list + UPnP + NAT punch; relay fallback later)
```

Four deliverables:
1. **`reLCS-server.exe`** — dedicated headless server: entity pools, sync, gamemodes,
   full console + RCON + `server.cfg`.
2. **Client net layer inside `reLCS.exe`** — `#ifdef GTA_NETWORK` code driving/reading
   game entities, chat UI, MP menu.
3. **`reLCS-launcher.exe`** — server browser (Internet / LAN / Favorites / Direct),
   player name, launches the game with connect args.
4. **Master server** — tiny HTTP service (list/announce/heartbeat) + optional relay later.

Plus **`relcs-netbot`** — headless protocol client for automated E2E testing (§8).

---

## 3. Transport: ENet now, abstraction so we can swap later

| Option | License | Pros | Cons |
|---|---|---|---|
| **ENet** (choice) | MIT | tiny, vendored in an hour, reliable/unreliable + ordered channels, proven in games | no built-in NAT traversal/encryption |
| GameNetworkingSockets | BSD-3 | encryption, Steam relay/NAT traversal, message classes | heavy build, more moving parts |
| Raw UDP | — | full control | we'd rebuild reliability/fragmentation badly |

* Vendor as `vendor/enet` + a premake project; wrap behind a small `INetTransport`
  interface (connect/disconnect/send(channel, reliability)/poll/callbacks) so GNS can be
  dropped in later for crypto + relay.
* **Channels:** `ch0` reliable-ordered (RPCs, handshake, chat, gamemode),
  `ch1` unreliable-sequenced (state snapshots), `ch2` reliable-ordered low-priority
  (file/skin transfers later).
* **LAN and internet use the exact same code path** (UDP). Only discovery and NAT differ (§6).

## 4. Wire protocol

Bit-packed (`BitStream` — first unit-tested module). Envelope:

```
[ u8 msgType ][ u16 seq ][ payload … ]
```

**Message classes**

| Type | Ch | Direction | Payload sketch |
|---|---|---|---|
| `HELLO` | 0 | C→S | protocol version, name, game-version hash, password |
| `WELCOME` | 0 | S→C | assigned playerId, server tick rate, stream distance, weather/time |
| `PLAYER_JOIN/QUIT` | 0 | S→C | playerId, name, skin |
| `PLAYER_STATE` | 1 | C↔S | see §5 (quantized) |
| `VEHICLE_STATE` | 1 | C↔S | see §5 |
| `RPC_*` | 0 | both | chat, damage, death, respawn, enter/exit vehicle, pickup, gamemode events, world time/weather |
| `SERVER_VAR/RCON` | 0/2 | both | console traffic |
| `PING/PONG` | 1 | both | RTT + lag compensation bookkeeping |

Handshake: `HELLO → (banned?/wrong version?/full? → DISCONNECT+reason) → WELCOME →
PLAYER_JOIN broadcast → client requests spawn → gamemode decides spawn → `RPC_SPAWN`.

Versioning: `PROTOCOL_VERSION` constant; server rejects mismatched clients (launcher
pre-filters by version from the master list).

## 5. Player & vehicle sync (the core)

**Model: server-authoritative-lite** (SA-MP style). The client's physics stays authoritative
for its *own* ped/vehicle (we are not rewriting the game into a lockstep sim); the server
**validates** movement (speed/teleport/health checks) and owns all *events* (damage, death,
spawns, pickups, gamemode). This is the only model that fits GTA-class physics without a
rewrite, and it is what every successful GTA MP used.

**Player state (20 Hz, unreliable, on-ch1, only to players in AOI):**
* position: 3 × int16 centimetres relative to a per-player 32-bit origin cell (±3.2 km)
* orientation: heading + pitch, 16-bit each
* velocity: 3 × int16 (dm/s) for extrapolation
* anim: u16 animId + u8 blend + flags (crouch/aim/fire/jump/in-vehicle/sprint)
* vitals: health u8, armour u8, weapon u8 + ammo u16
* vehicleId u16 + seat u8 (0xFF = on foot)

**Vehicle state (20 Hz, unreliable):** position/quat/velocity (same quantization), throttle/
brake/steer u8, wheel/door/damage bits, siren/lights/horn flags, health u16.

**Consumption on remote entities:**
* Interpolation buffer **120 ms**; extrapolate ≤250 ms on loss, then freeze at last state.
* Remote peds: spawn `CPed`/`CPlayerPed`-style entities (`src/peds/Ped.cpp`,
  `src/peds/PlayerPed.cpp`) with a distinct `ePedType` (the enum already has
  `PLAYER2..PLAYER8` slots) and drive their position/anim **directly from net state**
  instead of `CPad`; skip `CPed::ProcessControl` AI for them.
* Remote vehicles: `src/vehicles/Vehicle.cpp` etc. — kinematic application of the last
  interpolated state; local player's vehicle keeps full physics (it is the sender).
* Local player: unchanged `src/core/Pad.cpp` input path; each tick the state is sampled
  and sent. Server runs validation:
  * max speed ≈ game's real max + 20% slack (per vehicle class),
  * teleport distance check per tick,
  * health/armour monotonicity (no client-side healing),
  * on violation: `RPC_CORRECTION` → client snaps to server position (rubber-band).
* AOI = streaming radius (default 200 m, server-configurable `stream_distance`); server
  only sends states of entities within (and interesting to) each client. Interior/level
  partitioning uses the existing zone/cull data (`src/core/World.cpp`, `Zones`).

**Events (reliable ch0):** chat, damage (server validates range/LOS-less "is attacker
nearby + weapon plausible"), death → `RPC_DEATH` → gamemode respawn logic, vehicle
enter/exit (server owns the seat map so two players can't grab one seat), pickups,
weather/time (`CClock`/`CWeather` forced from server every few seconds), gamemode events.

**What gets disabled in MP mode:** mission scripts (`CTheScripts`), pause menu (or
pause = local-only), auto-save, cheats; ambient `CPopulation` density is server-configured
(peds/cars slider) — ambient world stays client-local and purely cosmetic.

## 6. LAN **and** internet — same protocol, different discovery/NAT

**LAN (works with zero internet):**
* Server listens on `0.0.0.0:7777/udp` (configurable).
* **Broadcast discovery:** server answers `DISCOVER` broadcasts on port 7777; the
  launcher's LAN tab sweeps `255.255.255.255` (+ subnet broadcast) every 2 s.
* Direct `ip:port` box always available. No master server required at any point.

**Internet:**
* **Master server** (small HTTP JSON service): `announce` (server heartbeat every 30 s with
  hostname, players, maxplayers, version, gamemode, ping port), `list` (browser pulls),
  `remove` on shutdown. Launcher Internet tab = list + live player counts + latency probe.
* **NAT traversal, in order of attempt:**
  1. **UPnP** port mapping requested by the server (and by the launcher when hosting).
  2. **UDP hole punching:** server introduces two NAT-ed clients (both send keep-alives to
     each other's observed endpoints — ENet connection already keeps the mapping warm;
     keep-alive interval 5–10 s).
  3. **Fallback:** clear launcher message "Port 7777/UDP not reachable — forward the port
     or enable UPnP", plus optional **relay** later (GNS/Steam relay or our own VPS relay —
     the staging VPS can host a relay + master from day one).
* IPv4 first; dual-stack IPv6 later (ENet supports it).

## 7. Server: `reLCS-server.exe` + full console

Headless (no librw rendering; math/collision/entities only). Reuses engine types where they
make sense (entity pools, `CVector`, zone data) but **not** `CGame`/rendering.

**`server.cfg`** (SA-MP-shaped):
```
hostname = Liberty City Stories — Free Roam
port = 7777
maxplayers = 32
gamemode = survivor   # see modes below
announce = 1          # list on master server
lan_discover = 1
rcon_password = …
stream_distance = 200.0
tick_rate = 20
ambient_peds = 0.5
weather = 0 ; time = 12
```

**Console & RCON:**
* Interactive console on stdin/stdout (arrow-key history), same command set over **RCON**
  (passworded, ch0 reliable or plain TCP).
* Commands: `status`, `players`, `say`, `kick`, `ban`, `banip`, `unban`, `setr`/`getr`
  (runtime server vars), `gmx` (restart gamemode), `loadmode`, `exec <cfg>`, `uptime`,
  `version`, `netstats` (bytes/packets/in-loss per client), `quit`.
* Logging: `server_log.txt` with levels (join/leave, chat, admin actions, validation
  rejections) + console mirror.

**Gamemodes:** v1 = **built-in C++ gamemode classes** (`OnPlayerConnect/Spawn/Death/
Update`-style virtuals — the SA-MP event surface), starting with the official PSP modes
(§ below). A scripting layer (Squirrel/Lua) is a later milestone; the C++ API is designed
so scripts map 1:1 onto it.

**Official PSP LCS modes → server gamemodes** (reuse of the "regular LCS online stack"):
1. **Liberty City Survivor** — deathmatch / team deathmatch, kill limit or timer. *(first)*
2. **Protection Racket** — 2 gangs, defend vs destroy 4 limos, roles swap on round end.
3. **Get Stretch** — steal rival gang cars to your base, score limit/timer.
4. **The Hit List** — rotating "Mark", hunters kill fast; longest survival wins.
5. **Street Rage** — checkpoint racing, vehicle swaps, weapons allowed, respawn-in-car.
6. **The Wedding List** — steal wanted cars, deliver to shipping crates, cash by condition.
7. **Tanks for the Memories** — hold the Rhino tank for target time; top damager gets it next.

The PSP power-ups (Radar Invisibility, Mega Damage, Frenzy, Health Boost, Sticky Tires,
Instant Repair) become server-spawned pickups — `RPC_PICKUP` events.

## 8. Launcher (SA-MP style)

`reLCS-launcher.exe` — small Win32 app:
* **Tabs:** Internet (master list) | LAN (broadcast sweep) | Favorites | Direct connect.
* **Columns:** server name, players/max, ping, gamemode, version, map/region.
* Name + settings (nickname, preferred skin, stream rate cap, GPU/window settings pass-through).
* **Join** → `reLCS.exe -connect <ip:port> -nick <name> [-password x]` (new args parsed in
  `src/skel/win/win.cpp` / `src/core/main.cpp`); also an **in-game fallback**: Frontend
  "MULTIPLAYER → Direct Connect" screen (`src/core/Frontend.cpp`) so the game works even
  without the launcher (also how LAN guests join on a machine with no launcher).
* Launcher ⇄ game is one-way (spawn + args); the game reports "connection failed: reason"
  via its own UI, so no IPC is required in v1.

## 9. Client integration points (where the code goes)

| Concern | File(s) | Change |
|---|---|---|
| Frame pump | `src/core/main.cpp` | `CNetClient::Update()` each frame; MP mode gate around pause/demo |
| Command line | `src/skel/win/win.cpp`, `src/core/main.cpp` | `-connect`, `-nick`, `-password`, `-port` |
| Input | `src/core/Pad.cpp` | unchanged locally; remote entities bypass `CPad` |
| Remote ped | `src/peds/Ped.cpp`, `PlayerPed.cpp` | net-driven state application + anim control |
| Remote vehicle | `src/vehicles/Vehicle.cpp`, `Automobile.cpp`, `Bike.cpp`, `Boat.cpp` | kinematic state application, seat map |
| World/events | `src/core/World.cpp`, `src/control/` (events, scripts) | damage/death routing, disable missions |
| Time/weather | `src/core/Clock.cpp`, `Weather.cpp` | server-forced values |
| Streaming | `src/core/Streaming.cpp` | AOI alignment; stream-in/out hooks for net entities |
| Chat/MP UI | `src/core/Frontend.cpp` + new `src/net/` | chat overlay, scoreboard (TAB), connect/disconnect UI |
| Menu text | `src/text/` | new MP strings |
| Build | `premake5.lua` | `GTA_NETWORK` define, `src/net/*`, `vendor/enet`, new targets `reLCS-server`, `relcs-netbot`, launcher |

New module `src/net/` (client+shared): `NetClient`, `BitStream`, `MsgCodec`, `InterpBuffer`,
`NetEntities`, `Validation`, `Chat`. The **server and the bot share `MsgCodec` + `BitStream`
verbatim** — one protocol implementation, three consumers (client, server, test bot).

## 10. End-to-end test plan ("tested end-to-end properly")

1. **Unit tests** (run headless in CI): BitStream round-trip & bit-level drift, quantization
   error bounds (pos ≤ 2 cm, angle ≤ 0.006°), delta/codec round-trip, AOI culling truth
   table, server validation rules (speed/teleport/health cases), interpolation correctness
   (synthetic jittery streams → monotonic, ≤ 120 ms lag).
2. **`relcs-netbot` integration harness** (the workhorse): headless bots speaking the real
   protocol. Script: N bots join local server → spawn → deterministic random-walk paths →
   chat → enter/exit vehicles → shoot/damage → death/respawn → disconnect/reconnect.
   Assertions every second: all clients' interpolated views of entity X agree with server
   truth within tolerance; every join has a matching leave; zero unhandled messages;
   server memory/handles flat.
3. **Live 2-instance test** (manual + scripted): server + 2 windowed game instances on this
   PC (reuse the `input-test2.ps1` key-injection harness for both windows). Checklist:
   see each other run/wave/shoot, chat round-trip < 200 ms, passenger seat both ways,
   kill → respawn at gamemode spawn, reconnect mid-game.
4. **Network degradation:** `clumsy`/NEWTsim-equivalent proxy — 100 ms ± 30 ms jitter,
   2 % loss, 300 ms spikes → assert no visible desync > 0.5 m, no crash, clean
   reconnection after a 10 s drop.
5. **LAN path:** second machine (or VM) on the LAN, discovery + direct IP, play 10 min.
6. **Internet path:** server on the staging VPS (port 7777/udp + master), two remote
   clients behind consumer NATs → verify hole punch/UPnP and the port-forward fallback
   message.
7. **Soak:** 24 h bot run (32 bots), `gmx` every hour; watch RSS, handles, server log for
   validation noise; netstats bandwidth budget: **≤ 6 KB/s upstream per client @ 20 Hz**
   with full AOI.
8. **Acceptance:** all of 1–7 green on one run book before calling player sync "done".

## 11. Milestones

| # | Scope | Exit criteria |
|---|---|---|
| **M0** | ENet vendored, `src/net` skeleton, BitStream + unit tests, loopback ping tool | tests green, ping RTT printed |
| **M1** | `reLCS-server.exe` + console + `server.cfg` + handshake + chat + `status/players/kick` | 2 netbots chat through the server |
| **M2** | **Player sync**: state msgs, interpolation, remote ped rendering, spawn/respawn | 2-instance manual test: see each other move smoothly |
| **M3** | Vehicles: seat map, enter/exit, vehicle state sync, damage/death | drive together, passenger works, wreck + respawn works |
| **M4** | **Launcher + master + LAN discovery + UPnP/hole punch** | join from launcher on LAN *and* internet |
| **M5** | Gamemode: Liberty City Survivor (+ pickups/power-ups), time/weather sync | full 4-player deathmatch match end-to-end |
| **M6** | Hardening: validation/anti-cheat-lite, RCON, ban lists, netstats | cheat-ish inputs get corrected/kicked |
| **M7** | **E2E campaign** (§10): bots, degradation, LAN, internet, soak, perf | run book fully green; remaining PSP modes land as gamemodes |

## 12. Risks & mitigations

* **Legal** — no LICENSE in tree (re3 takedown). Keep everything private; no public repos,
  no binary distribution. The master server and any VPS stay under our control.
* **Physics desync** — mitigated by server-authoritative-lite + generous-but-not-infinite
  validation slack; interpolation buffer tunable per-server.
* **The known heavy-load crash** (`findMinVertAndNumVertices` bad index buffer during world
  render) will hit MP harder (more entities) — root-cause it before M2/M3 testing gets loud.
* **Engine coupling** — `CPed`/`CPad` are tightly woven; remote-entity driving is isolated
  in `src/net/NetEntities` + small hooks so single-player mode is bit-identical when
  `GTA_NETWORK` is off... and the net code compiles to nothing in SP builds.
* **Transport upgrade** — if encryption/relay becomes mandatory, swap ENet →
  GameNetworkingSockets behind `INetTransport` without touching protocol code.
