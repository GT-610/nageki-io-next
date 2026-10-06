# Locates the MSVC x64 compiler and the Windows SDK, and exports INCLUDE/LIB so
# that cl.exe can be invoked directly.
#
# vswhere is used rather than a hardcoded install path because the two places
# this runs differ: a developer machine usually has Build Tools under
# "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools", while a
# GitHub windows runner has Visual Studio Enterprise under
# "C:\Program Files\Microsoft Visual Studio\2022\Enterprise". vswhere already
# knows the layout, so it is asked instead of guessing.
#
# Version directories are compared as [version] rather than sorted as strings.
# "14.9.25508" sorts after "14.44.35207" alphabetically, which would silently
# select an older toolset on a machine that has both.
function Initialize-MsvcEnvironment {
    [CmdletBinding()]
    param()

    $ErrorActionPreference = 'Stop'

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "vswhere not found at $vswhere. Install Visual Studio 2022 (or Build Tools) with the C++ workload."
    }

    # Ask for an installation that actually carries the x64 compiler; without the
    # -requires filter vswhere happily returns one that has no toolset at all.
    $vsRoot = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $vsRoot) {
        throw 'No Visual Studio installation with the x64 C++ tools was found.'
    }

    $msvcRoot = Join-Path $vsRoot 'VC\Tools\MSVC'
    $version = Get-ChildItem -LiteralPath $msvcRoot -Directory -ErrorAction Stop |
        Sort-Object { [version]($_.Name -replace '^.*?(\d+\.\d+\.\d+).*$', '$1') } -Descending |
        Select-Object -First 1
    if (-not $version) { throw "No MSVC toolset found under $msvcRoot" }

    $compiler = Join-Path $version.FullName 'bin\Hostx64\x64\cl.exe'
    if (-not (Test-Path -LiteralPath $compiler)) { throw "MSVC compiler missing: $compiler" }

    $kitRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
    $includeRoot = Join-Path $kitRoot 'Include'
    $kit = Get-ChildItem -LiteralPath $includeRoot -Directory -ErrorAction Stop |
        Where-Object { $_.Name -match '^10\.\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1
    if (-not $kit) { throw "No Windows SDK found under $includeRoot" }

    $env:INCLUDE = @(
        (Join-Path $version.FullName 'include')
        (Join-Path $kit.FullName 'ucrt')
        (Join-Path $kit.FullName 'um')
        (Join-Path $kit.FullName 'shared')
    ) -join ';'
    $env:LIB = @(
        (Join-Path $version.FullName 'lib\x64')
        (Join-Path $kitRoot "Lib\$($kit.Name)\ucrt\x64")
        (Join-Path $kitRoot "Lib\$($kit.Name)\um\x64")
    ) -join ';'

    return [pscustomobject]@{
        Compiler = $compiler
        Version  = $version.Name
        Sdk      = $kit.Name
        VsRoot   = $vsRoot
    }
}