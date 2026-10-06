# Prove the single-file claim for ghost.exe.
#
# ghost.exe carries the shim as an RCDATA resource and extracts it on first use.
# Copying it into a directory that holds nothing else, wiping the extraction
# cache, and running its self-test there is the only check that would notice a
# missing or stale embedded resource -- a normal build-tree run would silently
# pick the sibling files up from disk instead.
#
#   pwsh -File tools/verify_ghost.ps1 -GhostExe native/build/bin/ghost.exe
#
# Exits 0 only when the self-test passes 9/9 out of a cold cache.
param(
    [Parameter(Mandatory = $true)][string]$GhostExe,
    [string]$WorkDir = ''
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $GhostExe)) {
    throw "ghost.exe not found at $GhostExe"
}
$GhostExe = (Resolve-Path -LiteralPath $GhostExe).Path
if ([string]::IsNullOrWhiteSpace($WorkDir)) {
    $WorkDir = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }
}

$ship = Join-Path $WorkDir 'ghost-selfcontained'
if (Test-Path -LiteralPath $ship) { Remove-Item -LiteralPath $ship -Recurse -Force }
New-Item -ItemType Directory -Path $ship -Force | Out-Null
Copy-Item -LiteralPath $GhostExe -Destination $ship
$exe = Join-Path $ship 'ghost.exe'

$cache = Join-Path $env:LOCALAPPDATA 'GhostBrowser\engine'
if (Test-Path -LiteralPath $cache) { Remove-Item -LiteralPath $cache -Recurse -Force }

Write-Host "shipped copy : $exe"
Write-Host ("size         : {0} bytes" -f (Get-Item -LiteralPath $exe).Length)
Write-Host "cache wiped  : $cache"

$out = Join-Path $WorkDir 'ghost-selftest.out'
$err = Join-Path $WorkDir 'ghost-selftest.err'
$p = Start-Process -FilePath $exe -ArgumentList 'selftest' -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput $out -RedirectStandardError $err
if (-not $p.WaitForExit(180000)) { $p.Kill(); throw 'ghost selftest did not exit within 180s' }

$text = Get-Content -LiteralPath $out -Raw
Write-Host $text
$errText = Get-Content -LiteralPath $err -Raw -ErrorAction SilentlyContinue
if ($errText) { Write-Host $errText }

if ($p.ExitCode -ne 0) { throw "ghost selftest exited with $($p.ExitCode)" }
if ($text -notlike '*9/9 checks passed*') { throw 'ghost selftest did not report 9/9' }

# The extraction directory must exist and be named after the resource content
# hash, which is what makes a stale shim impossible to reuse. Exactly one file
# is expected: extracting and executing a second unsigned executable is the
# pattern Defender quarantined, so the self-test target is ghost.exe itself.
$extracted = @(Get-ChildItem -LiteralPath $cache -Recurse -File -ErrorAction SilentlyContinue)
if ($extracted.Count -ne 1) {
    throw "expected only the shim to be extracted into $cache, found $($extracted.Count) file(s)"
}
if ($extracted[0].Name -ne 'ghost_shim.dll') {
    throw "expected the extracted file to be ghost_shim.dll, found $($extracted[0].Name)"
}
Write-Host ("extracted    : {0}" -f (($extracted | ForEach-Object { $_.Name }) -join ', '))
Write-Host 'PASS: the lone ghost.exe spoofed every probe value out of a cold cache'
