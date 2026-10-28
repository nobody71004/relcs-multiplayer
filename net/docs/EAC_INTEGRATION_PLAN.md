# Easy Anti-Cheat (EAC) — Implementation Plan for reLCS Online

EAC ships through **Epic Online Services (EOS) Anti-Cheat**: a client-side module
(local kernel driver + local session) and a **dedicated-server session** that
receives the client's integrity traffic and returns verdicts. reLCS relays that
traffic over its own UDP protocol, so no hosted anti-cheat server is needed.

**Status: WP2 (client session), WP3 (server) and WP4 (launcher handoff) are
implemented.** The protected-clients-only policy is enforced by the server. What
remains is the dev-portal permission and the tampered-client acceptance run that
depends on it. This document records the design, what landed, and what is still
open.

| Layer | What it stops | Status |
|---|---|---|
| **Server-side validator** (`src/net/rnet_validator.h`) | speed/teleport hacks, fake state, money edits, packet floods, chat spam | **shipped** |
| **EAC bootstrapper** (`net/launcher/eac.cpp`) | tampered game files at launch, injected DLLs before start | **shipped** |
| **EOS Anti-Cheat server session** (`net/server/anticheat.cpp`) | *live* client tampering mid-session, via opaque driver traffic | **shipped** |
| **EOS Anti-Cheat client session** (game) | opens the local session that feeds the above | **shipped** (`src/net/acclient.cpp`) |

---

## 1. Why both layers

The validator can never see a wallhack (the client legitimately knows all
streamed entity positions) and EOS cannot adjudicate economy or movement
authority. Both stay: EOS supplies integrity, reLCS supplies authority.

```
 player machine                                server machine
┌────────────────────────────┐                ┌──────────────────────────────┐
│ reLCS.exe                  │                │ reLCS-server                 │
│  ├─ game code              │  MSG_AC_*      │  ├─ gamemode / world         │
│  ├─ cefui.dll (Chromium)   │◄──────────────►│  ├─ rnet_validator           │
│  └─ EOS_AntiCheatClient    │  (our UDP)     │  ├─ ac::Server               │
│        ▲                   │                │  │   EOS_AntiCheatServer      │
│  EasyAntiCheat_EOS service │                │  └─ profiles / resources     │
└────────────────────────────┘                └──────────────────────────────┘
            ▲                                                ▲
            └──────────── EOS backend (Epic) ────────────────┘
                    registration · tickets · verdicts
```

---

## 2. Implemented — server (WP3)

Files:

| File | Role |
|---|---|
| `net/server/anticheat.h` | `ac::Server` facade — no EOS headers, so the rest of the server stays SDK-free |
| `net/server/anticheat.cpp` | runtime SDK binding, platform/session lifecycle, registration, relay |
| `net/server/anticheat_config.h` | `ac::Config`, `eos.ini` + `Settings.json` readers, PUID validation (SDK-free, unit-tested) |
| `net/server/server.cpp` | message handlers, callbacks, deferred kicks, `ac` command |
| `src/net/rnet_common.h`, `rnet_protocol.h` | `MSG_AC_*` ids, structs and codecs |

### Lifecycle

```
EOS_Initialize
  → EOS_Platform_Create { bIsServer = EOS_TRUE, ProductId, SandboxId, DeploymentId,
                          ClientCredentials, absolute CacheDirectory }
  → EOS_Platform_GetAntiCheatServerInterface
  → AddNotifyMessageToClient / AddNotifyClientActionRequired / AddNotifyClientAuthStatusChanged
  → EOS_AntiCheatServer_BeginSession { RegisterTimeoutSeconds = 60, ServerName, bEnableGameplayData = false }
  ... EOS_Platform_Tick at 20 Hz from Server::Tick ...
  → EndSession → Platform_Release → EOS_Shutdown
```

The SDK is bound with `LoadLibrary` / `dlopen`, not linked. A host without the
library still serves players and reports `state=sdk-missing`; missing
credentials report `state=not-configured` (a dedicated server platform needs the
same client credential pair as the launcher).

### Registration

```
client                                server
  MSG_AC_HELLO {platform, PUID, JWT}  →
                                      EOS_Connect_VerifyIdToken
                                      EOS_AntiCheatServer_RegisterClient{ ClientHandle = ENetPeer*,
                                                                          ClientType = ProtectedClient,
                                                                          ClientPlatform, UserId, IpAddress }
  ←  MSG_AC_STATUS {AC_REG_OK | reason}
```

- The ProductUserId returned by token verification is authoritative; a client
  that presents somebody else's token is refused (`AC_REG_BAD_PUID`).
- `eac_require_token=0` registers without verification (LAN/lab only).
- Failure reasons: `AC_REG_DISABLED` 1, `BAD_PLATFORM` 2, `BAD_PUID` 3,
  `BAD_TOKEN` 4, `REJECTED` 5, `ALREADY` 6.

### Relay

| Direction | Path |
|---|---|
| client → EOS | `MSG_AC_FROM_CLIENT` → `ReceiveMessageFromClient` |
| EOS → client | `OnMessageToClient` → `MSG_AC_TO_CLIENT` |

Opaque bytes, capped at `MAX_AC_PAYLOAD` (1200 B) on both the wire and the SDK
call. Unregistered peers are ignored. Message ids 60–63 are additive, so
`PROTOCOL_VERSION` stays at 7 and older clients keep joining.

### Verdicts

`OnClientActionRequired` never re-enters the SDK: it marks the peer
(`acKick` + reason) and `Server::Tick` applies the kick after
`EOS_Platform_Tick` returns. `eac_kick=0` downgrades a verdict to a log line and
a chat warning. Kick and disconnect both `UnregisterClient`, so the EOS session
and the reLCS roster cannot drift.

### Configuration (`server.cfg`)

```
eac=1
eac_server_name=reLCS
eac_product_id=…        # default: EasyAntiCheat/Settings.json, else the shipped ids
eac_client_id=…         # default: eos.ini (searched next to the server)
eac_register_timeout=60
eac_require_token=1
eac_kick=1
eac_sdk_dir=…           # optional; RELCS_EOSSDK also works
```

### Administration

```
ac status      # eac=on state=running clients=0 pending=0 lib=…
ac list        # PUID, platform, IP, nickname, pending/registered
ac kick <id>
ac restart     # tear the session down and build it again
```

`status` also prints the AC line, and `setr ac_kick 0|1` toggles enforcement live.

### Verified

| Check | Result |
|---|---|
| Windows build (`net-server`, MSBuild x64 Release) | builds |
| `net-tests` (AC codecs, PUID, ini/JSON readers, `ac_*` config clamping) | 3440 checks, 0 failures |
| `reLCS.exe` with the AC client module (MSVC x64 Release) | builds, no EOS import in the PE |
| `ac_require=1 ac_grace=5`, live session, unprotected bot | removed after 5 s, bot reads `EOS anti-cheat required — this client is not protected` |
| `ac_failopen=0`, no live session | join refused with `this server requires EOS anti-cheat` |
| launcher `eac::Prepare` | copies `EOSSDK-Win64-Shipping.dll` (19.5 MB) + `eos.ini` into the game folder |
| Windows server, real product ids | `[EAC] anti-cheat session started`, `state=running` |
| Backend authentication | ⚠️ 403 `policy_missing_action` — client needs the anticheat permissions |
| Linux build (g++ 15, `anticheat.cpp` under POSIX branch) | builds, links |
| Linux server + `libEOSSDK-Linux-Shipping.so` | `[EAC] anti-cheat session started`, `state=running` |
| Host without the SDK | `state=sdk-missing`, server keeps serving |

---

## 3. What landed (WP2/WP3/WP4) and what is left

**WP2 — client session** (`src/net/acclient.h/.cpp`; §2 covers the server it talks to)

- SDK bound at runtime with `LoadLibrary` + `GetProcAddress`: `reLCS.exe` has no EOS import,
  so a game folder without the DLL still starts and simply plays unprotected.
- Connect device-id login inside the game (its own `eos-ac-cache/`), `CopyIdToken` on every
  registration attempt, `BeginSession(ClientServer)` with two retries on a cold service.
- `AddNotifyMessageToServer` → `MSG_AC_FROM_CLIENT`; `MSG_AC_TO_CLIENT` →
  `ReceiveMessageFromServer`; `AddNotifyClientIntegrityViolated` surfaced to the player.
- Driven from `NetClient::Pump`, so the game loop needed no new calls;
  `NetClient::Disconnect` calls `EndSession` before the next server opens its own
  (a session left open keeps the kernel driver attached and blocks a fresh
  `BeginSession`).
- The player sees the answer: `MSG_AC_STATUS` becomes a banner (`EOS ANTI-CHEAT:
  PROTECTED` / `NOT PROTECTED`) plus an `anti-cheat: <detail>` console line, and a
  `REFUSE_ANTICHEAT` disconnect explains what to install instead of dropping silently.

**WP4 — launcher handoff**: the game logs in for itself, so no secrets or tokens cross the
process boundary. What the launcher does hand over is the SDK and the credentials file: on
join `eac::Prepare` copies `EOSSDK-Win64-Shipping.dll` and `eos.ini` into the game folder
alongside the EAC runtime and bootstrapper.

**Policy — only protected clients**: `ac_require` (default 1) starts a `ac_grace`-second
deadline when a peer is admitted and removes it if it never registers; `ac_failopen`
(default 1) decides whether a server whose own session is down refuses joins up front
(`0`) or stays playable with a loud warning (`1`). `ac status` reports
`require/enforced/grace/failOpen/registered/rejected` and `SUSPENDED(server-side config)`
when EOS refuses every registration.

**Left to do**

0. **Portal**: the EOS client in `eos.ini` must hold
   `anticheat:authenticateForAnyUser` + `anticheat:authenticateForLocalUser`. Today the
   backend answers `403 policy_missing_action`; `BeginSession` still succeeds locally, so
   the server reports `state=running` with `lastError` set, and a protected client cannot
   finish registration. `net/eac/README.md` has the steps.
1. **Acceptance** (needs a real EOS client session, therefore blocked by 0):
   - tampered client flagged within one session and kicked through `ac`
   - clean client 30 min soak with zero verdicts
   - relay overhead < 1 KB/s per client
   - `eac=0` LAN session unaffected

Verified without the portal permission: the policy itself (an unprotected bot is removed
after the grace window with `EOS anti-cheat required — this client is not protected`),
the `ac_failopen=0` refusal path, the launcher's SDK/credentials install, and every path and
scanner case under §10.

---

## 4. Risks

- **False positives** kick real players: `eac_kick=0` first, watch `[EAC] action
  required` lines, then enforce.
- **Credentials**: the session needs the product's client id/secret in `eos.ini`.
  Without them the fleet runs `state=not-configured` (validator + bootstrapper only).
- **SDK distribution**: the Linux `.so` is deployed, not committed
  (`net/bin/` is ignored); see `net/eac/README.md`.
- **Kernel driver**: only the client needs it; a server host only needs the SDK.
