# Builds Nexus as a native PS5 payload (ps5-payload-dev SDK) and optionally sends it to the
# console for an ephemeral test run (no install, no icon yet - see src/platform/ps5).
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\build-ps5.ps1                  # build only
#   powershell -ExecutionPolicy Bypass -File tools\build-ps5.ps1 -Send -Host <ip> # build + send
param(
    [switch]$Send,
    [string]$ConsoleHost = '192.168.100.22',
    [int]$Port = 9021
)
$ErrorActionPreference = 'Stop'
$proj = Split-Path -Parent $PSScriptRoot
$sdk  = 'C:\Users\Zenth\dev\toolchains\ps5\sdk\ps5-payload-sdk'
# LLVM 18, not whatever is newest: the SDK's prebuilt crt objects need lld from the same major
# version the project is tested against (see its own README), newer lld rejects them as
# duplicate-symbol errors. Also must live at a path with no spaces (clang's own self-location
# lookup breaks the PS5 target's linker search under "C:\Program Files").
$env:Path = "C:\llvm18\bin;$sdk\win;$env:Path"

$out = "$proj\build\ps5"; $obj = "$out\obj"
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $obj | Out-Null

function Check($what) { if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)" } }

$sources = @(
    'src\ui\app.cpp', 'src\ui\gfx.cpp', 'src\ui\brand.cpp', 'src\ui\stb_impl.cpp',
    'src\platform\sample_data.cpp', 'src\platform\ps5\ps5_tilemap.cpp',
    'src\platform\ps5\ps5_platform.cpp', 'src\platform\ps5\ps5_main.cpp')
$flags = @('-Wall', '-Wno-unused-parameter', '-g', '-O2', '-std=gnu++17',
           '-DNEXUS_PS5=1', '-DNEXUS_NO_PNG_WRITE=1', "-I$proj\src")
$objs = @()
foreach ($s in $sources) {
    $o = "$obj\" + ([IO.Path]::GetFileNameWithoutExtension($s)) + '.o'
    Write-Host "compile $s"
    & "$sdk\win\prospero-clang++.cmd" @flags -c -o $o "$proj\$s"
    Check "compiling $s"
    $objs += $o
}

Write-Host 'link'
& "$sdk\win\prospero-clang++.cmd" -g -o "$out\nexus.elf" @objs `
    -lSceVideoOut -lScePad -lSceUserService -lSceSystemService -lSceSysmodule
Check 'linking'
$f = Get-Item "$out\nexus.elf"
Write-Host ("Built: {0}  ({1:N1} MB)" -f $f.FullName, ($f.Length / 1MB))

if ($Send) {
    Write-Host "sending to $ConsoleHost`:$Port ..."
    & "$sdk\win\prospero-deploy.cmd" -h $ConsoleHost -p $Port "$out\nexus.elf"
    Check 'deploy'
    Write-Host 'Sent. Nexus is now running as an ephemeral payload (not installed).'
}
