# Builds the PC preview into build/pc and copies runtime files next to the exe.
$ErrorActionPreference = 'Stop'
$proj = Split-Path -Parent $PSScriptRoot
$tc   = 'C:\Users\Zenth\dev\toolchains'
$llvm = (Get-ChildItem "$tc\llvm-mingw" -Directory | Select-Object -First 1).FullName
$sdl  = (Get-ChildItem "$tc\sdl2" -Directory | Select-Object -First 1).FullName

$ninja = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter ninja.exe | Select-Object -First 1).DirectoryName
$env:Path = "$llvm\bin;C:\Program Files\CMake\bin;$ninja;$env:Path"
$out = "$proj\build\pc"

cmake -S $proj -B $out -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "-DSDL2_ROOT=$sdl"
if ($LASTEXITCODE) { throw 'configure failed' }
cmake --build $out
if ($LASTEXITCODE) { throw 'build failed' }

$arch = 'x86_64-w64-mingw32'
Copy-Item "$sdl\$arch\bin\SDL2.dll" $out -Force
foreach ($d in 'libc++.dll', 'libunwind.dll') {
    $f = Get-ChildItem "$llvm\$arch\bin" -Filter $d -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($f) { Copy-Item $f.FullName $out -Force }
}
robocopy "$proj\assets" "$out\assets" /MIR /NFL /NDL /NJH /NJS | Out-Null
Write-Host "Built: $out\nexus.exe"
