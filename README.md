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
| Launcher (CEF GUI) | `net/launcher/` | Chromium/CEF launcher: server browser, settings, join flow, in-game UI host |
| Dedicated server | `net/server/` | 20 Hz authoritative server: movement anticheat, vehicle sync, gamemodes, RCON, SQLite player profiles |
| Master server | `net/master/` | LAN/internet server listing |
| Bot / test client | `net/bot/` | Scriptable scenario bot (drive/roam/combat) for E2E tests |
| Agent | `net/agent/` | Tracing/telemetry harness |
| Game hooks | `src/net/` | nethooks (HUD/chat/console/admin), netclient, CEF HUD glue |
| Tests | `net/tests/` | 3177-check unit suite (protocol codecs, validator, profile store) |
| E2E run book | `net/e2e.sh` | 21-check end-to-end script |

### Building the net stack

```
cd net
../premake5.exe vs2019
# open build/reLCS-net.sln, targets: enet net-tests net-server net-bot net-launcher net-agent
```

Outputs land in `net/bin/Release/`. The game hooks build together with `reLCS.exe`.

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

RCON-authenticated players are exempt from the movement anticheat (admin tools such
as noclip legitimately exceed movement budgets). All other players get the full
tough validator. See `patches/librw-geoplg-readmesh-guard.patch` for a required
librw crash-guard fix (apply inside `vendor/librw`).

Protocol, validator and EAC planning docs: `net/docs/`.

### License

This project is licensed under the GNU General Public License v3.0 — see `LICENSE`.
Vendored dependencies keep their own terms: `vendor/enet` (BSD-3-Clause),
`vendor/sqlite` (public domain).
