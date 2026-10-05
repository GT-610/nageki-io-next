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
Copy-Item -LiteralPath (Join-Path $build 'trace_analyze.exe') -Destination (Join-Path $stage 'trace_analyze.exe') -Force
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
    'Captured on this controller: left 0x032A, right 0x04A4. The electrical'
    'centre is 0x0400, so lever_neutral=1024 gives exactly 0000H there.'
    'The resting value is NOT the centre: the stick cannot be parked exactly'
    'at it, so it reads wherever it is left (1026 and 1035 were both seen).'
    'Do not set lever_neutral from a resting reading.'
    'Set lever_neutral=0 to reproduce the original DLL''s uncentred output.'
    ''
    'The operator byte (payload offset 23) has no option here on purpose.'
    'A capture shows the Test-menu button reporting 0x03 (Test+Service) and'
    'the other rear button 0x04 (Coin), and this DLL passes both through'
    'unchanged. Rewriting that byte to separate the two keys would make the'
    'DLL stop reporting the controller''s real state; it is not implemented.'
    ''
    'BUTTON TRACE (off by default; needs no exclusive access to the cabinet).'
    'Remove the semicolons from the two lines below and restart mu3.exe and'
    'amdaemon.exe. Each process then writes a log beside this DLL, of only'
    'button CHANGES:'
    '  MU3CustomIO-wire.log    the button bytes as the device sent them'
    '  MU3CustomIO-served.log  the sample mu3_io_poll handed the game'
    'Analyze with:  trace_analyze.exe MU3CustomIO-wire.log'
    'A key that misbehaves on the wire is a controller fault. A key clean on'
    'the wire but missing from served is a software fault.'
    '; [trace]'
    '; enabled=1'
) | Set-Content -Encoding ascii -LiteralPath $ini

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $stage 'MU3CustomIO.dll')).Hash
$probeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $stage 'hid_probe.exe')).Hash
$analyzeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $stage 'trace_analyze.exe')).Hash
@(
    "MU3CustomIO.dll   SHA256 $hash",
    "hid_probe.exe     SHA256 $probeHash",
    "trace_analyze.exe SHA256 $analyzeHash",
    "architecture      x64 (machine=0x8664, PE32+)",
    "",
    "Verified offline: PE architecture, DLL load, 26 exports, segatools bind",
    "simulation (mu3 7/7, aime 17/17), core/LED/card/lever/HID-marshalling and",
    "button-map tests. The button-map test proves key decoding is per-key",
    "independent over all 1024 possible button-byte vectors, so a fault that",
    "depends on which keys are held cannot originate in this DLL.",
    "",
    "Not yet verified on the cabinet: the two input-path fixes below. They are",
    "structural fixes for defects read out of the code, not diagnoses of an",
    "observed symptom.",
    "  1. A failed LED write no longer tears down input. It used to break the",
    "     read loop, which reported neutral input for at least the 500 ms",
    "     reconnect backoff: one timed-out colour write cost half a second of",
    "     no input. Input validity is now decided by reads alone.",
    "  2. In the game process, a shared-memory copy that raced the owner's",
    "     write no longer returns a neutral frame. It now serves the previous",
    "     consistent sample, so a held key cannot be released for one poll;",
    "     a departed owner still clears the cache to neutral immediately.",
    "",
    "Verified against a real capture of THIS controller (hid_probe.exe dump):",
    "  report id byte = 0x00; lever field = report bytes 11..12 little-endian",
    "  full left 0x032A (810)   full right 0x04A4 (1188)",
    "  -> electrical centre lever_neutral = 1024 (0x400)",
    "  -> travel 378 counts; excursions from centre -214 (left) / +164 (right)",
    "  The resting reading is not a hardware constant: the stick cannot be",
    "  parked exactly at centre (1026 and 1035 both observed). Do not set",
    "  lever_neutral from it.",
    "",
    "  operator byte = payload offset 23 (wire byte 24), also capture-confirmed:",
    "  the Test-menu button reports 0x03 (Test+Service together) and the other",
    "  rear button reports 0x04 (Coin), both 0x00 on release. A standalone 0x02",
    "  (Service) was never seen, so Service cannot be triggered on its own.",
    "  Coin is not verifiable in game: mu3hook counts a credit on every poll",
    "  where that bit is set, with no rising-edge check.",
    "",
    "Lever centre is now subtracted before scaling. At the default sensitivity",
    "2 this is bit-identical to the original DLL; it removes the odd/even",
    "sensitivity fault and raises the usable sensitivity ceiling from 2 to 4.",
    "Tune with MU3CustomIO.ini (see MU3CustomIO.ini.txt); set lever_neutral=0",
    "to reproduce the original uncentred output instead.",
    "",
    "The controller streams continuously at about 200 reports/s (measured:",
    "5986 reports in 30 s with the lever untouched, all one value). So the",
    "input is never at risk from a read timeout, and a noise gate is not",
    "warranted: the reading has a spread of 0 counts.",
    "",
    "hid_probe.exe jitter <ms> reproduces that measurement. Run it before",
    "considering a noise gate or touching lever_neutral.",
    "",
    "hid_probe.exe chord <ms> is the device-side test for a report that several",
    "keys held at once stop a further key from registering. It reads the ten",
    "button bytes straight off the wire, with the game closed, so its result",
    "does not depend on this DLL or on segatools. It counts, per key, how many",
    "other keys were already held when that key went down; a key counted under",
    "four or more keys already held was reported by the controller under that",
    "combination. See the output for how to read it.",
    "",
    "hid_probe.exe needs the cabinet to itself. When it is in use, enable the",
    "in-DLL trace instead ([trace] enabled=1 in MU3CustomIO.ini) and run",
    "trace_analyze.exe over MU3CustomIO-wire.log afterwards. That reports, per",
    "key, how often it clicked: a release and re-press within 15 ms, which a",
    "player cannot do on purpose and a bouncing contact does by itself. Read it",
    "as clicks per press, not as a raw count, and compare the keys against each",
    "other: one key clicking while the others stay near zero is the hardware,",
    "all keys clicking alike is the player.",
    "",
    "Still NOT verified on hardware: the key bytes (0-9), scan/card and LED",
    "mapping, in-game fault display, amdaemon.exe being the sole HID owner.",
    "The lever field and the operator byte ARE capture-confirmed (above).") | Set-Content -Encoding utf8 -LiteralPath (Join-Path $stage 'SHA256.txt')

Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -LiteralPath (Join-Path $stage 'MU3CustomIO.dll'), (Join-Path $stage 'hid_probe.exe'), (Join-Path $stage 'trace_analyze.exe'), (Join-Path $stage 'MU3CustomIO.ini.txt'), (Join-Path $stage 'README.md'), (Join-Path $stage 'README.zh-CN.md'), (Join-Path $stage 'SHA256.txt') -DestinationPath $zip -Force
Get-FileHash -Algorithm SHA256 -LiteralPath $zip | Format-List
Get-Item -LiteralPath $zip | Select-Object FullName, Length
