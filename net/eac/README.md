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

## Server-side validation (next step, needs the same client credentials)

The client is protected by EAC today. Full EOS anti-cheat also wants the game
server to call `EOS_AntiCheatServer_BeginSession` and register each client with
the Connect product-user id, relaying the `OnMessageToServer` traffic over the
reLCS protocol. That work needs the Client ID/Secret plus the Linux EOS SDK from
the same product zip; it is not wired into `net/server` yet.
