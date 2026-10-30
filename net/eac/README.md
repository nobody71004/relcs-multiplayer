# Easy Anti-Cheat + Epic Online Services

The launcher installs and drives this payload; the game itself needs no code
change to be protected. Everything here is product data for
`fb155afe4a754a4b91929491873c844e` (sandbox `515e78d9d395488faf0a3f63041dbba0`,
deployment `a287ae738e5241829bdcd1d8a0d3b0bb`).

## Layout

| Path | What it is |
| --- | --- |
| `runtime/` | Easy Anti-Cheat redistributable from the EOS SDK (`SDK/Tools/EOS_AntiCheatTools-win32-x64-2.3.0.zip`) |
| `runtime/start_protected_game.exe` | EAC bootstrapper copied next to the game executable |
| `runtime/EasyAntiCheat/` | service installer, splash screen, localisation |
| `reLCS.exe.eac` | EAC module CDN config downloaded from the dev portal |
| `devtools/anticheat_integritytool.exe` | signs the integrity catalogue |
| `keys/base_public.cer` | product certificate (public) |
| `keys/base_private.key` | **signing key — git-ignored, keep it out of the repo** |
| `integrity.cfg` | what gets hashed (game data, binaries; not logs/saves/config) |

## What the launcher does

1. `Prepare()` copies the runtime into the game directory, writes
   `EasyAntiCheat/Settings.json` (title/executable/product ids) and
   `<exe>.eac`.
2. `InstallService()` runs `EasyAntiCheat_EOS_Setup.exe install <productId>`,
   registering the `EasyAntiCheat_EOS` service.
3. `BuildIntegrity()` runs the integrity tool with the product keys and writes
   `EasyAntiCheat/Certificates/base.bin`, `base.cer` and `runtime.conf`.
4. Join starts `start_protected_game.exe`, which loads the EAC module and then
   launches the real `reLCS.exe`.

Do steps 2–3 again after replacing `reLCS.exe` or any game data: the catalogue
hashes those files, and a mismatch makes EAC refuse the launch. The launcher
re-runs `Prepare()` on every join, but signing is explicit because it is slow.

```powershell
# maintenance, no GUI
reLCS-launcher.exe -game "C:\Games\reLCS\reLCS.exe" -eacinstall
reLCS-launcher.exe -game "C:\Games\reLCS\reLCS.exe" -eacintegrity
reLCS-launcher.exe -eostest        # EOS bootstrap result in launcher.log
```

## Dev portal: what still needs a click

EOS SDK 1.19 refuses `EOS_Platform_Create` without a client credential pair, so
the launcher bootstrap currently stops at `client-credentials-missing`.

1. https://dev.epicgames.com/portal → sign in → **Your Products** → the reLCS
   product (`fb155afe…`).
2. **Clients** → **Create Client** (type: *Client*, not *Service*).
   Give it the product's default permissions, save.
3. Copy the **Client ID** and **Client Secret** into `net/launcher/eos.ini`
   (next to `reLCS-launcher.exe` in a shipped build):
   ```ini
   client_id=...
   client_secret=...
   ```
   `eos.ini` is git-ignored; `eos.ini.example` documents the format.
4. **Anti-Cheat** page: confirm the EAC integration is **Active**, and download
   the *product* zip again if you rotate the base keys. The portal also hosts the
   `.eac` bootstrap config (`<executable>.eac`) used above.

With the credentials in place the launcher runs: SDK init → platform → device
ID → Connect login, and the Settings page shows each step live
(`eos-bootstrap.log` has the same trail).

## Server side — implemented

`net/server/anticheat.cpp` opens a **dedicated-server anti-cheat session** at boot:
`EOS_Initialize` → `EOS_Platform_Create(bIsServer)` →
`EOS_AntiCheatServer_BeginSession` (60 s register window by default), pumps
`EOS_Platform_Tick` at 20 Hz, verifies each peer's Connect ID token, registers the
peer with the *ENet peer pointer* as the EOS client handle, and relays the driver
traffic in both directions over `MSG_AC_FROM_CLIENT` / `MSG_AC_TO_CLIENT`
(≤ 1200 B per message). A verdict from EOS becomes a normal reLCS kick.

### What the server host needs

| Piece | Where |
|---|---|
| SDK library | Windows: `EOSSDK-Win64-Shipping.dll` beside `reLCS-server.exe`. Linux: `libEOSSDK-Linux-Shipping.so` beside `reLCS-server` (or set `eac_sdk_dir` / `RELCS_EOSSDK`). From the EOS SDK `SDK/Bin` for the matching platform. |
| Credentials | `eos.ini` (`client_id`, `client_secret`) in the server's working directory — the same file the launcher uses. `eac_client_id` / `eac_client_secret` in `server.cfg` override it. |
| Product data | Built into the binary **and** re-read from `EasyAntiCheat/Settings.json` when that file sits next to the server (`eac_settings` points elsewhere). |

The library and `eos.ini` are **not** committed (`net/bin/` is ignored); a host
that lacks either still serves players, logging `state=sdk-missing` or
`state=not-configured` — `ac status` shows which.

### Operating it

```
ac status                 # eac=on state=running clients=0 pending=0 lib=…
ac list                   # PUID, platform, IP, nickname
ac kick <id>              # remove a player and drop their AC slot
ac restart                # rebuild the session after rotating eos.ini
setr ac_kick 0            # log verdicts without kicking (soak mode)
```

A protected player shows up in `ac list` a few seconds after joining: `reLCS.exe`
opens its own `EOS_AntiCheatClient` session (`src/net/acclient.cpp`), registers
over `MSG_AC_HELLO` and relays the driver traffic. The launcher puts
`EOSSDK-Win64-Shipping.dll` and `eos.ini` into the game folder on join; without
them the client logs `[ac] sdk-missing` in `eos-ac.log` and plays unprotected.

### Protected clients only

`ac_require = 0` is the default. A protected peer still registers with the
session, but enforcement is opt-in because of the identity requirement below.
`ac_require = 1` removes a peer that has not registered inside `ac_grace`
seconds; `ac_failopen = 1` (default) keeps the server playable when its own
session cannot start. Set `ac_failopen = 0` for the strict deployment: with no
live session the server then refuses joins up front.

```
ac status                 # require=0 enforced=0 grace=30s failOpen=1 registered=1 rejected=0
```

### Stable identity (why enforcement defaults off)

Epic refuses EOS **device-ID** users for anti-cheat:

```
errors.com.epicgames.eos.eac.deviceid_not_supported
    DeviceID is not supported for anti-cheat since it requires a stable account identity
```

The shipped game logs in with a device ID, so its AC client registers, the
backend then asks the server to remove it (`action=1 reason=2`, InvalidMessage).
That is an identity limitation, not a cheat verdict: the server logs
`could not be authenticated by EOS — keeping the player`, unregisters the AC
slot and keeps the connection in the game. A real anti-cheat verdict
(`reason` 6/7, ClientViolation / BackendViolation) still kicks when
`eac_kick=1`.

To enable `ac_require=1`, clients must authenticate with an Epic Account Portal
(or Steam and similar) identity before `EOS_Connect_Login`, and pass that ID
token to the AC client. Until then, keep enforcement off so players are not
removed 6 seconds after joining.

`enforced=0` with `SUSPENDED(server-side config)` means EOS refused every
registration (the 403 below) and the policy turned itself off instead of
emptying the server — fix the permissions and run `ac restart`.

### Dev portal: grant the anti-cheat permissions

`BeginSession` only creates the local session; the backend then authenticates it
with the client credential, and a client without the anti-cheat permissions is
refused:

```
403 errors.com.epicgames.common.policy_missing_action
    Client <clientId> missing anticheat:authenticateForAnyUser,anticheat:authenticateForLocalUser
```

`ac status` surfaces that line as `lastError="…ServiceName=[AntiCheat]…"`, so a
fleet host that starts but never authenticates is easy to spot. Fix: EOS portal →
product → **Clients** → the client used in `eos.ini` → grant the anti-cheat
permissions (and confirm **Anti-Cheat** is active for the deployment), then
`ac restart` (or restart the server).
