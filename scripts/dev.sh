#!/usr/bin/env bash
# Builds the engine/server (release), then runs the C++ server and the Vite dev server
# together. Ctrl-C stops both.
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset release >/dev/null
cmake --build --preset release -j "$(nproc 2>/dev/null || echo 2)" --target traffic_server traffic_cli

(cd frontend && [ -d node_modules ] || npm ci --no-audit --no-fund)

./build/release/traffic_server --config config/default.json --static-dir frontend/dist &
SERVER=$!
trap 'kill $SERVER 2>/dev/null' EXIT
echo "API on http://localhost:8080 — UI with hot reload on http://localhost:5173"
cd frontend && npm run dev
