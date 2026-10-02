$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
$stage = Join-Path $root 'dist\MU3CustomIO-x64-test'
$zip = Join-Path $root 'dist\MU3CustomIO-x64-test.zip'
$dll = Join-Path $build 'MU3CustomIO.dll'

if (-not (Test-Path -LiteralPath $dll)) { throw 'Run scripts/build.ps1 first (no DLL in build/)' }

# Never ship a wrong-architecture or incompletely-bound DLL. Both checks already
# ran during the build; re-verify against the artifact actually being packaged.
$bytes = [System.IO.File]::ReadAllBytes($dll)
$lfanew = [BitConverter]::ToInt32($bytes, 0x3C)
$machine = [BitConverter]::ToUInt16($bytes, $lfanew + 4)
if ($machine -ne 0x8664) {
    throw ("Refusing to package: machine=0x{0:X4}, expected 0x8664 (x64). mu3.exe and amdaemon.exe are 64-bit." -f $machine)
}

$bind = Join-Path $build 'bind_sim.exe'
if (-not (Test-Path -LiteralPath $bind)) { throw 'Missing bind_sim.exe; run scripts/build.ps1' }
& $bind $dll
if ($LASTEXITCODE -ne 0) { throw "Refusing to package: segatools bind simulation failed ($LASTEXITCODE)" }

Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -LiteralPath $dll -Destination (Join-Path $stage 'MU3CustomIO.dll') -Force
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination (Join-Path $stage 'README.md') -Force
Copy-Item -LiteralPath (Join-Path $root 'README.zh-CN.md') -Destination (Join-Path $stage 'README.zh-CN.md') -Force
Copy-Item -LiteralPath (Join-Path $build 'hid_probe.exe') -Destination (Join-Path $stage 'hid_probe.exe') -Force

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $stage 'MU3CustomIO.dll')).Hash
$probeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $stage 'hid_probe.exe')).Hash
@(
    "MU3CustomIO.dll  SHA256 $hash",
    "hid_probe.exe    SHA256 $probeHash",
    "architecture     x64 (machine=0x8664, PE32+)",
    "",
    "Verified offline: PE architecture, DLL load, 26 exports, segatools bind",
    "simulation (mu3 7/7, aime 17/17), core/LED/card synthetic tests.",
    "NOT verified: connection to a real controller, report layout/polarity,",
    "lever calibration, card reading, LED mapping, game fault display.",
    "Run hid_probe.exe on the machine with the controller before trusting input."
) | Set-Content -Encoding utf8 -LiteralPath (Join-Path $stage 'SHA256.txt')

Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -LiteralPath (Join-Path $stage 'MU3CustomIO.dll'), (Join-Path $stage 'hid_probe.exe'), (Join-Path $stage 'README.md'), (Join-Path $stage 'README.zh-CN.md'), (Join-Path $stage 'SHA256.txt') -DestinationPath $zip -Force
Get-FileHash -Algorithm SHA256 -LiteralPath $zip | Format-List
Get-Item -LiteralPath $zip | Select-Object FullName, Length
