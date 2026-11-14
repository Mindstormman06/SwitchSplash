# Builds SwitchSplash: the sysmodule and the Ultrahand overlay. GNU make can't handle spaces
# in the project path ("Nintendo Stuff"), so this builds through a space-free junction
# under %TEMP%.
# Usage:  .\build.ps1            -> sysmodule `make dist` + overlay, both into dist/
#         .\build.ps1 clean      -> clean both
param([string]$Target = "dist")

$ErrorActionPreference = "Stop"
$junction = Join-Path $env:TEMP "SwitchSplash-build"
if (-not (Test-Path $junction)) {
    New-Item -ItemType Junction -Path $junction -Target $PSScriptRoot | Out-Null
}

Push-Location $junction
try {
    # Non-login bash keeps the junction as the cwd (a login shell would cd to $HOME).
    & C:\devkitPro\msys2\usr\bin\bash.exe -c "export DEVKITPRO=/opt/devkitpro PATH=/usr/bin:/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin; make $Target 2>&1 | grep -v 'field not present'"

    # Overlay (needs devkitPro's switch-curl, switch-zlib, switch-mbedtls; see overlay/README.md).
    Push-Location "$junction\overlay"
    try {
        $ovlTarget = if ($Target -eq "clean") { "clean" } else { "" }
        & C:\devkitPro\msys2\usr\bin\bash.exe -c "bash ./build.sh $ovlTarget"
        if ($LASTEXITCODE -ne 0) { throw "overlay build failed (see overlay/build.log)" }
    } finally {
        Pop-Location
    }

    if ($Target -eq "dist") {
        $ovlDir = Join-Path $PSScriptRoot "dist\switch\.overlays"
        New-Item -ItemType Directory -Force -Path $ovlDir | Out-Null
        Copy-Item (Join-Path $PSScriptRoot "overlay\switchsplash.ovl") $ovlDir -Force
        Write-Output "built ... dist/switch/.overlays/switchsplash.ovl"
    }
} finally {
    Pop-Location
}
