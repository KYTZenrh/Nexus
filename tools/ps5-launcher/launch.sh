#!/usr/bin/env bash
# Builds the launcher payload for a title and sends it to the console's ELF loader.
# Usage (WSL): bash tools/ps5-launcher/launch.sh [TITLE_ID] [--kill]
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-$HOME/ps5dev/ps5-payload-sdk}"
TITLE="${1:-PPSA99780}"
CONSOLE="${CONSOLE:-192.168.100.22}"
OUT="$DIR/../../build/launcher"
mkdir -p "$OUT"
EXTRA=()
[ "${2:-}" = "--kill" ] && EXTRA=(-DKILL_ONLY)
"$PS5_PAYLOAD_SDK/bin/prospero-clang" -O2 -Wall "${EXTRA[@]}" -DTITLE_ID="\"$TITLE\"" -o "$OUT/launcher.elf" "$DIR/launcher.c" \
    -lSceSystemService -lSceUserService
python3 - "$OUT/launcher.elf" "$CONSOLE" <<'EOF'
import socket, sys
data = open(sys.argv[1], 'rb').read()
s = socket.create_connection((sys.argv[2], 9021), timeout=10)
s.sendall(data)
s.close()
print(f"sent {len(data)} bytes")
EOF
