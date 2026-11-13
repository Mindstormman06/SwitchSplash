# Builds SwitchSplash. GNU make can't handle spaces in the project path
# ("Nintendo Stuff"), so this builds through a space-free junction under %TEMP%.
# Usage:  .\build.ps1            -> make dist
#         .\build.ps1 clean      -> make clean
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
} finally {
    Pop-Location
}
