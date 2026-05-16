#!/usr/bin/env bash
# TinyShell — run-clean.sh
# Checks deps, builds, and launches. No database needed.
set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
G='\033[0;32m'; Y='\033[1;33m'; R='\033[0;31m'; N='\033[0m'
ok()  { echo -e "${G}[✓]${N} $1"; }
err() { echo -e "${R}[✗]${N} $1"; }

echo "╔══════════════════════════════════════════╗"
echo "║   TinyShell — Setup & Launch             ║"
echo "╚══════════════════════════════════════════╝"

# Step 1 — build deps
echo -e "\nStep 1: Checking build dependencies..."
MISSING=()
for cmd in cmake make g++ openssl nc; do
    command -v "$cmd" &>/dev/null && ok "$cmd" || MISSING+=("$cmd")
done
if [[ ${#MISSING[@]} -gt 0 ]]; then
    err "Missing: ${MISSING[*]}"
    echo "  sudo apt install -y build-essential cmake libssl-dev netcat-openbsd"
    exit 1
fi

# Step 2 — sudo (needed for setcap)
echo -e "\nStep 2: Checking sudo access..."
if ! sudo -v 2>/dev/null; then
    err "sudo required. Run: sudo bash run-clean.sh"
    exit 1
fi
ok "sudo available"

# Step 3 — build
echo -e "\nStep 3: Building TinyShell..."
mkdir -p "$SCRIPT_DIR/build"
cd "$SCRIPT_DIR"
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF 2>&1 | tail -3 && ok "cmake done"
cmake --build build -j"$(nproc)" \
      --target tsh_server tsh_worker tsh_spine_server tsh_spine_agent tsh_gui tsh_server_gui \
      2>&1 | tail -5 && ok "build done"

# Step 4 — launch
echo -e "\nStep 4: Launching...\n"
echo "╔══════════════════════════════════════════╗"
echo "║   Press Ctrl+C to stop all services      ║"
echo "╚══════════════════════════════════════════╝"
chmod +x "$SCRIPT_DIR/start.sh"
sudo --preserve-env=HOME,DISPLAY,WAYLAND_DISPLAY,XAUTHORITY,XDG_RUNTIME_DIR,TERM \
     bash "$SCRIPT_DIR/start.sh"