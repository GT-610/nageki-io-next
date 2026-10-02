$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vsRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC'
$kitRoot = 'C:\Program Files (x86)\Windows Kits\10'
$version = Get-ChildItem -LiteralPath $vsRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
$kit = Get-ChildItem -LiteralPath (Join-Path $kitRoot 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $version -or -not $kit) { throw 'MSVC and Windows SDK required' }
# segatools LoadLibraryW's this DLL inside mu3.exe and amdaemon.exe, which are
# both 64-bit (the frozen MU3Input.dll is machine=0x8664). x86 here only ever
# produces ERROR_BAD_EXE_FORMAT (0x800700c1) at load time.
$compiler = Join-Path $version.FullName 'bin\Hostx64\x64\cl.exe'
$env:INCLUDE = @((Join-Path $version.FullName 'include'), (Join-Path $kit.FullName 'ucrt'), (Join-Path $kit.FullName 'um'), (Join-Path $kit.FullName 'shared')) -join ';'
$env:LIB = @((Join-Path $version.FullName 'lib\x64'), (Join-Path $kitRoot "Lib\$($kit.Name)\ucrt\x64"), (Join-Path $kitRoot "Lib\$($kit.Name)\um\x64")) -join ';'
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
Push-Location $build
try {
    & $compiler /nologo /W4 /WX /wd5105 /std:c11 /LD '/Fe:MU3CustomIO.dll' (Join-Path $root 'src\io_core.c') (Join-Path $root 'src\hid_device.c') (Join-Path $root 'src\led_packet.c') (Join-Path $root 'src\card_id.c') (Join-Path $root 'src\lever.c') (Join-Path $root 'src\mu3_io.c') "/link" '/MACHINE:X64' "/DEF:$(Join-Path $root 'src\mu3_io.def')" setupapi.lib hid.lib
    if ($LASTEXITCODE -ne 0) { throw "DLL build failed: $LASTEXITCODE" }
    # Guard the exact defect that produced LoadLibraryW error 0x800700c1 on the
    # cabinet: a 32-bit DLL in a 64-bit mu3.exe/amdaemon.exe.
    $dllPath = Join-Path $build 'MU3CustomIO.dll'
    $bytes = [System.IO.File]::ReadAllBytes($dllPath)
    $lfanew = [BitConverter]::ToInt32($bytes, 0x3C)
    $machine = [BitConverter]::ToUInt16($bytes, $lfanew + 4)
    $optMagic = [BitConverter]::ToUInt16($bytes, $lfanew + 24)
    if ($machine -ne 0x8664 -or $optMagic -ne 0x20B) {
        throw ("Architecture check failed: machine=0x{0:X4} optmagic=0x{1:X4}; expected 0x8664/0x020B (PE32+ x64)" -f $machine, $optMagic)
    }
    Write-Output ("Architecture check passed: machine=0x{0:X4} optmagic=0x{1:X4} (PE32+ x64)" -f $machine, $optMagic)
    & $compiler /nologo /W4 /WX /wd5105 /std:c11 "/Fe:$(Join-Path $build 'dll_smoke.exe')" (Join-Path $root 'tests\dll_smoke.c')
    if ($LASTEXITCODE -ne 0) { throw "Smoke test compile failed: $LASTEXITCODE" }
    & (Join-Path $build 'dll_smoke.exe')
    if ($LASTEXITCODE -ne 0) { throw "DLL smoke failed: $LASTEXITCODE" }
    & $compiler /nologo /W4 /WX /wd5105 /std:c11 "/Fe:$(Join-Path $build 'bind_sim.exe')" (Join-Path $root 'tests\bind_sim.c')
    if ($LASTEXITCODE -ne 0) { throw "Bind simulation compile failed: $LASTEXITCODE" }
    & (Join-Path $build 'bind_sim.exe') $dllPath
    if ($LASTEXITCODE -ne 0) { throw "Bind simulation failed: $LASTEXITCODE" }
    & $compiler /nologo /W4 /WX /wd5105 /std:c11 "/Fe:$(Join-Path $build 'hid_probe.exe')" (Join-Path $root 'tests\hid_probe.c') "/link" setupapi.lib hid.lib
    if ($LASTEXITCODE -ne 0) { throw "HID probe compile failed: $LASTEXITCODE" }
    Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $build 'MU3CustomIO.dll') | Format-List
} finally { Pop-Location }
