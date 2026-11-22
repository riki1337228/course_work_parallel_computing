# syntax=docker/dockerfile:1
# =============================================================================
# Multi-stage build of the C++20 search backend.
#   deps     - toolchain + Drogon (built from source, pinned tag) + libraries
#   build    - compiles search_server, CLI `search` and all GoogleTest suites
#   test     - runs unit + integration + concurrency tests (ctest)
#   tsan     - core library + unit/concurrency tests under ThreadSanitizer
#   runtime  - minimal image used by docker compose (search_server + search CLI)
# =============================================================================

FROM ubuntu:24.04 AS deps
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates \
        libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev \
        nlohmann-json3-dev libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

ARG DROGON_VERSION=v1.9.11
RUN git clone --depth 1 --branch ${DROGON_VERSION} --recurse-submodules --shallow-submodules \
        https://github.com/drogonframework/drogon.git /tmp/drogon \
    && cmake -S /tmp/drogon -B /tmp/drogon/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_EXAMPLES=OFF -DBUILD_CTL=OFF -DBUILD_ORM=OFF \
        -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=OFF \
    && cmake --build /tmp/drogon/build \
    && cmake --install /tmp/drogon/build \
    && rm -rf /tmp/drogon

# -----------------------------------------------------------------------------
FROM deps AS build
WORKDIR /src
COPY CMakeLists.txt ./
COPY backend ./backend
COPY cli ./cli
COPY tests ./tests
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON \
    && cmake --build build

# -----------------------------------------------------------------------------
FROM build AS test
WORKDIR /src
CMD ["ctest", "--test-dir", "build", "--output-on-failure"]

# -----------------------------------------------------------------------------
FROM deps AS tsan
WORKDIR /src
COPY CMakeLists.txt ./
COPY backend ./backend
COPY cli ./cli
COPY tests ./tests
RUN cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
        -DBUILD_API=OFF -DBUILD_TESTS=ON -DENABLE_TSAN=ON \
    && cmake --build build-tsan
ENV TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1"
# GCC 13 TSan cannot cope with the high ASLR entropy of recent kernels, so ASLR is
# disabled for the test process (requires `--security-opt seccomp=unconfined`).
CMD ["sh", "-c", "setarch $(uname -m) -R ctest --test-dir build-tsan --output-on-failure"]

# -----------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libjsoncpp25 libuuid1 zlib1g libssl3t64 ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/backend/search_server /usr/local/bin/search_server
COPY --from=build /src/build/cli/search /usr/local/bin/search
COPY frontend /app/frontend
ENV FRONTEND_PATH=/app/frontend \
    DATASET_PATH=/data \
    SERVER_PORT=8080
EXPOSE 8080
CMD ["search_server"]
