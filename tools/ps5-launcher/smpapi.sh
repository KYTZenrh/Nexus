#!/usr/bin/env bash
# Sends one ShadowMountPlus API request from the console itself; the response lands in
# /data/smp_api.txt. Usage (WSL): bash tools/ps5-launcher/smpapi.sh /api/v1/games '{}'
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-$HOME/ps5dev/ps5-payload-sdk}"
CONSOLE="${CONSOLE:-192.168.100.22}"
OUT="$DIR/../../build/launcher"
mkdir -p "$OUT"
BODY="${2:-{\}}"
BODY_ESC=$(printf '%s' "$BODY" | sed 's/"/\\"/g')
"$PS5_PAYLOAD_SDK/bin/prospero-clang" -O2 -Wall -DAPI_PATH="\"$1\"" -DAPI_BODY="\"$BODY_ESC\"" \
    -o "$OUT/smpapi.elf" "$DIR/smpapi.c"
python3 - "$OUT/smpapi.elf" "$CONSOLE" <<'EOF'
import socket, sys
data = open(sys.argv[1], 'rb').read()
s = socket.create_connection((sys.argv[2], 9021), timeout=10)
s.sendall(data); s.close()
EOF
sleep 3
curl -s "ftp://$CONSOLE:2121/data/smp_api.txt"
