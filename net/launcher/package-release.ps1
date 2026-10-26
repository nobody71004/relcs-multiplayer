# package-release.ps1 — build the reLCS launcher release zip.
#
# The package must start on a machine that has nothing installed: no game folder, no
# VC++ redist, no previous launcher. That means every runtime piece the launcher loads
# has to be in the zip:
#
#   reLCS-launcher.exe            imports cefui.dll + EOSSDK-Win64-Shipping.dll
#   cefui.dll                     the CEF host (windowless browser)
#   libcef.dll, chrome_elf.dll, chrome_*.pak, resources.pak, icudtl.dat,
#   v8_context_snapshot.bin, locales\, vk_swiftshader*, d3dcompiler_47, dxcompiler,
#   dxil                          Chromium runtime cefui.dll loads from its own folder
#   cefsub.exe                    CEF browser subprocess (dead without it)
#   EOSSDK-Win64-Shipping.dll     Epic Online Services SDK
#   msvcp140, vcruntime140, vcruntime140_1
#                                 MSVC runtime, so no redist install is needed
#   eac\                          Easy Anti-Cheat payload (public parts only)
#
# v0.3.0 shipped the exe alone and died with "EOSSDK-Win64-Shipping.dll was not found";
# v0.3.1 added the EOS SDK but not the Chromium runtime, so the launcher got as far as
# loading cefui.dll and then failed on libcef.dll. Both are required here, and the script
# refuses to produce a zip when any of them is missing.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File net/launcher/package-release.ps1 -Version 0.3.2
#   ... -Version 0.3.2 -BuildDir net/bin/Release -Locale en-US,en-GB -SkipZip
#
# Everything is rooted under reLCS-launcher/ so the zip unzips over an existing launcher
# folder without touching launcher.ini, favorites.ini, usernames.ini or eos.ini.

[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$Version,
  [string]$BuildDir = "",
  [string]$EacDir   = "",
  [string]$OutDir   = "",
  [string]$RedistDir = "",
  [string[]]$Locale = @("en-US", "en-GB"),
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

# --- 1. required files ----------------------------------------------------------
# Every entry is something the launcher or its CEF host loads at startup.
$required = @(
  @{ n = "reLCS-launcher.exe";        why = "the launcher" },
  @{ n = "launcher.html";             why = "launcher UI" },
  @{ n = "cefui.dll";                 why = "CEF host (static import)" },
  @{ n = "libcef.dll";                why = "Chromium (imported by cefui.dll)" },
  @{ n = "cefsub.exe";                why = "CEF browser subprocess" },
  @{ n = "chrome_elf.dll";            why = "Chromium crash handler" },
  @{ n = "chrome_100_percent.pak";    why = "Chromium resources" },
  @{ n = "chrome_200_percent.pak";    why = "Chromium resources (HiDPI)" },
  @{ n = "resources.pak";             why = "Chromium resources" },
  @{ n = "icudtl.dat";                why = "ICU data" },
  @{ n = "v8_context_snapshot.bin";   why = "V8 snapshot" },
  @{ n = "d3dcompiler_47.dll";        why = "Chromium ANGLE/D3D shader compiler" },
  @{ n = "dxcompiler.dll";            why = "Chromium D3D12 shader compiler" },
  @{ n = "dxil.dll";                  why = "Chromium D3D12 shader signing" },
  @{ n = "vk_swiftshader.dll";        why = "Chromium software Vulkan fallback" },
  @{ n = "vk_swiftshader_icd.json";   why = "swiftshader registration" },
  @{ n = "vulkan-1.dll";              why = "Chromium Vulkan loader" },
  @{ n = "EOSSDK-Win64-Shipping.dll"; why = "EOS SDK (static import)" }
)
foreach ($l in $Locale) {
  $required += @{ n = "locales\$l.pak"; why = "CEF locale pack ($l)" }
}

$redist = @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")

function Find-File([string]$name, [string[]]$dirs) {
  foreach ($d in $dirs) {
    if (-not $d) { continue }
    $p = Join-Path $d $name
    if (Test-Path -LiteralPath $p) { return (Resolve-Path -LiteralPath $p).Path }
  }
  return $null
}

# VCToolsRedistDir only exists inside a VS developer shell; fall back to System32,
# whose copies are the redistributable ones Windows installs.
$redistSearch = @()
if ($RedistDir) { $redistSearch += $RedistDir }
if ($env:VCToolsRedistDir) {
  $redistSearch += (Join-Path $env:VCToolsRedistDir "x64\Microsoft.VC143.CRT")
  $redistSearch += (Join-Path $env:VCToolsRedistDir "x64\Microsoft.VC142.CRT")
}
$redistSearch += (Join-Path $env:WINDIR "System32")

$missing = @()
$sources = @{}
foreach ($r in $required) {
  $p = Find-File $r.n @($BuildDir)
  if ($p) { $sources[$r.n] = $p } else { $missing += "$($r.n)  ($($r.why))" }
}
$redistFound = @{}
foreach ($n in $redist) {
  $p = Find-File $n $redistSearch
  if ($p) { $redistFound[$n] = $p } else { $missing += "$n  (MSVC runtime, pass -RedistDir)" }
}

if ($missing.Count -gt 0) {
  Write-Host ""
  Write-Host "ERROR: the package would not start on a clean machine. Missing:" -ForegroundColor Red
  foreach ($m in $missing) { Write-Host "  - $m" -ForegroundColor Red }
  Write-Host ""
  Write-Host "Build the net solution first (it copies the CEF runtime next to the launcher):"
  Write-Host "  msbuild net\build\reLCS-net.sln -p:Configuration=Release -p:Platform=x64 -t:net-launcher"
  exit 1
}

# --- 2. stage -------------------------------------------------------------------
$pkgRoot = Join-Path $OutDir "package"
if (Test-Path -LiteralPath $pkgRoot) { Remove-Item -Recurse -Force -LiteralPath $pkgRoot }
New-Item -ItemType Directory -Force -Path $staging | Out-Null

function Copy-Into([string]$src, [string]$destRel) {
  $dest = Join-Path $staging $destRel
  $dir = Split-Path -Parent $dest
  if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
  Copy-Item -LiteralPath $src -Destination $dest -Force
}

foreach ($rel in $sources.Keys) { Copy-Into $sources[$rel] $rel }
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
$licDir = Join-Path $EacDir "runtime\EasyAntiCheat\Licenses"
if (Test-Path -LiteralPath $licDir) {
  Get-ChildItem -LiteralPath $licDir -File | ForEach-Object {
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
$files = Get-ChildItem -LiteralPath $staging -Recurse -File
$totalMb = [math]::Round(($files | Measure-Object -Property Length -Sum).Sum / 1MB, 1)
$localeList = ($Locale -join ", ")
$notes = @"
reLCS launcher v$Version (win64)
===============================

This package is self-contained: it starts on a machine with no game folder, no VC++
redistributable and no earlier launcher install.

Install
  1. Unzip this folder anywhere and run reLCS-launcher.exe.
  2. Point Settings at your game folder (reLCS.exe) before joining.
  3. Joining passes the fleet RCON password to the game, so the in-game admin panel
     (F6) works out of the box. Your own server: rcon = <server.cfg rcon_password>
     in launcher.ini.

Bundled runtime (all required to start)
  reLCS-launcher.exe            launcher / server browser
  launcher.html                 launcher UI
  cefui.dll, cefsub.exe         CEF host + browser subprocess
  libcef.dll, chrome_elf.dll, chrome_100_percent.pak, chrome_200_percent.pak,
  resources.pak, icudtl.dat, v8_context_snapshot.bin, locales\$localeList.pak
                                full Chromium runtime
  d3dcompiler_47.dll, dxcompiler.dll, dxil.dll, vk_swiftshader.dll,
  vk_swiftshader_icd.json, vulkan-1.dll
                                Chromium GPU helpers + software Vulkan fallback
  EOSSDK-Win64-Shipping.dll     Epic Online Services SDK
  msvcp140.dll, vcruntime140.dll, vcruntime140_1.dll
                                MSVC runtime (no redist needed)
  eac\                          Easy Anti-Cheat payload: runtime, reLCS.exe.eac,
                                integrity.cfg, public certificate, licences

Still provided by the game install
  reLCS.exe + game data         the game itself; protected launch verifies it
  eos.ini (optional)           EOS product client id/secret for Connect login

Easy Anti-Cheat
  The signing key is never shipped. If you replace reLCS.exe or game data, re-sign the
  integrity catalogue so the protected launch accepts the folder:
    reLCS-launcher.exe -game "C:\path\reLCS\reLCS.exe" -eacintegrity

Staging size: $totalMb MB in $($files.Count) files
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
