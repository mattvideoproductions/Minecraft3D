# Host tests for the v0.3 pure code (no 3DS, no emulator).
#   1. Python model of pica_shadow / projection / hysteresis / pacer (runs everywhere)
#   2. C++ host test: compile-checked with devkitARM; built and run if a desktop g++ is on PATH
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$py = if (Test-Path "C:\Python310\python.exe") { "C:\Python310\python.exe" } else { "python" }
& $py -I (Join-Path $here "test_c3_model.py")
if ($LASTEXITCODE -ne 0) { throw "Python model tests failed" }

$gxx = "C:\devkitPro\devkitARM\bin\arm-none-eabi-g++.exe"
$inc = Join-Path $here "..\include"
& $gxx -std=gnu++20 -Wall -Wextra -fsyntax-only "-I$inc" (Join-Path $here "test_pica_shadow.cpp")
if ($LASTEXITCODE -ne 0) { throw "C++ host test does not compile" }
Write-Host "C++ host test: compiles (devkitARM syntax check)"

$host_gxx = Get-Command g++ -ErrorAction SilentlyContinue
if ($host_gxx) {
    $exe = Join-Path $env:TEMP "mc3d_host_test.exe"
    & g++ -std=c++20 -O1 -Wall -Wextra "-I$inc" (Join-Path $here "..\source\pica_shadow.cpp") (Join-Path $here "test_pica_shadow.cpp") -o $exe
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "C++ host test failed" }
} else {
    Write-Host "No desktop g++ on PATH: C++ host test not run (Python model covers the same cases)."
}
