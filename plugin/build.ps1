# Build Minecraft3D.3gx from Windows PowerShell.
#   .\build.ps1            release -> Minecraft3D.3gx
#   .\build.ps1 debug      debug   -> Minecraft3D-debug.3gx
#   .\build.ps1 clean
# Runs build.sh inside the devkitPro msys2 shell (which handles the
# no-spaces junction needed by devkitPro's make rules).
param([string]$Target = "release")

$ErrorActionPreference = "Stop"
$devkitPro = if ($env:DEVKITPRO_WIN) { $env:DEVKITPRO_WIN } else { "C:\devkitPro" }
$bash = Join-Path $devkitPro "msys2\usr\bin\bash.exe"
if (-not (Test-Path $bash)) { throw "devkitPro msys2 bash not found at $bash" }

$script = Join-Path $PSScriptRoot "build.sh"
# Convert C:\x\y -> /c/x/y for msys2
$unix = "/" + $script.Substring(0,1).ToLower() + ($script.Substring(2) -replace '\\','/')

$env:MSYSTEM = "MSYS"
$env:CHERE_INVOKING = "1"
& $bash -l $unix $Target
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
