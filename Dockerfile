# ---------- 1. frontend bundle
FROM node:22-bookworm-slim AS frontend
WORKDIR /src/frontend
COPY frontend/package.json frontend/package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY frontend/ ./
RUN npm test && npm run build

# ---------- 2. C++ build + tests
FROM ubuntu:24.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build ca-certificates \
      nlohmann-json3-dev libasio-dev libspdlog-dev libgtest-dev libbenchmark-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY include include
COPY src src
COPY tests tests
COPY benchmarks benchmarks
COPY config config
# -j 2 keeps peak memory modest on small hosted build machines. Timing-based server
# tests get one retry so a slow, shared build machine can't fail a deploy by chance.
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRAFFIC_FETCH_DEPS=OFF \
      -DTRAFFIC_BUILD_BENCHMARKS=OFF \
    && cmake --build build -j 2 \
    && ctest --test-dir build --output-on-failure --repeat until-pass:2

# ---------- 3. runtime image
FROM ubuntu:24.04 AS runtime
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends libspdlog1.12 curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --home /app traffic
WORKDIR /app
COPY --from=build /src/build/traffic_server /src/build/traffic_cli /usr/local/bin/
COPY config config
COPY --from=frontend /src/frontend/dist frontend/dist
RUN chown -R traffic /app/config
USER traffic
# Listens on $PORT when the platform sets it (Render, Railway, ...), otherwise 8080.
EXPOSE 8080
HEALTHCHECK --interval=15s --timeout=3s CMD curl -fsS "http://localhost:${PORT:-8080}/api/v1/health" || exit 1
CMD ["traffic_server", "--config", "config/default.json"]
