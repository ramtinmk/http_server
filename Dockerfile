# syntax=docker/dockerfile:1

# --- Stage 1: build ---------------------------------------------------------
# A pinned Alpine release keeps the toolchain reproducible. `build-base` pulls
# in gcc, make, and the libc headers; `zlib-dev` and `openssl-dev` satisfy the
# REQUIRED CMake packages (`find_package(ZLIB)` and `find_package(OpenSSL)`).
FROM alpine:3.20 AS builder

RUN apk add --no-cache \
        build-base \
        cmake \
        pkgconfig \
        python3 \
        linux-headers \
        zlib-dev \
        openssl-dev

WORKDIR /app

# Copy the whole (dockerignored) tree. CMake needs CMakeLists.txt, src/,
# include/, cmake/, root/, and http_server.conf.
COPY . .

# Explicit out-of-source build. The runtime artifacts land in ./bin at the
# source root because CMakeLists.txt sets CMAKE_RUNTIME_OUTPUT_DIRECTORY to
# ${CMAKE_SOURCE_DIR}/bin (so the documented ./bin/http_server path holds in
# and out of the container).
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel "$(nproc)"

# --- Stage 2: runtime -------------------------------------------------------
# Only the shared libraries the binary needs; no compiler, headers, or build
# tree in the final image.
FROM alpine:3.20 AS runtime

RUN apk add --no-cache libssl3 libcrypto3 zlib libgcc \
    && addgroup -S http-server \
    && adduser -S -G http-server -H -s /sbin/nologin http-server

WORKDIR /app

# The binary, the document-root assets, and the default config. The config is
# baked in as DEFAULT_CONFIG_FILE="/app/http_server.conf" and sets
# `document_root = root`, which resolves relative to this working directory.
COPY --from=builder /app/bin/http_server /usr/local/bin/http_server
COPY --from=builder /app/root/ ./root/
COPY --from=builder /app/http_server.conf ./http_server.conf

USER http-server

EXPOSE 8081

# `/home` is served from the startup-cached assets on the default listener, so
# it is a dependency-free liveness check (busybox `wget`).
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD wget -q -O /dev/null http://127.0.0.1:8081/home || exit 1

CMD ["http_server"]
