$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'toolchain.ps1')
$toolchain = Initialize-MsvcEnvironment
$compiler = $toolchain.Compiler
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
Push-Location $build
try {
    & $compiler /nologo /W4 /WX /wd5105 /std:c11 "/Fe:$(Join-Path $build 'button_map_tests.exe')" (Join-Path $root 'src\button_map.c') (Join-Path $root 'tests\button_map_tests.c')
    if ($LASTEXITCODE -ne 0) { throw "Button map test compile failed: $LASTEXITCODE" }
    & (Join-Path $build 'button_map_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw "Button map tests failed: $LASTEXITCODE" }
    & $compiler /nologo /W4 /WX /wd5105 /std:c11 "/Fe:$(Join-Path $build 'trace_tests.exe')" (Join-Path $root 'src\button_map.c') (Join-Path $root 'src\trace.c') (Join-Path $root 'tests\trace_tests.c')
    if ($LASTEXITCODE -ne 0) { throw "Trace test compile failed: $LASTEXITCODE" }
    & (Join-Path $build 'trace_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw "Trace tests failed: $LASTEXITCODE" }
    # io_core.c now decodes buttons through button_map.c, so it needs it too.
    & $compiler /nologo /W4 /WX /wd5105 /std:c11 "/Fe:$(Join-Path $build 'core_tests.exe')" (Join-Path $root 'src\button_map.c') (Join-Path $root 'src\io_core.c') (Join-Path $root 'tests\core_tests.c')
    if ($LASTEXITCODE -ne 0) { throw "Compiler failed: exit $LASTEXITCODE" }
    & (Join-Path $build 'core_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw "Offline IO tests failed: exit $LASTEXITCODE" }
    & $compiler /nologo /W4 /WX /wd5105 /std:c11 "/Fe:$(Join-Path $build 'led_tests.exe')" (Join-Path $root 'src\led_packet.c') (Join-Path $root 'tests\led_tests.c')
    if ($LASTEXITCODE -ne 0) { throw "LED test compile failed: exit $LASTEXITCODE" }
    & (Join-Path $build 'led_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw "LED tests failed: exit $LASTEXITCODE" }
    & $compiler /nologo /W4 /WX /wd5105 /std:c11 "/Fe:$(Join-Path $build 'card_tests.exe')" (Join-Path $root 'src\card_id.c') (Join-Path $root 'tests\card_tests.c')
    if ($LASTEXITCODE -ne 0) { throw "Card test compile failed: exit $LASTEXITCODE" }
    & (Join-Path $build 'card_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw "Card tests failed: exit $LASTEXITCODE" }
    & $compiler /nologo /W4 /WX /wd5105 /std:c11 "/Fe:$(Join-Path $build 'lever_tests.exe')" (Join-Path $root 'src\lever.c') (Join-Path $root 'tests\lever_tests.c')
    if ($LASTEXITCODE -ne 0) { throw "Lever test compile failed: exit $LASTEXITCODE" }
    & (Join-Path $build 'lever_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw "Lever tests failed: exit $LASTEXITCODE" }
    # hid_device.c pulls in SetupAPI and HID, so this one needs those libs.
    & $compiler /nologo /W4 /WX /wd5105 /std:c11 "/Fe:$(Join-Path $build 'hid_pack_tests.exe')" (Join-Path $root 'src\hid_device.c') (Join-Path $root 'tests\hid_pack_tests.c') "/link" setupapi.lib hid.lib
    if ($LASTEXITCODE -ne 0) { throw "HID marshalling test compile failed: exit $LASTEXITCODE" }
    & (Join-Path $build 'hid_pack_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw "HID marshalling tests failed: exit $LASTEXITCODE" }
} finally { Pop-Location }
