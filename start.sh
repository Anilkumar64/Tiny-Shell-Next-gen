#!/usr/bin/env bash
# TinyShell — start.sh
# Builds and launches all services. No database required.
# Usage:  sudo bash start.sh
set -eo pipefail

# ── Load .env if present ──────────────────────────────────────────────────────
if [[ -f "$HOME/.env_tinyshell" ]]; then
    set -a
    source "$HOME/.env_tinyshell"
    set +a
    echo "[TinyShell] Loaded env from ~/.env_tinyshell"
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

# ── Config ─────────────────────────────────────────────────────────────────────
API_TOKEN="${TSH_API_TOKEN:-}"
API_PORT="${TSH_API_PORT:-8080}"
TSH_PORT="${TSH_PORT:-4444}"
WORKER_PORT="${TSH_WORKER_PORT:-5555}"
WORKERS="${TSH_WORKERS:-worker-1@127.0.0.1:$WORKER_PORT}"

SPINE_CONTROL_PORT="${TSH_SPINE_CONTROL_PORT:-7443}"  # GUI / client connects here
SPINE_AGENT_PORT="${TSH_SPINE_AGENT_PORT:-7444}"       # agent connects here (DIFFERENT)

# BUG FIX: SPINE_TARGET for the agent must point to the AGENT port (7444),
# not the control-plane port (7443). The original had both pointing to 7443,
# which meant the agent's grpc::Connect call always failed silently and no
# jobs could ever be dispatched.
SPINE_CONTROL_TARGET="127.0.0.1:$SPINE_CONTROL_PORT"  # for the GUI
SPINE_AGENT_TARGET="127.0.0.1:$SPINE_AGENT_PORT"       # for the agent binary

JOB_SIGNING_KEY="${TSH_JOB_SIGNING_KEY:-}"
JOB_KEY_ID="${TSH_JOB_KEY_ID:-local-hmac-v1}"

# Default to WAL-only (ephemeral) mode when no Postgres DSN is provided.
# Set TSH_POSTGRES_DSN to a real DSN if you want events to survive restarts.
TSH_WAL_ONLY_MODE="${TSH_WAL_ONLY_MODE:-1}"

if [[ -z "$API_TOKEN" || "$API_TOKEN" == "dev-token-1234" ]]; then
    echo "ERROR: Default dev token detected. Set a real token." >&2
    exit 1
fi
if [[ -z "$JOB_SIGNING_KEY" ]]; then
    echo "ERROR: TSH_JOB_SIGNING_KEY is required. Run ./generate_secrets.sh" >&2
    exit 1
fi
if [[ -z "${TSH_TOFU_AUTO_TRUST:-}" ]]; then
    export TSH_TOFU_AUTO_TRUST=0
fi

TLS_CERT="${TSH_TLS_CERT:-$SCRIPT_DIR/certs/server.crt}"
TLS_KEY="${TSH_TLS_KEY:-$SCRIPT_DIR/certs/server.key}"
if [[ ! -f "$TLS_CERT" || ! -f "$TLS_KEY" ]]; then
    # BUG: the API previously started as plaintext HTTP and exposed bearer tokens.
    # FIX: generate a local TLS certificate before any API listener starts.
    mkdir -p "$(dirname "$TLS_CERT")"
    openssl req -x509 -newkey rsa:4096 -keyout "$TLS_KEY" \
      -out "$TLS_CERT" -days 365 -nodes -subj "/CN=tinyshell-server"
    chmod 600 "$TLS_KEY"
fi

# ── ZK secret (stable across restarts) ───────────────────────────────────────
ZK_SECRET_FILE="${HOME}/.tsh/zk_secret"
if [[ -n "${TSH_ZK_SECRET:-}" ]]; then
    ZK_SECRET="$TSH_ZK_SECRET"
elif [[ -f "$ZK_SECRET_FILE" ]]; then
    ZK_SECRET="$(cat "$ZK_SECRET_FILE")"
else
    mkdir -p "$(dirname "$ZK_SECRET_FILE")"
    ZK_SECRET="$(openssl rand -hex 32)"
    printf '%s' "$ZK_SECRET" > "$ZK_SECRET_FILE"
    chmod 600 "$ZK_SECRET_FILE"
    echo "[TinyShell] Generated new ZK secret → $ZK_SECRET_FILE"
fi

# ── Build ─────────────────────────────────────────────────────────────────────
echo "[TinyShell] Configuring..."
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=/usr/lib/x86_64-linux-gnu/cmake/Qt6 \
      -DBUILD_TESTING=OFF 2>&1 | tee /tmp/tsh_cmake.log | tail -5

echo "[TinyShell] Building..."
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_PREFIX_PATH=/usr/lib/x86_64-linux-gnu/cmake/Qt6 \
      -DBUILD_TESTING=ON 2>&1 | tee /tmp/tsh_cmake.log | tail -5

cmake --build "$BUILD_DIR" -j"$(nproc)" \
      --target tsh_server tsh_worker tsh_spine_server tsh_spine_agent tsh_gui tsh_server_gui tsh_tests

echo "[TinyShell] Running tests..."
"$BUILD_DIR/tests/tsh_tests" 2>&1 | tee /tmp/tsh_tests.log
echo "[TinyShell] Tests done. Log at /tmp/tsh_tests.log"

# ── Capabilities (bind any port, no permanent root needed) ───────────────────
for bin in "$BUILD_DIR/tsh_server" "$BUILD_DIR/tsh_worker" \
           "$BUILD_DIR/tsh_spine_server" "$BUILD_DIR/tsh_spine_agent"; do
    [[ -f "$bin" ]] && sudo setcap 'cap_net_bind_service=+ep' "$bin" 2>/dev/null || true
done

# ── Worker ────────────────────────────────────────────────────────────────────
echo "[TinyShell] Starting worker on :$WORKER_PORT..."
TSH_WORKER_BIND_ADDR=127.0.0.1 TSH_WORKER_PORT=$WORKER_PORT \
    "$BUILD_DIR/tsh_worker" &
WORKER_PID=$!
for i in $(seq 1 40); do
    nc -z 127.0.0.1 "$WORKER_PORT" 2>/dev/null && echo "[TinyShell] Worker ready." && break
    sleep 0.5
done

# ── Spine server ──────────────────────────────────────────────────────────────
echo "[TinyShell] Starting spine gRPC server..."
echo "             control-plane port : $SPINE_CONTROL_PORT  (GUI connects here)"
echo "             agent-connector port: $SPINE_AGENT_PORT   (agent connects here)"
TSH_GRPC_INSECURE_DEV=1 \
TSH_I_KNOW_THIS_IS_INSECURE=1 \
TSH_SPINE_LISTEN_ADDR="0.0.0.0:$SPINE_CONTROL_PORT" \
TSH_SPINE_AGENT_LISTEN_ADDR="0.0.0.0:$SPINE_AGENT_PORT" \
TSH_JOB_SIGNING_KEY="$JOB_SIGNING_KEY" \
TSH_JOB_KEY_ID="$JOB_KEY_ID" \
TSH_WAL_ONLY_MODE="$TSH_WAL_ONLY_MODE" \
    "$BUILD_DIR/tsh_spine_server" &
SPINE_SERVER_PID=$!
for i in $(seq 1 40); do
    nc -z 127.0.0.1 "$SPINE_CONTROL_PORT" 2>/dev/null && \
    nc -z 127.0.0.1 "$SPINE_AGENT_PORT"   2>/dev/null && \
    echo "[TinyShell] Spine server ready." && break
    sleep 0.5
done

# ── Spine agent ───────────────────────────────────────────────────────────────
# BUG FIX: connect to the AGENT port (7444), not the control-plane port (7443).
echo "[TinyShell] Starting spine agent → $SPINE_AGENT_TARGET..."
TSH_GRPC_INSECURE_DEV=1 \
TSH_I_KNOW_THIS_IS_INSECURE=1 \
TSH_SPINE_TARGET="$SPINE_AGENT_TARGET" \
TSH_JOB_SIGNING_KEY="$JOB_SIGNING_KEY" \
TSH_JOB_KEY_ID="$JOB_KEY_ID" \
TSH_ALLOW_ROOT_AGENT_EXEC=1 \
    "$BUILD_DIR/tsh_spine_agent" &
SPINE_AGENT_PID=$!

# ── Main server ───────────────────────────────────────────────────────────────
echo "[TinyShell] Starting main server on :$TSH_PORT (API :$API_PORT)..."
TSH_PORT=$TSH_PORT \
TSH_API_PORT=$API_PORT \
TSH_API_TOKEN=$API_TOKEN \
TSH_TLS_CERT="$TLS_CERT" \
TSH_TLS_KEY="$TLS_KEY" \
TSH_ZK_SECRET=$ZK_SECRET \
TSH_WORKERS="$WORKERS" \
TSH_TOFU_AUTO_TRUST="$TSH_TOFU_AUTO_TRUST" \
TSH_I_KNOW_THIS_IS_INSECURE=1 \
TSH_JOB_SIGNING_KEY="$JOB_SIGNING_KEY" \
TSH_SPINE_CONTROL_ADDR="$SPINE_CONTROL_TARGET" \
TSH_SPINE_TARGET="$SPINE_CONTROL_TARGET" \
TSH_ADMIN_TOKEN="${TSH_ADMIN_TOKEN:-}" \
TSH_VIEWER_TOKEN="${TSH_VIEWER_TOKEN:-}" \
    "$BUILD_DIR/tsh_server" &
SERVER_PID=$!

trap 'echo "[TinyShell] Shutting down...";
      kill "$SERVER_PID" "$WORKER_PID" "$SPINE_SERVER_PID" "$SPINE_AGENT_PID" "$SERVER_GUI_PID" 2>/dev/null;
      wait "$SERVER_PID" "$WORKER_PID" "$SPINE_SERVER_PID" "$SPINE_AGENT_PID" 2>/dev/null;
      echo "[TinyShell] Done."' EXIT

sleep 2

# ── GUI (both server and client) ──────────────────────────────────────────────
echo "[TinyShell] Starting Server GUI..."
env -i \
    HOME="$HOME" \
    PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
    DISPLAY="${DISPLAY:-}" \
    XAUTHORITY="${XAUTHORITY:-}" \
    WAYLAND_DISPLAY="" \
    QT_QPA_PLATFORM="xcb" \
    XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-}" \
    TERM="${TERM:-xterm-256color}" \
    LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu" \
    QT_PLUGIN_PATH=/usr/lib/x86_64-linux-gnu/qt6/plugins \
    TSH_GRPC_INSECURE_DEV=1 \
    TSH_I_KNOW_THIS_IS_INSECURE=1 \
    TSH_SPINE_TARGET="$SPINE_CONTROL_TARGET" \
    "$BUILD_DIR/gui/tsh_server_gui" \
        --url "https://127.0.0.1:$API_PORT" \
        --token "$API_TOKEN" \
    2>&1 | tee /tmp/tsh_server_gui.log &
SERVER_GUI_PID=$!

echo "[TinyShell] Starting Client GUI..."
env -i \
    HOME="$HOME" \
    PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
    DISPLAY="${DISPLAY:-}" \
    XAUTHORITY="${XAUTHORITY:-}" \
    WAYLAND_DISPLAY="" \
    QT_QPA_PLATFORM="xcb" \
    XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-}" \
    TERM="${TERM:-xterm-256color}" \
    LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu" \
    QT_PLUGIN_PATH=/usr/lib/x86_64-linux-gnu/qt6/plugins \
    TSH_GRPC_INSECURE_DEV=1 \
    TSH_I_KNOW_THIS_IS_INSECURE=1 \
    TSH_SPINE_TARGET="$SPINE_CONTROL_TARGET" \
    "$BUILD_DIR/gui/tsh_gui" \
        --url "https://127.0.0.1:$API_PORT" \
        --token "$API_TOKEN" \
    2>&1 | tee /tmp/tsh_client_gui.log

wait $SERVER_GUI_PID
