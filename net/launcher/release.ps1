# release.ps1 - one command for a shippable reLCS release.
#
#   powershell -ExecutionPolicy Bypass -File net/launcher/release.ps1 -Version 0.5.3
#
# Order matters, and this script exists so it cannot be done out of order:
#
#   1. build reLCS.exe        (Release, win-amd64-librw_gl3_glfw-oal)
#   2. build reLCS-launcher.exe (net solution, Release x64)
#   3. copy the client into the game folder
#   4. re-sign the EAC integrity catalogue for that client and copy it into
#      net/eac/runtime/EasyAntiCheat/Certificates (the payload that gets zipped)
#   5. package out/reLCS-launcher-v<Version>-win64.zip
#   6. run the packaged launcher's -selftest (CEF, LAN, master, favorites,
#      game detection) and refuse to report success unless it says PASS
#   7. copy out/reLCS-client-v<Version>-win64.exe
#   8. with -Publish, create the GitHub release for the tag, once 5-7 passed
#      and HEAD is pushed (a notes file is required); -Draft keeps it
#      unpublished, so the publish path can be rehearsed without going live
#
# The selftest needs the master and a LAN server to answer, so -SkipSelftest
# exists for an offline build host; a release cut with it has not been smoke
# tested and should be run once by hand before publishing.
#
# The catalogue hashes reLCS.exe. Signing before the client copy signs the old
# binary, and packaging before signing ships a stale catalogue - either way
# start_protected_game.exe aborts with "210, Unrecognized game client".
#
# The game folder is taken from -GameDir, or from "game =" in launcher.ini at the
# repo root. Its reLCS.exe is replaced by this build, which is what the signing
# tool hashes; the rest of the game data is left alone.

[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [ValidatePattern('^\d+\.\d+\.\d+$')]
  [string]$Version,

  [string]$GameDir = "",

  [switch]$SkipClientBuild,
  [switch]$SkipLauncherBuild,
  [switch]$SkipSelftest,

  # Publish is opt-in: it creates a public GitHub release. Never implied by the
  # build. It is refused with -SkipSelftest, because publishing a package that
  # has not been smoke tested defeats the point of the gate.
  [switch]$Publish,
  [switch]$Draft,
  [string]$NotesFile = "",
  [string]$Title = ""
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path

function Fail([string]$message) {
  Write-Host ""
  Write-Host "ERROR: $message" -ForegroundColor Red
  exit 1
}

Write-Host "== reLCS release v$Version"
Write-Host "   repo : $repo"

$repoSlug = "nobody71004/relcs-multiplayer"
if ($Draft -and -not $Publish) { Fail "-Draft only applies to -Publish" }
if ($Publish) {
  if ($SkipSelftest) {
    Fail "-Publish cannot be combined with -SkipSelftest: refusing to publish a package that was not smoke tested"
  }
  if (-not $NotesFile) { $NotesFile = Join-Path $repo "_scratch\release-notes-$Version.md" }
  if (-not (Test-Path -LiteralPath $NotesFile)) {
    Fail "release notes not found: $NotesFile (write them, or pass -NotesFile)"
  }
  $NotesFile = (Resolve-Path -LiteralPath $NotesFile).Path
}
if (-not $Title) { $Title = "reLCS launcher v$Version" }

# --- 1. toolchain ---------------------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vs = ""
if (Test-Path -LiteralPath $vswhere) {
  # vcvars64.bat shells out to vswhere; put it on PATH so the build is quiet.
  $env:PATH = "$(Split-Path -Parent $vswhere);$env:PATH"
  $vs = (& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath) |
        Select-Object -First 1
}
if (-not $vs) {
  $guesses = @(
    "C:\Program Files\Microsoft Visual Studio\18\Enterprise",
    "C:\Program Files\Microsoft Visual Studio\18\Community",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise",
    "C:\Program Files\Microsoft Visual Studio\2022\Community"
  )
  foreach ($g in $guesses) {
    if (Test-Path -LiteralPath (Join-Path $g "MSBuild\Current\Bin\MSBuild.exe")) { $vs = $g; break }
  }
}
if (-not $vs) { Fail "Visual Studio with MSBuild was not found. Install the C++ desktop workload or run from a developer shell." }

$msbuild = Join-Path $vs "MSBuild\Current\Bin\MSBuild.exe"
$vcvars  = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $msbuild)) { Fail "MSBuild not found under $vs" }
if (-not (Test-Path -LiteralPath $vcvars))  { Fail "vcvars64.bat not found under $vs" }

function Invoke-Build([string]$solution, [string]$target, [string]$platform) {
  if (-not (Test-Path -LiteralPath $solution)) { Fail "solution not found: $solution" }
  Write-Host ""
  Write-Host "== build $target ($platform)"
  $line = "call `"$vcvars`" >nul && `"$msbuild`" `"$solution`" /t:$target /p:Configuration=Release /p:Platform=$platform /m /v:m"
  & cmd.exe /c $line
  if ($LASTEXITCODE -ne 0) { Fail "build failed: $target ($platform), exit $LASTEXITCODE" }
}

$clientExe = Join-Path $repo "bin\win-amd64-librw_gl3_glfw-oal\Release\reLCS.exe"
$launcherExe = Join-Path $repo "net\bin\Release\reLCS-launcher.exe"

if ($SkipClientBuild) {
  Write-Host "== build reLCS (skipped)"
} else {
  Invoke-Build (Join-Path $repo "build\reLCS.sln") "reLCS" "win-amd64-librw_gl3_glfw-oal"
}
if (-not (Test-Path -LiteralPath $clientExe)) { Fail "client build produced no $clientExe" }

if ($SkipLauncherBuild) {
  Write-Host "== build launcher (skipped)"
} else {
  Invoke-Build (Join-Path $repo "net\build\reLCS-net.sln") "net-launcher" "x64"
}
if (-not (Test-Path -LiteralPath $launcherExe)) { Fail "launcher build produced no $launcherExe" }

# --- 2. game folder -------------------------------------------------------------
$game = $GameDir
if (-not $game) {
  $ini = Join-Path $repo "launcher.ini"
  if (Test-Path -LiteralPath $ini) {
    $hit = Select-String -LiteralPath $ini -Pattern '^\s*game\s*=\s*(.+?)\s*$' | Select-Object -First 1
    if ($hit) { $game = $hit.Matches[0].Groups[1].Value.Trim('"', ' ') }
  }
}
if (-not $game) { Fail "no game folder: pass -GameDir or set 'game =' in launcher.ini" }
if ($game -like "*.exe") { $game = Split-Path -Parent $game }
if (-not (Test-Path -LiteralPath $game)) { Fail "game folder does not exist: $game" }
$game = (Resolve-Path -LiteralPath $game).Path
Write-Host "   game : $game"

if (Get-Process -Name "reLCS" -ErrorAction SilentlyContinue) {
  Fail "reLCS.exe is running; close the game before releasing (the client copy would fail)"
}

$gameExe = Join-Path $game "reLCS.exe"
Write-Host ""
Write-Host "== stage client into the game folder"
Copy-Item -LiteralPath $clientExe -Destination $gameExe -Force
$srcHash = (Get-FileHash -LiteralPath $clientExe -Algorithm SHA256).Hash
$dstHash = (Get-FileHash -LiteralPath $gameExe -Algorithm SHA256).Hash
if ($srcHash -ne $dstHash) { Fail "client copy did not match $clientExe (is the game running or the file locked?)" }

# --- 3. EAC catalogue -----------------------------------------------------------
Write-Host ""
Write-Host "== re-sign the EAC integrity catalogue"
# reLCS-launcher.exe is a GUI-subsystem binary: calling it with & returns at once
# and $LASTEXITCODE stays empty, so the old catalogue is checked while signing is
# still running. Start-Process -Wait is the only reliable wait for it.
$sign = Start-Process -FilePath $launcherExe `
                     -ArgumentList "-game `"$gameExe`" -eacintegrity" `
                     -NoNewWindow -Wait -PassThru
if ($sign.ExitCode -ne 0) {
  $log = Join-Path (Split-Path -Parent $launcherExe) "launcher.log"
  if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Tail 10 | ForEach-Object { Write-Host "   $_" } }
  Fail "EAC catalogue signing failed (exit $($sign.ExitCode))"
}

$certDir = Join-Path $game "EasyAntiCheat\Certificates"
$catalogue = Join-Path $certDir "base.bin"
if (-not (Test-Path -LiteralPath $catalogue)) { Fail "signing reported success but $catalogue is missing" }
if ((Get-Item -LiteralPath $catalogue).LastWriteTime -lt (Get-Item -LiteralPath $gameExe).LastWriteTime) {
  Fail "catalogue is older than the client; signing did not refresh it"
}

$payloadCerts = Join-Path $repo "net\eac\runtime\EasyAntiCheat\Certificates"
Copy-Item -Path (Join-Path $certDir "*") -Destination $payloadCerts -Force
Write-Host "   catalogue copied into net/eac (ships inside the launcher zip)"

# --- 4. package ----------------------------------------------------------------
Write-Host ""
& (Join-Path $PSScriptRoot "package-release.ps1") -Version $Version
if ($LASTEXITCODE -ne 0) { Fail "launcher packaging failed (exit $LASTEXITCODE)" }

$zip = Join-Path $repo "out\reLCS-launcher-v$Version-win64.zip"
if (-not (Test-Path -LiteralPath $zip)) { Fail "packaging produced no $zip" }

# --- 5. packaged selftest ------------------------------------------------------
# The zip can be structurally complete and still not start (the v0.3.x CEF bugs).
# Run the copy that will ship, from its own folder, and read its log rather than
# trusting the archive listing. GUI subsystem: Start-Process -Wait again.
if ($SkipSelftest) {
  Write-Host ""
  Write-Host "== packaged selftest (skipped)" -ForegroundColor Yellow
} else {
  $staging = Join-Path $repo "out\package\reLCS-launcher"
  $stagedLauncher = Join-Path $staging "reLCS-launcher.exe"
  if (-not (Test-Path -LiteralPath $stagedLauncher)) { Fail "staged launcher missing: $stagedLauncher" }

  Write-Host ""
  Write-Host "== packaged selftest"
  $selftestLog = Join-Path $staging "launcher.log"
  Remove-Item -LiteralPath $selftestLog -Force -ErrorAction SilentlyContinue
  $test = Start-Process -FilePath $stagedLauncher -ArgumentList "-selftest" `
                       -WorkingDirectory $staging -NoNewWindow -Wait -PassThru
  $logText = ""
  if (Test-Path -LiteralPath $selftestLog) { $logText = Get-Content -LiteralPath $selftestLog -Raw }

  if ($test.ExitCode -ne 0 -or $logText -notmatch '\[SELFTEST\] RESULT: PASS') {
    if ($logText) { ($logText -split "`n") | Select-Object -Last 12 | ForEach-Object { Write-Host "   $($_.TrimEnd())" } }
    Fail "packaged launcher selftest failed (exit $($test.ExitCode)); see $selftestLog"
  }
  Write-Host "   RESULT: PASS"
}

$clientAsset = Join-Path $repo "out\reLCS-client-v$Version-win64.exe"
Copy-Item -LiteralPath $clientExe -Destination $clientAsset -Force

# --- 5. what to publish --------------------------------------------------------
Write-Host ""
Write-Host "== release artifacts"
Get-FileHash -LiteralPath $zip, $clientAsset -Algorithm SHA256 |
  ForEach-Object { Write-Host ("   {0}  {1}" -f $_.Hash, $_.Path) }

if ($Publish) {
  # Publishing targets the pushed branch tip, so the tag and the package agree.
  # A local build from unpushed commits would otherwise tag the old remote tip.
  Write-Host ""
  Write-Host "== publish v$Version"
  $branch = (& git -C $repo rev-parse --abbrev-ref HEAD).Trim()
  if ($branch -eq "HEAD") { Fail "detached HEAD: check out the release branch before -Publish" }

  $head = (& git -C $repo rev-parse HEAD).Trim()
  $remote = (& git -C $repo ls-remote "https://github.com/$repoSlug.git" "refs/heads/$branch") -split "\s+" | Select-Object -First 1
  if (-not $remote) { Fail "branch $branch does not exist on $repoSlug; push it before -Publish" }
  if ($remote -ne $head) {
    Fail "HEAD $head is not pushed to $branch ($remote on GitHub); push before -Publish"
  }

  $ghArgs = @("release", "create", "v$Version", "--repo", $repoSlug, "--target", $branch,
              "--title", $Title, "--notes-file", $NotesFile)
  if ($Draft) { $ghArgs += "--draft" }
  $ghArgs += @($zip, $clientAsset)
  $ghOut = & gh @ghArgs
  if ($LASTEXITCODE -ne 0) { Fail "gh release create failed (exit $LASTEXITCODE)" }
  # gh prints the real URL. A draft has no tag yet, so its URL is
  # /releases/tag/untagged-... and cannot be constructed from the version.
  $url = @($ghOut) | Where-Object { $_ -match '^https?://' } | Select-Object -Last 1
  if (-not $url) { $url = "https://github.com/$repoSlug/releases/tag/v$Version" }
  $kind = if ($Draft) { "draft release (not public)" } else { "release" }
  Write-Host "   ${kind}: $url"
  if ($Draft) { Write-Host "   publish it with: gh release edit v$Version --repo $repoSlug --draft=false" }
} else {
  Write-Host ""
  Write-Host "Publish with:"
  Write-Host "   gh release create v$Version --repo $repoSlug --target port/multiplayer \"
  Write-Host "     --notes-file _scratch/release-notes-$Version.md \"
  Write-Host "     `"$zip`" `"$clientAsset`""
  Write-Host "   (or rerun this script with -Publish to do the same after the checks)"
}
