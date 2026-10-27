#!/usr/bin/env bash
# Builds Nexus as a native PS5 title (home screen icon, launched by the system like any app),
# the way the PS5 homebrew already working on the console is built: payload-SDK objects linked
# as a PIE, converted by ps5-native-tool (SCE dynamic tables + process parameters), signed as a
# fake SELF, shipped with the generated sce_module/libc.prx. Tooling: third_party/ps5-native-app-boilerplate
# (GPL-3.0-or-later, BlackBearReloaded, via the Snes9x PS5 port).
# Runs inside WSL. Usage (from Windows):
#   wsl -d Ubuntu -- bash tools/build-ps5-native.sh            # build only
#   wsl -d Ubuntu -- bash tools/build-ps5-native.sh --upload   # build + FTP upload to the console
set -euo pipefail

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-$HOME/ps5dev/ps5-payload-sdk}"
CONSOLE="${CONSOLE:-192.168.100.22}"
FTP_PORT="${FTP_PORT:-2121}"
TITLE_ID="${TITLE_ID:-PPSA99784}"
CONTENT_ID="UP9000-${TITLE_ID}_00-NEXUSDASH0000000"
VERSION="01.000.000"

NAT="$PROJ/third_party/ps5-native-app-boilerplate"
T="$NAT/native"
SDK_LIB="$PS5_PAYLOAD_SDK/target/lib"
OUT="$PROJ/build/ps5native"
HOST="$OUT/host"
OBJ="$OUT/obj"
PKG="$OUT/$TITLE_ID"

rm -rf "$OBJ" "$PKG"
mkdir -p "$HOST" "$OBJ" "$PKG/sce_sys" "$PKG/sce_module" "$PKG/assets/fonts" "$PKG/assets/trophies"

# ---- host tools (rebuilt only when missing)
if [ ! -x "$HOST/ps5-native-tool" ]; then
    echo "build ps5-native-tool"
    clang++-18 -std=c++20 -O2 -o "$HOST/ps5-native-tool" \
        "$T/native_app_builder.cpp" "$T/elf_object.cpp" "$T/self_container.cpp" "$T/sce_module_writer.cpp" -lz
fi
if [ ! -x "$HOST/libc-builder" ]; then
    echo "build libc-builder"
    clang++-18 -std=c++20 -O2 -Wall -Wextra -o "$HOST/libc-builder" "$T/libc_builder.cpp"
fi
TOOL="$HOST/ps5-native-tool"

# ---- sce_module/libc.prx: generated and signed, must match the published digest
if [ ! -f "$HOST/libc.prx" ]; then
    echo "generate libc.prx"
    "$HOST/libc-builder" "$T/runtime/api-surface.txt" "$T/runtime/imports.txt" "$HOST/libc.raw.elf"
    "$TOOL" self --sign --in "$HOST/libc.raw.elf" --out "$HOST/libc.prx"
    (cd "$HOST" && sha256sum --check --strict "$NAT/libc.prx.sha256")
fi

# ---- compile
CXX="$PS5_PAYLOAD_SDK/bin/prospero-clang++"
FLAGS=(-std=gnu++17 -O2 -fPIC -march=znver2 -fno-strict-aliasing
       -DNEXUS_PS5=1 -DNEXUS_PS5_APP=1 -DNEXUS_NO_PNG_WRITE=1 "-DNEXUS_TITLE_ID=\"$TITLE_ID\""
       -I"$PROJ/src")
SOURCES=(src/ui/app.cpp src/ui/gfx.cpp src/ui/brand.cpp src/ui/stb_impl.cpp
         src/core/sfx_synth.cpp src/core/music_synth.cpp src/core/mixer.cpp src/core/sqlite_read.cpp
         src/platform/console_library.cpp src/platform/ps5/ps5_tilemap.cpp
         src/platform/ps5/ps5_platform.cpp src/platform/ps5/ps5_main.cpp)
OBJS=()
for s in "${SOURCES[@]}"; do
    o="$OBJ/$(basename "${s%.*}").o"
    echo "compile $s"
    "$CXX" "${FLAGS[@]}" -c -o "$o" "$PROJ/$s"
    OBJS+=("$o")
done
for s in app_crt app_cpp_runtime; do
    echo "compile $s"
    PS5_PAYLOAD_SDK="$PS5_PAYLOAD_SDK" sh "$NAT/prospero-clang18" -std=c++20 -O2 -fno-exceptions -fno-rtti \
        -ffunction-sections -fdata-sections -c -o "$OBJ/$s.o" "$T/$s.cpp"
done

# ---- link as a PIE against the SDK's system-module stubs, then convert and sign
STUBS=()
for f in "$SDK_LIB"/*.so; do
    case "$f" in */libScePosixForWebKit.so|*/libSceGLSlimVSH.so) ;; *) STUBS+=("$f") ;; esac
done
echo "link"
ld.lld-18 -m elf_x86_64 -pie -z max-page-size=0x4000 --hash-style=gnu \
    -T "$T/ps5-pie.ld" -T "$T/ehframe.ld" --eh-frame-hdr \
    --version-script "$T/app-symbols.map" --version-script "$T/localize.map" \
    -e _start --no-dynamic-linker -o "$OUT/nexus-pie.elf" \
    "$OBJ/app_crt.o" "$OBJ/app_cpp_runtime.o" "${OBJS[@]}" \
    --as-needed "${STUBS[@]}" -L "$SDK_LIB" \
    "$SDK_LIB/libc++.a" "$SDK_LIB/libc++abi.a" "$SDK_LIB/libunwind.a" "$SDK_LIB/libc.a" -lpthread
echo "convert"
"$TOOL" link --in "$OUT/nexus-pie.elf" --out "$OUT/eboot.elf" --stub-dir "$SDK_LIB" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
# Both the eboot and libc.prx must be signed: the console refuses unsigned eboots (ENOEXEC), and a
# signed eboot cannot load an unsigned libc.prx (rtld error 46). Note the console's FTP server
# decrypts SELF files on the fly, so files downloaded from it look unsigned even when they are not.
"$TOOL" self --sign --in "$OUT/eboot.elf" --out "$PKG/eboot.bin" --magic 0x1D3D154F

# ---- launcher payload: Nexus sends it to the ELF loader to start games (see launcher.c)
echo "build launcher payload"
"$PS5_PAYLOAD_SDK/bin/prospero-clang" -O2 -Wall -o "$PKG/assets/launcher.elf" \
    "$PROJ/tools/ps5-launcher/launcher.c" -lSceSystemService -lSceUserService

# ---- app folder
cp "$HOST/libc.prx" "$PKG/sce_module/"
cp "$PROJ/assets/sce_sys/icon0.png" "$PKG/sce_sys/"
cp "$PROJ"/assets/fonts/*.ttf "$PKG/assets/fonts/"
cp "$PROJ"/assets/trophies/*.png "$PKG/assets/trophies/"
# Registered as a non-game application (category 65536, with the attribute value YouTube and
# Trophy Manager use) rather than a game (category 0, attribute 0): the system only passes PS4
# controllers (DualShock 4 and compatible pads) to PS4 games and to applications, never to
# PS5 games. APP_TYPE=game builds the old way.
if [ "${APP_TYPE:-app}" = "game" ]; then CATEGORY=0; ATTRIBUTE=0; else CATEGORY=65536; ATTRIBUTE=1644167168; fi
cat > "$PKG/sce_sys/param.json" <<EOF
{
  "ageLevel": { "default": 0 },
  "applicationCategoryType": $CATEGORY,
  "applicationDrmType": "free",
  "attribute": $ATTRIBUTE,
  "attribute2": 0,
  "attribute3": 0,
  "conceptId": "${TITLE_ID#PPSA}",
  "contentBadgeType": 1,
  "contentId": "$CONTENT_ID",
  "contentVersion": "$VERSION",
  "downloadDataSize": 256,
  "gameIntent": { "permittedIntents": [ { "intentType": "launchActivity" } ] },
  "localizedParameters": {
    "defaultLanguage": "en-US",
    "en-US": { "titleName": "Nexus" }
  },
  "masterVersion": "01.00",
  "pubtools": { "creationDate": "2026-10-26 00:00:00", "loudnessSnd0": "-28.00", "toolVersion": "2.00" },
  "requiredSystemSoftwareVersion": "0x0000000000000000",
  "sdkVersion": "0x0000000000000000",
  "titleId": "$TITLE_ID",
  "versionFileUri": ""
}
EOF
echo "Built: $PKG ($(stat -c %s "$PKG/eboot.bin") byte eboot.bin)"

if [ "${1:-}" = "--upload" ]; then
    base="ftp://$CONSOLE:$FTP_PORT/data/homebrew/$TITLE_ID"
    (cd "$PKG" && find . -type f | sed 's|^\./||' | while read -r rel; do
        echo "upload $rel"
        curl -s --ftp-create-dirs -T "$rel" "$base/$rel"
    done)
    echo "Uploaded to /data/homebrew/$TITLE_ID. ShadowMountPlus registers it within about 30 seconds."
fi
