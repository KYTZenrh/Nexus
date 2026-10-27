# Builds the Nexus PS4 package (OpenOrbis toolchain) into dist\ps4\<content id>.pkg
# Usage: powershell -ExecutionPolicy Bypass -File tools\build-ps4.ps1
$ErrorActionPreference = 'Continue'   # native tools write warnings to stderr; we check exit codes instead
$proj = Split-Path -Parent $PSScriptRoot
$tc   = 'C:\Users\Zenth\dev\toolchains\openorbis\toolchain\OpenOrbis\PS4Toolchain'
$bin  = "$tc\bin\windows"
$env:OO_PS4_TOOLCHAIN = $tc
$env:DOTNET_ROLL_FORWARD = 'Major'            # the package tool targets an older .NET than is installed
$env:Path = "C:\Program Files\LLVM\bin;$env:Path"

$titleId   = 'KYTN00001'
$contentId = 'IV0000-KYTN00001_00-NEXUSDASH0000000'
$title     = 'Nexus'
$version   = '0.21'

$out = "$proj\build\ps4"; $obj = "$out\obj"; $pkg = "$out\pkg"
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $obj, "$pkg\sce_sys\about", "$pkg\sce_module", "$pkg\assets\fonts", "$pkg\assets\trophies" | Out-Null

function Check($what) { if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)" } }

# ---- compile
$sources = @(
    'src\ui\app.cpp', 'src\ui\gfx.cpp', 'src\ui\brand.cpp', 'src\ui\stb_impl.cpp',
    'src\platform\sample_data.cpp', 'src\platform\ps4\ps4_platform.cpp', 'src\platform\ps4\ps4_main.cpp')
$flags = @('--target=x86_64-pc-freebsd12-elf', '-fPIC', '-funwind-tables', '-O2', '-std=gnu++17',   # GNU mode: the toolchain's libc++ headers need it (strict ISO hides nanosleep etc.)

           '-DNEXUS_PS4=1', '-DNEXUS_NO_PNG_WRITE=1', "-I$tc\include", "-I$tc\include\c++\v1", "-I$proj\src")
$objs = @()
foreach ($s in $sources) {
    $o = "$obj\" + [IO.Path]::GetFileNameWithoutExtension($s) + '.o'
    Write-Host "compile $s"
    & clang++ @flags -c -o $o "$proj\$s"
    Check "compiling $s"
    $objs += $o
}

# ---- link and sign the executable
Write-Host 'link'
& ld.lld -m elf_x86_64 -pie --script "$tc\link.x" --eh-frame-hdr -o "$out\nexus.elf" "-L$tc\lib" `
    -lc -lkernel -lc++ -lSceVideoOut -lScePad -lSceUserService "$tc\lib\crt1.o" @objs
Check 'linking'
& "$bin\create-fself.exe" -in "$out\nexus.elf" --out "$out\nexus.oelf" --eboot "$pkg\eboot.bin" --paid 0x3800000000000011
Check 'create-fself'

# ---- stage the package contents
Copy-Item "$proj\assets\sce_sys\icon0.png", "$proj\assets\sce_sys\pic1.png" "$pkg\sce_sys\"
Copy-Item "$tc\samples\graphics\sce_sys\about\right.sprx" "$pkg\sce_sys\about\"
Copy-Item "$tc\samples\graphics\sce_module\*.prx" "$pkg\sce_module\"     # stub libc / libSceFios2 modules
Copy-Item "$proj\assets\fonts\*.ttf" "$pkg\assets\fonts\"
Copy-Item "$proj\assets\trophies\*.png" "$pkg\assets\trophies\"

Push-Location $pkg
$P = "$bin\PkgTool.Core.exe"
$sfo = 'sce_sys/param.sfo'
& $P sfo_new $sfo | Out-Null
& $P sfo_setentry $sfo APP_TYPE --type Integer --maxsize 4 --value 1 | Out-Null
& $P sfo_setentry $sfo APP_VER --type Utf8 --maxsize 8 --value $version | Out-Null
& $P sfo_setentry $sfo ATTRIBUTE --type Integer --maxsize 4 --value 0 | Out-Null
& $P sfo_setentry $sfo CATEGORY --type Utf8 --maxsize 4 --value 'gd' | Out-Null
& $P sfo_setentry $sfo CONTENT_ID --type Utf8 --maxsize 48 --value $contentId | Out-Null
& $P sfo_setentry $sfo DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0 | Out-Null
& $P sfo_setentry $sfo SYSTEM_VER --type Integer --maxsize 4 --value 0 | Out-Null
& $P sfo_setentry $sfo TITLE --type Utf8 --maxsize 128 --value $title | Out-Null
& $P sfo_setentry $sfo TITLE_ID --type Utf8 --maxsize 12 --value $titleId | Out-Null
& $P sfo_setentry $sfo VERSION --type Utf8 --maxsize 8 --value $version | Out-Null

$files = @('eboot.bin', 'sce_sys/about/right.sprx', 'sce_sys/param.sfo', 'sce_sys/icon0.png', 'sce_sys/pic1.png',
           'sce_module/libc.prx', 'sce_module/libSceFios2.prx')
foreach ($f in Get-ChildItem 'assets' -Recurse -File) {
    $files += ($f.FullName.Substring($pkg.Length + 1) -replace '\\', '/')
}
Write-Host "package files: $($files.Count)"
& "$bin\create-gp4.exe" -out pkg.gp4 "--content-id=$contentId" --files ($files -join ' ') | Out-Null
Check 'create-gp4'
# create-gp4 only declares a fixed set of asset folders (audio, fonts, images, misc, videos);
# declare ours too, otherwise pkg_build cannot place the files.
$gp4 = Get-Content 'pkg.gp4' -Raw
$gp4 = $gp4.Replace('<dir targ_name="videos" />', "<dir targ_name=`"videos`" />`r`n`t`t`t<dir targ_name=`"trophies`" />")
if (-not $gp4.Contains('targ_name="trophies"')) { throw 'could not add the trophies folder to pkg.gp4' }
Set-Content 'pkg.gp4' $gp4 -Encoding utf8
& $P pkg_build pkg.gp4 . | Select-Object -Last 3
Check 'pkg_build'
Pop-Location

$dist = "$proj\dist\ps4"
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item "$pkg\$contentId.pkg" $dist -Force
$f = Get-Item "$dist\$contentId.pkg"
Write-Host ("Built: {0}  ({1:N1} MB)" -f $f.FullName, ($f.Length / 1MB))

