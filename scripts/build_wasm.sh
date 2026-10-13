#!/usr/bin/env bash
# Compiles the C++ engine to WebAssembly for the serverless (static) browser build.
#
#   scripts/build_wasm.sh            # writes frontend/src/wasm/traffic-engine.js
#
# Requires Emscripten (em++ on PATH; https://emscripten.org/docs/getting_started) and
# nlohmann/json headers (found in /usr/include, $NLOHMANN_JSON_INCLUDE, or downloaded).
# The generated file is committed, so building or deploying the frontend does NOT need
# Emscripten — only changes to the C++ engine do.
set -euo pipefail
cd "$(dirname "$0")/.."

command -v em++ >/dev/null || { echo "em++ not found: install Emscripten first" >&2; exit 1; }

INC="${NLOHMANN_JSON_INCLUDE:-}"
if [ -z "$INC" ]; then
  if [ -f /usr/include/nlohmann/json.hpp ]; then
    mkdir -p build/wasm/include && ln -sfn /usr/include/nlohmann build/wasm/include/nlohmann
  elif [ ! -f build/wasm/include/nlohmann/json.hpp ]; then
    mkdir -p build/wasm/include/nlohmann
    curl -fsSL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp \
      -o build/wasm/include/nlohmann/json.hpp
  fi
  INC=build/wasm/include
fi

SOURCES=(
  src/core/*.cpp src/network/*.cpp src/vehicles/*.cpp src/signals/*.cpp
  src/pedestrians/*.cpp src/controllers/*.cpp src/metrics/*.cpp src/experiments/*.cpp
  src/infrastructure/scenario_io.cpp src/infrastructure/snapshot_json.cpp
  src/infrastructure/command_parser.cpp src/infrastructure/experiment_io.cpp
  src/infrastructure/simulation_session.cpp
  src/wasm/wasm_bindings.cpp
)

mkdir -p frontend/src/wasm
em++ -std=c++20 -O3 -Iinclude -isystem "$INC" -fexceptions \
  "${SOURCES[@]}" \
  -lembind \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createTrafficEngine \
  -sENVIRONMENT=web,worker -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 \
  -sSINGLE_FILE=1 -sFILESYSTEM=0 -sASSERTIONS=0 \
  -o frontend/src/wasm/traffic-engine.js

ls -l frontend/src/wasm/traffic-engine.js
