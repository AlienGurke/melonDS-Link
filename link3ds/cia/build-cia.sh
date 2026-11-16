#!/bin/sh
# Builds melonDS-Link.cia from the .elf produced by `make`.
# Needs makerom and bannertool; if they are not on PATH they are downloaded
# from their GitHub releases (Linux x86_64).
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ELF="${1:-$HERE/../melonDS-Link.elf}"
OUT="${2:-$HERE/../melonDS-Link.cia}"
TOOLS="$HERE/.tools"
mkdir -p "$TOOLS"
export PATH="$TOOLS:$PATH"

fetch_latest() { # repo, grep pattern for the asset URL
    curl -fsSL "https://api.github.com/repos/$1/releases/latest" \
        | grep -o '"browser_download_url": *"[^"]*"' | cut -d'"' -f4 | grep -i -E "$2" | head -n1
}

if ! command -v makerom >/dev/null 2>&1; then
    URL="$(fetch_latest 3DSGuy/Project_CTR 'makerom.*(ubuntu|linux).*x86_64.*\.zip$')"
    echo "makerom: $URL"
    curl -fsSL -o "$TOOLS/makerom.zip" "$URL"
    unzip -o -q "$TOOLS/makerom.zip" -d "$TOOLS/makerom-x"
    cp "$(find "$TOOLS/makerom-x" -type f -name makerom | head -n1)" "$TOOLS/makerom"
    chmod +x "$TOOLS/makerom"
fi

if ! command -v bannertool >/dev/null 2>&1; then
    URL="$(fetch_latest Steveice10/bannertool 'bannertool.*\.zip$')"
    echo "bannertool: $URL"
    curl -fsSL -o "$TOOLS/bannertool.zip" "$URL"
    unzip -o -q "$TOOLS/bannertool.zip" -d "$TOOLS/bannertool-x"
    cp "$(find "$TOOLS/bannertool-x" -type f -name bannertool -path '*linux*x86_64*' | head -n1)" "$TOOLS/bannertool"
    chmod +x "$TOOLS/bannertool"
fi

bannertool makebanner -i "$HERE/banner.png" -a "$HERE/banner.wav" -o "$TOOLS/banner.bnr"
bannertool makesmdh -s "melonDS Link" -l "3DS als Controller fuer melonDS" -p "AlienGurke" \
    -i "$HERE/icon.png" -o "$TOOLS/icon.icn"

makerom -f cia -o "$OUT" -elf "$ELF" -rsf "$HERE/app.rsf" \
    -icon "$TOOLS/icon.icn" -banner "$TOOLS/banner.bnr" -target t -exefslogo \
    -DAPP_TITLE="melonDS Link" \
    -DAPP_PRODUCT_CODE="CTR-H-MDLK" \
    -DAPP_UNIQUE_ID=0xF4D5A \
    -DAPP_CATEGORY=Application \
    -DAPP_USE_ON_SD=true \
    -DAPP_ENCRYPTED=false \
    -DAPP_MEMORY_TYPE=Application \
    -DAPP_SYSTEM_MODE=64MB \
    -DAPP_SYSTEM_MODE_EXT=Legacy \
    -DAPP_CPU_SPEED=804MHz \
    -DAPP_ENABLE_L2_CACHE=true \
    -DAPP_VERSION_MAJOR=1

echo "built $OUT"
