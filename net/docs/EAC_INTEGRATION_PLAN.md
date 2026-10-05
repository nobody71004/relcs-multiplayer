# Easy Anti-Cheat (EAC) — Implementation Plan for reLCS Online

This maps a real EAC integration onto the existing reLCS multiplayer platform.
EAC today ships through **Epic Online Services (EOS) Anti-Cheat**, which is
client-hosted with a server relay — a good fit for our dedicated-server
architecture. The plan is layered on top of the server-side validator we
already ship (`src/net/rnet_validator.h`, `net/server/server.cpp`), not a
replacement for it.

---

## 1. Why both layers

| Layer | What it stops | Status today |
|---|---|---|
| **Server-side validator** (ours) | speed/teleport hacks, fake state, money editing, packet floods, chat spam — *after the fact*, server-authoritative | **shipped** (`rnet_validator.h`, 45 unit checks) |
| **EAC client integrity** | memory editing, injected DLLs, aimbots, wallhacks, debuggers — *at the source*, on the player's machine | **this plan** |

Server-side validation can never see a wallhack (the client legitimately knows
all streamed entity positions). EAC can. Conversely EAC can't validate game
economy/movement authority — our validator owns that. Both stay.

---

## 2. Integration architecture (EOS Anti-Cheat)

```
  player machine                                server machine
┌────────────────────────────┐                ┌──────────────────────────┐
│ reLCS.exe                  │                │ reLCS-server.exe         │
│  ├─ game code              │                │  ├─ game mode / world    │
│  ├─ cefui.dll (Chromium)   │                │  ├─ rnet_validator       │
│  └─ eosac-client.dll ◄─────┼── EAC SDK ────►│  └─ eosac-server module  │
│        ▲                   │   (kernel      │        ▲                │
│  EasyAntiCheat EOS service │    driver on   │  EOS Anti-Cheat         │
│  (local, via EOS SDK)      │    client)     │  dedicated-server API    │
└────────────────────────────┘                └──────────────────────────┘
        ▲                                                ▲
        │              EOS backend (Epic)                │
        └────────── registration / ticket / ban ─────────┘
```

Key properties of the EOS model (vs the legacy EAC SDK):
- **No hosted server required**: the game's own dedicated server relays
  anti-cheat messages (peer-to-server via our existing UDP channels).
- **Game server is authoritative**: EAC verdicts (client integrity) feed our
  validator's kick/ban pipeline.
- The kernel driver only exists on the client side (optional for us to require).

---

## 3. Work packages

### WP0 — Onboarding (account work, no code)
1. Register an Epic Games organization + product at
   <https://dev.epicgames.com/> (EOS Admin Portal).
2. Enable **Anti-Cheat** for the product; record `ProductId`,
   `SandboxId`, `DeploymentId`, `ClientId`, `ClientSecret`.
3. Generate an **EAC client module** credential (per-game anti-cheat key).
4. Accept the EAC service agreement (requires a shipping contact).
   - Nothing in this repo can proceed past WP1 without these IDs.

### WP1 — EOS SDK in the build
1. Vendor the EOS SDK (Windows x64) under `net/eos/` (same pattern as
   `net/cef/`): `SDK/` headers + `EOSSDK-Win64-Shipping.lib/dll`.
2. Root `premake5.lua`: add `eos-sdk` interface project (import lib only).
3. Compile-time gate: `RELCS_WITH_EOS` define; everything below compiles to
   no-ops without the SDK so the open-source build stays buildable.

### WP2 — Client integration (reLCS.exe)
1. New module `src/net/anticheat.cpp/.h` (mirrors `cefhud.cpp`'s dynamic-load
   style where practical):
   - `Anticheat_Init()` — EOS platform create + `EOS_AntiCheatClient_Init`.
   - `Anticheat_RegisterPeer(id, name)` / `Anticheat_UnregisterPeer`.
   - `Anticheat_Poll()` — pump `EOS_AntiCheatClient_PollMessages` each frame
     (hook: `NetGame_Frame`), emitting `MSG_AC_PAYLOAD` to the server.
   - `Anticheat_OnPeerMessage(from, data)` — inbound relayed messages.
2. Protocol: add `MSG_AC_PAYLOAD` (28) + `MSG_AC_VERDICT` (29) to
   `src/net/rnet_common.h` (v4 protocol bump) — opaque byte blobs, capped at
   512 bytes, rate-limited like chat.
3. Optional peer-to-peer mode is *not* used; everything routes through the
   server so the server can relay even when players are behind NAT.

### WP3 — Server integration (reLCS-server.exe)
1. `net/server/anticheat_server.cpp` — EOS Anti-Cheat **dedicated server**
   interface (`EOS_AntiCheatServer_*`):
   - `OnClientAuthReady` state per player (added to `ServerPlayer`).
   - Relay: `MSG_AC_PAYLOAD` from a client → `EOS_AntiCheatServer_Receive*
     MessageFromClient`; outgoing → `MSG_AC_PAYLOAD` to that client only.
   - `EOS_AntiCheatServer_PollStatus` → if a client fails integrity,
     `KickPlayer(id, "anticheat violation")` through the existing kick path.
2. New RCON commands: `ac status`, `ac kick <id> <reason>`, `ac exempt <id>`.
3. Telemetry: every EAC verdict is logged with the same `[anticheat]` prefix
   as the validator, so the run book and the agent pipeline see one stream.

### WP4 — Launcher integration
1. The CEF launcher (`net/launcher`) becomes the **EAC bootstrapper**:
   verify game binary hashes before `CreateProcess`, pass an
   `--ac-ticket` argument (short-lived EOS auth ticket).
2. `JoinServer` flow: obtain EOS auth session (device ID / exchange code),
   hand the ticket to the game via CLI; the game presents it to the server
   on `MSG_HELLO` (protocol v4 field `acTicket`).
3. Server validates tickets before slot allocation (rejects replayed/stale
   tickets with "anticheat ticket invalid").

### WP5 — Integrity policies (config)
`server.ini`:
```ini
anticheat = required | optional | off
anticheat_kick = 1            ; act on EAC verdicts
anticheat_ticket_ttl = 60     ; seconds
```
`anticheat=off` keeps LAN/LAN-party setups working without EOS.

### WP6 — Keybind/agent interop
- The dev console gains `ac status` (CEF + native).
- `reLCS-agentd` exposes `/v1/anticheat` (read-only status) so LLM agents can
  report on it; agents can run `ac kick` but not disable the system
  (no `ac off` over the API).

---

## 4. Code touchpoints (exact)

| File | Change |
|---|---|
| `src/net/rnet_common.h` | `MSG_AC_PAYLOAD=28`, `MSG_AC_VERDICT=29`, `acTicket` in `MsgHello`, `PROTOCOL_VERSION=4` |
| `src/net/rnet_protocol.h` | `WriteAcPayload/ReadAcPayload`, hello ticket codec |
| `src/net/anticheat.cpp` (new) | client EOS lifecycle, peer register, poll |
| `src/net/nethooks.cpp` | `Anticheat_Init` at connect, `Anticheat_Poll` in `NetGame_Frame`, verdict events |
| `net/server/server.cpp` | relay handlers, verdict → kick, ticket check in `HandleHello` |
| `net/server/anticheat_server.cpp` (new) | EOS server API wrapper |
| `net/launcher/main.cpp` | ticket acquisition in `JoinServer` |
| `net/agent/main.cpp` | `/v1/anticheat` status endpoint |
| `net/tests/main.cpp` | codec tests for the AC messages + ticket TTL logic |
| `net/e2e.sh` | phase 7: "anticheat relay" — two bots exchange AC payloads through the server, verdict kick is exercised |

---

## 5. Sequencing & risk

1. **WP0 blocks everything** (Epic account + legal). Start there.
2. WP1–WP3 are independent of the CEF/launcher work and can land as a
   protocol-v4 milestone; ship behind `anticheat=optional` first.
3. WP4 lands with the launcher release; WP5/WP6 are polish.
4. **Risks**
   - EAC requires a stable release channel and can gate on code signing —
     budget for a signing certificate.
   - EOS SDK is a large vendor blob (~200 MB with symbols) — same
     distribution mechanics as `net/cef` (not committed; downloaded/extracted
     by the build prep script).
   - False positives kick real players: keep `anticheat=optional` until the
     verdict pipeline has soak time; always log `[anticheat]` with detail.
   - The protocol bump (v3 → v4) needs the compatibility story we already
     have: mismatch → clean "protocol version mismatch" rejection.

## 6. Acceptance criteria

- [ ] Fresh client with a known injected DLL is flagged within one session
      and kicked through the standard kick path (visible in `[anticheat]` log).
- [ ] Clean client runs 30 min soak with zero verdicts (false-positive check).
- [ ] Two clients exchange AC payloads over the relay (< 1 KB/s overhead each).
- [ ] Stale/replayed ticket rejected at join.
- [ ] `anticheat=off` LAN session works with no EOS account.
- [ ] All existing e2e phases stay green with `anticheat=optional`.

Until EAC lands, the shipped **tough validator** (`rnet_validator.h`) is the
authoritative anti-cheat layer: movement budgets, payload sanity, flood/spam
limits, server-tracked economy, and violation escalation to kick.
