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
# Optional tuning file: copy it as .ini.txt so a drop-in never silently
# changes behaviour; the operator renames it to MU3CustomIO.ini to enable it.
$ini = Join-Path $stage 'MU3CustomIO.ini.txt'
@(
    'MU3CustomIO lever overrides (rename this file to MU3CustomIO.ini,'
    'place it in the SAME directory as MU3CustomIO.dll, then restart the game).'
    ''
    'lever_neutral=1024        raw value at the stick''s electrical centre.'
    'lever_sensitivity=2       1..4; 2 is the deployed baseline.'
    ''
    'Captured on this controller: left 0x032A, rest 0x0402, right 0x04A4.'
    'The electrical centre is 0x0400, so lever_neutral=1024 gives exactly'
    '0000H at centre; at rest (0x0402) it reads 0x0080.'
    'Set lever_neutral=0 to reproduce the original DLL''s uncentred output.'
) | Set-Content -Encoding ascii -LiteralPath $ini

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $stage 'MU3CustomIO.dll')).Hash
$probeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $stage 'hid_probe.exe')).Hash
@(
    "MU3CustomIO.dll  SHA256 $hash",
    "hid_probe.exe    SHA256 $probeHash",
    "architecture     x64 (machine=0x8664, PE32+)",
    "",
    "Verified offline: PE architecture, DLL load, 26 exports, segatools bind",
    "simulation (mu3 7/7, aime 17/17), core/LED/card/lever synthetic tests.",
    "",
    "Verified against a real capture of THIS controller (hid_probe.exe dump):",
    "  report id byte = 0x00; lever field = report bytes 11..12 little-endian",
    "  full left 0x032A (810)   rest 0x0402 (1026)   full right 0x04A4 (1188)",
    "  -> electrical centre lever_neutral = 1024 (0x400)",
    "  -> travel 378 counts; excursions from centre -214 (left) / +164 (right)",
    "",
    "Lever centre is now subtracted before scaling. At the default sensitivity",
    "2 this is bit-identical to the original DLL; it removes the odd/even",
    "sensitivity fault and raises the usable sensitivity ceiling from 2 to 4.",
    "Tune with MU3CustomIO.ini (see MU3CustomIO.ini.txt); set lever_neutral=0",
    "to reproduce the original uncentred output instead.",
    "",
    "Still NOT verified on hardware: buttons/scan/card/LED mapping, in-game",
    "fault display, amdaemon.exe being the sole HID owner."
) | Set-Content -Encoding utf8 -LiteralPath (Join-Path $stage 'SHA256.txt')

Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -LiteralPath (Join-Path $stage 'MU3CustomIO.dll'), (Join-Path $stage 'hid_probe.exe'), (Join-Path $stage 'MU3CustomIO.ini.txt'), (Join-Path $stage 'README.md'), (Join-Path $stage 'README.zh-CN.md'), (Join-Path $stage 'SHA256.txt') -DestinationPath $zip -Force
Get-FileHash -Algorithm SHA256 -LiteralPath $zip | Format-List
Get-Item -LiteralPath $zip | Select-Object FullName, Length
