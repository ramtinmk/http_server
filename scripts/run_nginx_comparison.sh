#!/usr/bin/env bash
# Full nginx comparison against the deterministic file-class corpus.
#
# Builds an optimized nginx from source and an optimized build of this server
# with a matched worker count, starts both (plaintext, gzip_static, and TLS),
# and drives `wrk` class by class with the servers pinned to one CPU set and
# the generator to a disjoint one. Writes benchmarks/nginx_comparison.{csv,json}.
#
# This is an on-demand comparison, not a default test: it needs network access
# (nginx tarball), `wrk`, `openssl`, and a C toolchain. See
# docs/runbooks/compare-against-nginx.md for the manual recipe and caveats.
#
# Tunables (environment variables):
#   HPNGX_WORK             scratch dir            (/tmp/http_server_nginx_compare)
#   HPNGX_NGINX_VERSION    nginx release          (1.30.5)
#   HPNGX_SERVER_CPUS      server CPU set         (0-3)
#   HPNGX_CLIENT_CPUS      wrk CPU set            (4-7)
#   HPNGX_WORKERS          worker/loop count      (4)
#   HPNGX_DURATION         seconds per run        (8)
#   HPNGX_REPEATS          runs per cell          (2)
#   HPNGX_CONNECTIONS      wrk conns, plaintext   (100)
#   HPNGX_TLS_CONNECTIONS  wrk conns, TLS         (32)
#   HPNGX_OUTPUT           CSV artifact           (benchmarks/nginx_comparison.csv)
#   HPNGX_OUTPUT_JSON      metadata artifact      (benchmarks/nginx_comparison.json)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

WORK="${HPNGX_WORK:-/tmp/http_server_nginx_compare}"
NGINX_VERSION="${HPNGX_NGINX_VERSION:-1.30.5}"
NGINX_PREFIX="$WORK/nginx-prefix"
SERVER_CPUS="${HPNGX_SERVER_CPUS:-0-3}"
CLIENT_CPUS="${HPNGX_CLIENT_CPUS:-4-7}"
WORKERS="${HPNGX_WORKERS:-4}"
DURATION="${HPNGX_DURATION:-8}"
REPEATS="${HPNGX_REPEATS:-2}"
CONNS="${HPNGX_CONNECTIONS:-100}"
TLS_CONNS="${HPNGX_TLS_CONNECTIONS:-32}"
OUT_CSV="${HPNGX_OUTPUT:-$REPO_ROOT/benchmarks/nginx_comparison.csv}"
OUT_JSON="${HPNGX_OUTPUT_JSON:-$REPO_ROOT/benchmarks/nginx_comparison.json}"

CORPUS="$REPO_ROOT/benchmarks/corpus"
CERT_DIR="$WORK/tls"
PORT_OURS=8081
PORT_OURS_TLS=8443
PORT_NGINX=8082
PORT_NGINX_GZIP=8084
PORT_NGINX_TLS=8445
OURS_BIN="$WORK/http_server_el$WORKERS"
OURS_PID=""

log() { printf '[nginx-compare] %s\n' "$*" >&2; }

require() {
    for tool in "$@"; do
        command -v "$tool" >/dev/null 2>&1 || {
            echo "missing required tool: $tool" >&2
            exit 2
        }
    done
}

cleanup() {
    if [ -n "$OURS_PID" ]; then
        kill "$OURS_PID" 2>/dev/null || true
        wait "$OURS_PID" 2>/dev/null || true
    fi
    if [ -x "$NGINX_PREFIX/sbin/nginx" ] && [ -f "$WORK/nginx.conf" ]; then
        "$NGINX_PREFIX/sbin/nginx" -c "$WORK/nginx.conf" -s stop 2>/dev/null || true
    fi
}
trap cleanup EXIT

cpu_affinity_masks() {
    python3 - "$1" "$2" <<'PY'
import sys
spec, n = sys.argv[1], int(sys.argv[2])
cpus = []
for part in spec.split(","):
    if "-" in part:
        a, b = part.split("-")
        cpus += list(range(int(a), int(b) + 1))
    elif part:
        cpus.append(int(part))
chosen = cpus[:n]
width = max(4, max(chosen) + 1) if chosen else 4
print(" ".join(format(1 << c, "b").zfill(width) for c in chosen))
PY
}

wait_probe() {
    local port="$1" timeout="${2:-15}"
    local deadline=$((SECONDS + timeout))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if curl -sk --connect-timeout 1 --max-time 2 -o /dev/null \
                "https://127.0.0.1:$port/" 2>/dev/null; then
            return 0
        fi
        if curl -s --connect-timeout 1 --max-time 2 -o /dev/null \
                "http://127.0.0.1:$port/" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

require cc make curl tar openssl python3 wrk taskset

mkdir -p "$WORK" "$CERT_DIR" benchmarks

# --- corpus ---------------------------------------------------------------
log "generating corpus at $CORPUS"
python3 scripts/benchmark_corpus.py --output-dir "$CORPUS" --with-gzip --quiet

# --- self-signed certificate ---------------------------------------------
if [ ! -f "$CERT_DIR/cert.pem" ] || [ ! -f "$CERT_DIR/key.pem" ]; then
    log "generating throwaway self-signed certificate"
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$CERT_DIR/key.pem" -out "$CERT_DIR/cert.pem" \
        -days 3650 -subj "/CN=localhost" >/dev/null 2>&1
fi

# --- build nginx ----------------------------------------------------------
if [ ! -x "$NGINX_PREFIX/sbin/nginx" ]; then
    log "building nginx $NGINX_VERSION into $NGINX_PREFIX"
    tarball="nginx-$NGINX_VERSION.tar.gz"
    ( cd "$WORK"
      [ -f "$tarball" ] || curl -sSLO "https://nginx.org/download/$tarball"
      [ -d "nginx-$NGINX_VERSION" ] || tar xzf "$tarball"
      cd "nginx-$NGINX_VERSION"
      make clean >/dev/null 2>&1 || true
      ./configure --prefix="$NGINX_PREFIX" --without-pcre \
        --without-http_rewrite_module --without-http_ssi_module \
        --without-http_geo_module --without-http_map_module \
        --without-http_split_clients_module --without-http_referer_module \
        --without-http_proxy_module --without-http_fastcgi_module \
        --without-http_uwsgi_module --without-http_scgi_module \
        --without-http_grpc_module --without-http_memcached_module \
        --without-http_empty_gif_module --without-http_autoindex_module \
        --with-http_ssl_module --with-http_gzip_static_module \
        --with-cc-opt='-O2' >/dev/null
      make -j"$(nproc)" >/dev/null
      make install >/dev/null )
fi

# --- build this server with a matched worker count -----------------------
log "building $OURS_BIN (-O2, EL_THREAD_COUNT=$WORKERS)"
cc -O2 -DNDEBUG -DEL_THREAD_COUNT="$WORKERS" \
   -DDEFAULT_CONFIG_FILE="\"$REPO_ROOT/http_server.conf\"" \
   -I include -std=gnu11 src/*.c -o "$OURS_BIN" \
   -lpthread -lz -lssl -lcrypto

# --- render nginx config --------------------------------------------------
AFFINITY="$(cpu_affinity_masks "$SERVER_CPUS" "$WORKERS")"
sed \
    -e "s|@WORKERS@|$WORKERS|g" \
    -e "s|@CPU_AFFINITY@|$AFFINITY|g" \
    -e "s|@WORK@|$WORK|g" \
    -e "s|@NGINX_CONF_DIR@|$NGINX_PREFIX/conf|g" \
    -e "s|@CORPUS_ROOT@|$CORPUS|g" \
    -e "s|@CERT@|$CERT_DIR/cert.pem|g" \
    -e "s|@KEY@|$CERT_DIR/key.pem|g" \
    -e "s|@PORT_IDENTITY@|$PORT_NGINX|g" \
    -e "s|@PORT_GZIP@|$PORT_NGINX_GZIP|g" \
    -e "s|@PORT_TLS@|$PORT_NGINX_TLS|g" \
    scripts/nginx_reference.conf > "$WORK/nginx.conf"
"$NGINX_PREFIX/sbin/nginx" -t -c "$WORK/nginx.conf" >/dev/null 2>&1

# --- start servers --------------------------------------------------------
log "starting this server (plaintext $PORT_OURS, TLS $PORT_OURS_TLS) on CPUs $SERVER_CPUS"
env \
    HTTP_SERVER_DOCUMENT_ROOT="$CORPUS" \
    HTTP_SERVER_PORT="$PORT_OURS" \
    HTTP_SERVER_ACCESS_LOG=0 \
    HTTP_SERVER_LOG_LEVEL=error \
    HTTP_SERVER_CPU_SET="$SERVER_CPUS" \
    HTTP_SERVER_MAX_KEEPALIVE_REQUESTS=1000000 \
    HTTP_SERVER_TLS=1 \
    HTTP_SERVER_TLS_PORT="$PORT_OURS_TLS" \
    HTTP_SERVER_TLS_CERT="$CERT_DIR/cert.pem" \
    HTTP_SERVER_TLS_KEY="$CERT_DIR/key.pem" \
    taskset -c "$SERVER_CPUS" "$OURS_BIN" >"$WORK/our.log" 2>&1 &
OURS_PID=$!

log "starting nginx on CPUs $SERVER_CPUS"
"$NGINX_PREFIX/sbin/nginx" -c "$WORK/nginx.conf" -s stop 2>/dev/null || true
"$NGINX_PREFIX/sbin/nginx" -c "$WORK/nginx.conf"

for port in "$PORT_OURS" "$PORT_OURS_TLS" "$PORT_NGINX" "$PORT_NGINX_GZIP" "$PORT_NGINX_TLS"; do
    wait_probe "$port" 15 || { echo "server on port $port did not become ready" >&2; exit 2; }
done
log "both servers ready; beginning wrk runs"

# --- run the matrix -------------------------------------------------------
rm -f "$OUT_CSV"
log "identity"
python3 scripts/compare_servers.py --corpus "$CORPUS" \
    --target ours="http://127.0.0.1:$PORT_OURS" \
    --target nginx="http://127.0.0.1:$PORT_NGINX" \
    --mode identity --wrk-cpus "$CLIENT_CPUS" --threads 4 \
    --connections "$CONNS" --duration "$DURATION" --repeats "$REPEATS" \
    --output "$OUT_CSV"

log "gzip-precompressed"
python3 scripts/compare_servers.py --corpus "$CORPUS" \
    --target ours="http://127.0.0.1:$PORT_OURS" \
    --target nginx="http://127.0.0.1:$PORT_NGINX_GZIP" \
    --class tiny --class small --class medium --gzip \
    --mode gzip-precompressed --wrk-cpus "$CLIENT_CPUS" --threads 4 \
    --connections "$CONNS" --duration "$DURATION" --repeats "$REPEATS" \
    --output "$OUT_CSV" --append

log "tls"
python3 scripts/compare_servers.py --corpus "$CORPUS" \
    --target ours-tls="https://127.0.0.1:$PORT_OURS_TLS" \
    --target nginx-tls="https://127.0.0.1:$PORT_NGINX_TLS" \
    --mode tls --wrk-cpus "$CLIENT_CPUS" --threads 4 \
    --connections "$TLS_CONNS" --duration "$DURATION" --repeats "$REPEATS" \
    --output "$OUT_CSV" --append

# --- metadata -------------------------------------------------------------
python3 - "$OUT_CSV" "$OUT_JSON" "$NGINX_VERSION" "$WORKERS" "$SERVER_CPUS" \
        "$CLIENT_CPUS" "$WORK" <<'PY'
import hashlib, json, os, platform, subprocess, sys
out_csv, out_json, nginx_version, workers, server_cpus, client_cpus, work = sys.argv[1:]
manifest_path = os.path.join("benchmarks", "corpus", "manifest.json")
manifest_bytes = open(manifest_path, "rb").read()

def out(cmd):
    try:
        return subprocess.check_output(cmd, text=True).strip()
    except Exception:
        return ""

cpu = ""
for line in open("/proc/cpuinfo"):
    if line.startswith("model name"):
        cpu = line.split(":", 1)[1].strip()
        break

meta = {
    "date_utc": out(["date", "-u", "+%Y-%m-%dT%H:%M:%SZ"]),
    "our_commit": out(["git", "rev-parse", "--short", "HEAD"]),
    "our_binary_flags": "-O2 -DNDEBUG -DEL_THREAD_COUNT=%s, HTTP_SERVER_CPU_SET=%s"
                        % (workers, server_cpus),
    "nginx_version": "nginx/%s (built from source, -O2, %s workers, cpus %s)"
                     % (nginx_version, workers, server_cpus),
    "wrk_version": out(["wrk", "--version"]).split("\n")[0].split(" ", 1)[0],
    "wrk_affinity": client_cpus,
    "corpus_seed": json.loads(manifest_bytes)["seed"],
    "corpus_total_bytes": json.loads(manifest_bytes)["total_bytes"],
    "corpus_manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
    "cpu": cpu,
    "logical_cpus": os.cpu_count(),
    "kernel": platform.release(),
    "csv": os.path.relpath(out_csv, os.getcwd()),
    "notes": [
        "Both servers serve byte-identical corpus assets; wrk pinned to CPUs %s, servers to %s." % (client_cpus, server_cpus),
        "gzip-precompressed compares our cached gzip representation to nginx gzip_static.",
        "large is bandwidth-bound and its difference is within run-to-run noise.",
        "wrk has no coordinated-omission correction; use wrk2 for tail-latency claims.",
    ],
    "targets": {
        "ours": "http://127.0.0.1:%s (corpus document root)" % 8081,
        "nginx": "http://127.0.0.1:%s (identity)" % 8082,
        "nginx-gzip-static": "http://127.0.0.1:%s (gzip_static)" % 8084,
        "ours-tls": "https://127.0.0.1:%s" % 8443,
        "nginx-tls": "https://127.0.0.1:%s" % 8445,
    },
}
with open(out_json, "w") as f:
    json.dump(meta, f, indent=2)
    f.write("\n")
print("wrote %s" % out_json)
PY

log "done: $OUT_CSV and $OUT_JSON"
