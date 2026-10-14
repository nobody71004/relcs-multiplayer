# reLCS
[![Build Status](https://img.shields.io/endpoint.svg?url=https%3A%2F%2Factions-badge.atrox.dev%2FGTAmodding%2Fre3%2Fbadge%3Fref%3Dlcs&style=flat)](https://actions-badge.atrox.dev/GTAmodding/re3/goto?ref=lcs)
<a href="https://discord.gg/RFNbjsUMGg"><img src="https://img.shields.io/badge/discord-join-7289DA.svg?logo=discord&longCache=true&style=flat" /></a>

## Intro

The aim of this project is to reverse GTA Liberty City Stories.

## How can I try it?

- reLCS requires game assets to work.
- Build reLCS or download it from one of the above links (Debug or Release).
- (Optional) If you want to use optional features, copy the files in /gamefiles folder to your game root folder.
- Move reLCS.exe to GTA LCS directory and run it.

## Preparing the environment for building

You may want to point GTA_LCS_RE_DIR environment variable to GTA LCS root folder if you want executable to be moved there via post-build script.

- For Linux, proceed: [Building on Linux](https://github.com/GTAmodding/re3/wiki/Building-on-Linux)
- For FreeBSD, proceed: [Building on FreeBSD](https://github.com/GTAmodding/re3/wiki/Building-on-FreeBSD) 
- For Windows, assuming you have Visual Studio:
    - Clone the repo using the argument `--recursive`.
    - Run one of the `premake-vsXXXX.cmd` variants on root folder.
    - Open the project via Visual Studio  
    
**If you use 64-bit D3D9**: We don't ship 64-bit Dx9 SDK. You need to download it from Microsoft if you don't have it(although it should come pre-installed after some Windows version)  

There are various settings at the very bottom of [config.h](https://github.com/GTAmodding/re3/tree/lcs/src/core/config.h), you may want to take a look there. i.e. FIX_BUGS define fixes the bugs we've come across.

> :information_source: **If you choose OpenAL on Windows** You must read [Running OpenAL build on Windows](https://github.com/GTAmodding/re3/wiki/Running-OpenAL-build-on-Windows).

> :information_source: **Did you notice librw?** reLCS uses completely homebrew RenderWare-replacement rendering engine; [librw](https://github.com/aap/librw/). librw comes as submodule of reLCS, but you also can use LIBRW enviorenment variable to specify path to your own librw.

## Contributing
Please read the [Coding Style](https://github.com/GTAmodding/re3/blob/master/CODING_STYLE.md) Document


## Multiplayer (net/) — SA-MP-style platform

This fork adds a full multiplayer stack on top of the port:

| Component | Path | What it is |
|---|---|---|
| Launcher (CEF GUI) | `net/launcher/` | Chromium/CEF launcher: server browser (master list + per-username favorites), settings, join flow, in-game UI host |
| Dedicated server | `net/server/` | 20 Hz authoritative server: movement anticheat, ped/vehicle puppets, gamemodes, resource packs, static vehicle spawns, in-band RCON, SQLite player profiles |
| Master server | `net/master/` | server list (30 s announce, 90 s expiry) + favorites in SQLite (`master.db`), so a player's list follows the nickname across machines |
| Bot / test client | `net/bot/` | scriptable scenario bot (roam/combat/drive/chaos/fight/solo/cruise/watch), `-master-query`, `-rcon`/`-cmd` |
| Agent | `net/agent/` | tracing/telemetry harness |
| Game hooks | `src/net/` | nethooks (HUD/chat/console/admin, ped + vehicle puppets), netclient, CEF HUD glue |
| Tests | `net/tests/` | 3287-check unit suite (protocol codecs, validator, profile store, resources) |
| E2E run book | `net/e2e.sh` | 21-check end-to-end script (Windows: game client, discovery, master, net-shaper) |
| E2E server + bots | `net/e2e-bots.sh` | portable (Linux / Git Bash) server + bot phases incl. late-join, lost-warp and stream-in vehicle sync |
| Internet run book | `net/internet-test/` | NAT/VPS deployment run-book, staging configs, systemd units, dry-run report |
| Proxmox deploy | `net/deploy/proxmox/` | start/stop scripts for a 5-instance, 32-player LAN host |

### Server features

- **Sync** — 20 Hz state and vehicle sync within `stream_distance`, with the
  movement/vehicle antispam running on the server.
- **Anticheat** — server-side movement validator (speed/teleport budgets). Players
  authenticated over RCON are exempt, because admin tools such as noclip
  legitimately exceed those budgets.
- **Gamemodes** — `survivor` and `freeroam`.
- **Resource packs** — a folder under `resources/` with a `resource.cfg` manifest
  (name, version, author, description, type, autoload, motd) plus `*.rsc` scripts.
  Scripts declare `@on start | join | leave | interval <seconds> | cmd <name>`
  hooks and run ordinary server commands; `%id%`, `%name%` and `%args%` are
  expanded. `type=external` packs are sandboxed to a command whitelist,
  `type=internal` packs get full command access. Console: `res list`, `res reload`.
- **Static vehicles** — `vehicles.cfg`, one spawn per line:
  `<model> <x> <y> <z> [heading]`. Console: `spawncar`, `cars`, `delcar`.
- **Player profiles** — SQLite (`profiles.db`; give every instance its own file):
  money and weapon inventory, saved on disconnect and every 10 s while dirty.
- **Admin** — in-band RCON on the game's own UDP port (`rcon_password`), plus a
  server console: `say`, `broadcast`, `kick`, `ban`/`unban`, `give`, `spawnbot`,
  `spawnprop`, `settime`, `setweather`, `gmx`, `netstats`, `status`.

### Server config (`server.cfg`, or `-key value` on the CLI)

| Key | Default | Notes |
|---|---|---|
| `hostname` | reLCS Server | shown in the launcher list |
| `port` | 7777 | game traffic and rcon, UDP |
| `maxplayers` | 32 | |
| `gamemode` | survivor | `survivor` \| `freeroam` |
| `announce` | false | **bool** — list this server on the master |
| `master`, `master_port` | 127.0.0.1, 7800 | master to announce to |
| `lan_discover` | true | raw-UDP broadcast on 7778; disable on a VPS |
| `rcon_password` | — | empty disables admin |
| `stream_distance` | 250 | state sync radius |
| `tick_rate` | 20 | |
| `hour`, `minute`, `weather` | 12, 0, 0 | world clock |
| `netstats_interval` | 0 | >0 logs bandwidth/RTT every N seconds |
| `profiles` | profiles.db | SQLite path, empty disables |
| `resources` | resources | pack directory, empty disables |
| `vehicles` | vehicles.cfg | static spawn list, empty disables |

`announce` is a bool, so `-announce 192.168.0.213:7800` silently leaves `master`
at `127.0.0.1`; pass `-announce 1 -master <host> -master_port <port>` instead.

### Building the net stack

```
cd net
../premake5.exe vs2019       # enet, net-tests, net-server, net-master, net-bot, net-launcher, net-agent
```

Outputs land in `net/bin/Release/`. The game hooks build together with `reLCS.exe`.
The generated projects pin the VS2019 `v142` toolset — on a machine with only a
newer toolchain, build with `-p:PlatformToolset=v143`.

On Linux the server, master, bot and unit tests need no game assets:

```
cd net && ./build-linux.sh   # -> net/bin/{reLCS-server,reLCS-master,relcs-netbot,relcs-net-tests}
```

The script links the C++ runtime statically (`-static-libstdc++ -static-libgcc`),
so binaries built on a newer toolchain still start on an older distro (Ubuntu
24.04 ships libstdc++ 3.4.32 while a g++ 15 build otherwise needs 3.4.34).

### Deploying a server host

- **LAN host / Proxmox Ubuntu VM** — `net/deploy/proxmox/start-servers.sh` brings
  up the master plus five 32-player instances (UDP 7777, 7781-7784) with pid
  files, per-instance configs and profiles databases; `stop-servers.sh` shuts them
  down again. Layout, port map, verification commands and the `@reboot` cron
  entry: `net/deploy/proxmox/README.md`.
- **Internet / behind NAT** — `net/internet-test/RUNBOOK.md` documents the port
  map, master announce and IP-substitution rules, firewall and NAT checklist, and
  an internet test matrix; `DRYRUN-REPORT.md` records what was verified against a
  real VPS (including the docker-bridge advertised-IP trap).

### Releases

The GitHub Releases page ships ready-to-run zips (no build needed):

- **`relcs-launcher-win64.zip`** — launcher + full CEF runtime + `launcher.html`. Configure `launcher.ini` (game path, nickname, master, RCON password) and run `reLCS-launcher.exe`.
- **`relcs-server-tools-win64.zip`** — `reLCS-server.exe`, `reLCS-master.exe`, `relcs-netbot.exe`, `reLCS-agentd.exe` for hosting.

Server quick start: `reLCS-server.exe -rcon_password <pw> -lan_discover 1 -announce 1`

Player profiles (money + weapon inventory) persist across reconnects in a SQLite
database (`profiles.db` by default; set with `-profiles <path>`, empty disables).
Profiles are keyed by player name and saved on disconnect + every 10 s when dirty.
The bot can run admin commands with `-rcon <pw> -cmd "<command>"` (repeatable).
Launcher CLI: `-join ip:port -nick X`, `-query "<raw UI query>"`, `-selftest`.
Game CLI: `-connect ip:port -nick X -rcon <pw> -fps N`.
Favorites are owned by the master (per nickname, `master.db`) — `favorites.ini`
next to the launcher is only a local cache; `addfav`/`delfav`/`favorites` go
through the master protocol.

RCON-authenticated players are exempt from the movement anticheat (admin tools such
as noclip legitimately exceed movement budgets). All other players get the full
tough validator. See `patches/librw-geoplg-readmesh-guard.patch` for a required
librw crash-guard fix (apply inside `vendor/librw`) — it bounds the geometry mesh
index/material reads, which otherwise turn malformed or truncated mod files into
out-of-bounds reads that crash much later in the d3d9 renderer.

Modding notes — BMW i8 model, door chime override, console commands:
`docs/bmw-i8-integration.md`.
Protocol, validator and EAC planning docs: `net/docs/`.

### License

This project is licensed under the GNU General Public License v3.0 — see `LICENSE`.
Vendored dependencies keep their own terms: `vendor/enet` (BSD-3-Clause),
`vendor/sqlite` (public domain).
