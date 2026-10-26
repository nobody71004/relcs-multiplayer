# package-release.ps1 — build the reLCS launcher release zip.
#
# The launcher statically imports EOSSDK-Win64-Shipping.dll and cefui.dll, plus the
# MSVC runtime. v0.3.0 shipped without the EOS SDK and died on other machines with
# "the code execution cannot proceed because EOSSDK-Win64-Shipping.dll was not found",
# so every dependency is now required here: a missing file fails the package instead of
# failing at a player's first launch.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File net/launcher/package-release.ps1 -Version 0.3.1
#   ... -Version 0.3.1 -BuildDir net/bin/Release -OutDir out
#
# The zip roots everything under reLCS-launcher/ so it can be unzipped over an existing
# launcher folder without touching launcher.ini, favorites.ini or usernames.ini.

[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$Version,
  [string]$BuildDir = "",
  [string]$EacDir   = "",
  [string]$OutDir   = "",
  [string]$RedistDir = "",
  [switch]$SkipZip
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path

if (-not $BuildDir) { $BuildDir = Join-Path $repo "net\bin\Release" }
if (-not $EacDir)   { $EacDir   = Join-Path $repo "net\eac" }
if (-not $OutDir)   { $OutDir   = Join-Path $repo "out" }

$staging = Join-Path $OutDir "package\reLCS-launcher"
$zipName = "reLCS-launcher-v$Version-win64.zip"
$zipPath = Join-Path $OutDir $zipName

Write-Host "== reLCS launcher package v$Version"
Write-Host "   build   : $BuildDir"
Write-Host "   eac     : $EacDir"
Write-Host "   staging : $staging"

# --- 1. required runtime dependencies -------------------------------------------
# name, description, extra search dirs
$required = @(
  @{ n = "reLCS-launcher.exe";        why = "the launcher itself" },
  @{ n = "launcher.html";             why = "CEF UI page" },
  @{ n = "cefui.dll";                 why = "CEF host the launcher links against" },
  @{ n = "EOSSDK-Win64-Shipping.dll"; why = "EOS bootstrapper (Epic Online Services)" }
)

# app-local MSVC runtime: without these a machine with no VC++ redist cannot start
# the launcher (MSVCP140.dll / VCRUNTIME140.dll / VCRUNTIME140_1.dll).
$redist = @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")

function Find-File([string]$name, [string[]]$dirs) {
  foreach ($d in $dirs) {
    $p = Join-Path $d $name
    if (Test-Path -LiteralPath $p) { return (Resolve-Path -LiteralPath $p).Path }
  }
  return $null
}

$redistSearch = @()
if ($RedistDir) { $redistSearch += $RedistDir }
# VCToolsRedistDir only exists inside a VS developer shell; fall back to System32,
# whose copies are the redistributable ones Windows installs.
if ($env:VCToolsRedistDir) {
  $redistSearch += (Join-Path $env:VCToolsRedistDir "x64\Microsoft.VC143.CRT")
  $redistSearch += (Join-Path $env:VCToolsRedistDir "x64\Microsoft.VC142.CRT")
}
$redistSearch += (Join-Path $env:WINDIR "System32")

$missing = @()
foreach ($r in $required) {
  $p = Find-File $r.n @($BuildDir)
  if (-not $p) { $missing += "$($r.n)  ($($r.why))" }
}
$redistFound = @{}
foreach ($n in $redist) {
  $p = Find-File $n $redistSearch
  if ($p) { $redistFound[$n] = $p } else { $missing += "$n  (MSVC runtime, pass -RedistDir)" }
}

if ($missing.Count -gt 0) {
  Write-Host ""
  Write-Host "ERROR: the package would be incomplete. Missing:" -ForegroundColor Red
  foreach ($m in $missing) { Write-Host "  - $m" -ForegroundColor Red }
  Write-Host ""
  Write-Host "Build the net solution first:"
  Write-Host "  msbuild net\build\reLCS-net.sln -p:Configuration=Release -p:Platform=x64 -t:net-launcher"
  exit 1
}

# --- 2. stage -------------------------------------------------------------------
if (Test-Path -LiteralPath (Join-Path $OutDir "package")) {
  Remove-Item -Recurse -Force -LiteralPath (Join-Path $OutDir "package")
}
New-Item -ItemType Directory -Force -Path $staging | Out-Null

function Copy-Into([string]$src, [string]$destRel) {
  $dest = Join-Path $staging $destRel
  $dir = Split-Path -Parent $dest
  if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
  Copy-Item -LiteralPath $src -Destination $dest -Force
}

Copy-Into (Find-File "reLCS-launcher.exe" @($BuildDir)) "reLCS-launcher.exe"
Copy-Into (Find-File "launcher.html" @($BuildDir)) "launcher.html"
Copy-Into (Find-File "cefui.dll" @($BuildDir)) "cefui.dll"
Copy-Into (Find-File "EOSSDK-Win64-Shipping.dll" @($BuildDir)) "EOSSDK-Win64-Shipping.dll"
foreach ($n in $redist) { Copy-Into $redistFound[$n] $n }

$eosExample = Join-Path $repo "net\launcher\eos.ini.example"
if (Test-Path -LiteralPath $eosExample) { Copy-Into $eosExample "eos.ini.example" }

# Easy Anti-Cheat payload: runtime + config + the PUBLIC certificate only.
$eacFiles = @(
  "integrity.cfg",
  "reLCS.exe.eac",
  "keys\base_public.cer",
  "runtime\start_protected_game.exe",
  "runtime\EasyAntiCheat\EasyAntiCheat_EOS_Setup.exe",
  "runtime\EasyAntiCheat\Settings.json",
  "runtime\EasyAntiCheat\SplashScreen.png"
)
foreach ($rel in $eacFiles) {
  $src = Join-Path $EacDir $rel
  if (-not (Test-Path -LiteralPath $src)) { Write-Host "WARN: EAC payload missing: $rel" -ForegroundColor Yellow; continue }
  Copy-Into $src ("eac\" + $rel)
}

$licWrite = Join-Path $EacDir "runtime\EasyAntiCheat\Licenses"
if (Test-Path -LiteralPath $licWrite) {
  Get-ChildItem -LiteralPath $licWrite -File | ForEach-Object {
    Copy-Into $_.FullName ("eac\runtime\EasyAntiCheat\Licenses\" + $_.Name)
  }
}

# --- 3. never ship key material or local state ----------------------------------
$forbidden = @("base_private.key", "launcher.ini", "favorites.ini", "usernames.ini", "eos.ini")
$leaks = Get-ChildItem -LiteralPath $staging -Recurse -File |
         Where-Object { $forbidden -contains $_.Name }
if ($leaks) {
  Write-Host "ERROR: refusing to package private/local files:" -ForegroundColor Red
  $leaks | ForEach-Object { Write-Host "  - $($_.FullName)" -ForegroundColor Red }
  exit 1
}

# --- 4. package notes -----------------------------------------------------------
$totalMb = [math]::Round(((Get-ChildItem -LiteralPath $staging -Recurse -File |
            Measure-Object -Property Length -Sum).Sum / 1MB), 1)
$notes = @"
reLCS launcher v$Version (win64)
===============================

Install
  1. Unzip this folder over your existing launcher folder. launcher.ini,
     favorites.ini and usernames.ini are not in the zip and are left alone.
  2. Start reLCS-launcher.exe, set the game folder in Settings if it is not detected.
  3. Join a server. First join opens the character creator.

Bundled runtime (all required)
  reLCS-launcher.exe            launcher / server browser
  launcher.html                 launcher UI
  cefui.dll                     CEF host the launcher links against
  EOSSDK-Win64-Shipping.dll     Epic Online Services SDK
  msvcp140.dll, vcruntime140.dll, vcruntime140_1.dll
                                MSVC runtime (no VC++ redist needed)
  eac\                          Easy Anti-Cheat payload: runtime, reLCS.exe.eac,
                                integrity.cfg, public certificate, licences

Provided by the game install (not in this zip)
  libcef.dll                    Chromium that cefui.dll loads (too large to bundle)
  chrome_elf.dll, chrome_100_percent.pak, chrome_200_percent.pak, resources.pak,
  icudtl.dat, v8_context_snapshot.bin, locales\
                                Chromium assets; without resources.pak and
                                v8_context_snapshot.bin CEF starts but its network
                                service crash-loops
  d3dcompiler_47.dll, dxcompiler.dll, dxil.dll, vk_swiftshader.dll, vulkan-1.dll
                                Chromium GPU helpers (fall back to software without them)
  cefsub.exe                    CEF subprocess helper
  reLCS.exe                     the game client (protected launch verifies it)

Admin / RCON
  Joining through the launcher passes the fleet RCON password to the game, so the
  in-game admin panel (F6) works on the public servers. To point at your own server,
  set rcon = <your server.cfg rcon_password> in launcher.ini.

Easy Anti-Cheat
  The signing key is never shipped. If you replace reLCS.exe or game data, re-sign
  the integrity catalogue so the protected launch accepts the folder:
    reLCS-launcher.exe -game "C:\path\reLCS\reLCS.exe" -eacintegrity

Staging size: $totalMb MB
"@
Set-Content -LiteralPath (Join-Path $staging "PACKAGE-README.txt") -Value $notes -Encoding UTF8

# --- 5. zip ---------------------------------------------------------------------
$files = Get-ChildItem -LiteralPath $staging -Recurse -File
Write-Host ""
Write-Host ("== staged {0} files, {1} MB" -f $files.Count, $totalMb)

if ($SkipZip) {
  Write-Host "== -SkipZip: staging only at $staging"
  exit 0
}

if (Test-Path -LiteralPath $zipPath) { Remove-Item -Force -LiteralPath $zipPath }
Compress-Archive -Path $staging -DestinationPath $zipPath -CompressionLevel Optimal

$zip = Get-Item -LiteralPath $zipPath
$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
Write-Host ("== {0}  ({1:N1} MB)" -f $zip.FullName, ($zip.Length / 1MB))
Write-Host ("   sha256 {0}" -f $hash)
Write-Host ""
Write-Host "Publish with:"
Write-Host "  gh release create v$Version `"$zipPath`" -R nobody71004/relcs-multiplayer --title `"...`" --notes-file <notes>`""
