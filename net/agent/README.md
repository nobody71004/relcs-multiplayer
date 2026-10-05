# reLCS Agent Control Pipeline

Drive the Liberty City Stories multiplayer platform with real-world LLM agent
models (or curl, or the web HUD) — from chat all the way down to live game state.

```
+--------------------+   OpenAI tool-calling   +--------------------------+
|  LLM (any vendor)  | --------------------- > |  agent_harness.py        |
|  gpt / claude / .. | <--------------------- |  (tool loop, --mock too) |
+--------------------+                         +-----------+--------------+
                                                           | HTTP/JSON
                                                           v
                                                 +--------------------------+
                                                 |  reLCS-agentd  :7801     |
                                                 |  (control bridge + HUD)  |
                                                 +-----------+--------------+
                                                     ENet  |        | spawn
                                                            v        v
                                            +----------------+   +----------------+
                                            | reLCS-server   |   | relcs-netbot   |
                                            | RCON, sync,    |   | NPC bot players|
                                            | gamemodes      |   +----------------+
                                            +----------------+
                                                     |
                                              game clients (reLCS.exe)
```

## Quick start

```sh
# 1. server
reLCS-server.exe -port 7777 -rcon secret -gamemode freeroam

# 2. bridge (serves the web HUD + JSON API)
reLCS-agentd.exe -server 127.0.0.1:7777 -rcon secret -http 7801

# 3. web HUD (CEF-style page — also embeddable in any CEF/overlay shell)
start http://127.0.0.1:7801/

# 4. mock agent (pipeline self-test, used by e2e)
python agent_harness.py --mock --base http://127.0.0.1:7801 --rcon secret

# 5. real LLM agent (any OpenAI-compatible endpoint)
OPENAI_API_BASE=https://api.openai.com/v1 OPENAI_API_KEY=sk-... OPENAI_MODEL=gpt-4o-mini \
  python agent_harness.py --base http://127.0.0.1:7801 --rcon secret \
  --goal "welcome everyone, then spawn a roaming bot named Scar and give the first player godmode"
```

## HTTP API (agentd)

| Method | Path        | Body / params                              | Effect |
|--------|-------------|--------------------------------------------|--------|
| GET    | `/`         | —                                          | Web HUD (server HUD + inventory + admin panel) |
| GET    | `/v1/tools` | —                                          | OpenAI function-calling schema (5 tools) |
| GET    | `/v1/status`| —                                          | Live state: `status`, `inventory` (money/hp/weapons per player), `netstats` |
| POST   | `/v1/rcon`  | `{ "command": "teleport 2 1133 -624 24" }` | Any server console command |
| POST   | `/v1/say`   | `{ "text": "..." }`                        | Broadcast to all players |
| POST   | `/v1/give`  | `{ "target": "id\|name", "kind": "money\|weapon\|god\|health", "value": N, "arg": ammo }` | Admin effects |
| POST   | `/v1/bot`   | `{ "name": "...", "scenario": "roam\|combat\|drive\|chaos", "duration": N }` | Spawn an NPC bot player |

Auth: `X-Rcon: <password>` header (or `"password"` in the JSON body) on all
`/v1/*` calls. `/v1/tools` is public so agents can self-describe.

## Tools given to the LLM

- `server_status` — read players, inventories, netstats
- `server_rcon` — raw console access (`teleport`, `spawnprop`, `loadmode`, `setr`, …)
- `server_say` — announcements
- `server_give` — money, weapons, godmode, health
- `server_spawn_bot` — NPC bot players with nametags and scripted scenarios

`GET /v1/tools` returns exactly these in OpenAI format — plug the response
straight into a `tools:` request field of any tool-calling chat API.

## Server console commands (agent + admin)

`status`, `inventory <id|name>`, `broadcast <text>`, `give <id|name>
money|weapon|god|health <value> [ammo]`, `spawnbot <name> [scenario]`,
`spawnprop <x> <y> <z> [weaponType] [qty]`, `teleport <id> <x> <y> <z>`,
`respawn <id>`, `kick`, `loadmode <freeroam|survivor>`, `gmx`, `setr/getr`,
`netstats`, `exec <file>`, `version`, `uptime`.

## In-game surfaces

- **F6** — server admin panel (broadcast, noclip, godmode, money, weapons,
  spawn NPC bot, spawn prop, FPS caps) via `-rcon <pw>` on the client
- **I** — inventory menu (money, health, armour, weapon slots + ammo)
- **T** — chat
- Nametags float above every remote player/bot; a welcome splash greets you
  on world entry.

## Security notes

- The rcon password is the bearer credential for the whole pipeline; run the
  bridge on loopback or behind a TLS reverse proxy.
- `server_rcon` is powerful (it *is* the server console) — gate tool access in
  the harness if you expose a model to untrusted goals.
