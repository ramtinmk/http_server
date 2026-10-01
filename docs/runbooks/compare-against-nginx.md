# Runbook: compare against nginx

Peer-comparison of this server against nginx on the deterministic corpus
(`docs/benchmarks.md`), driven by `wrk` (see the `wrk` vs `wrk2` note under
"Caveats"). One command builds an optimized nginx and an optimized build of this
server with a matched worker count, starts both (plaintext, `gzip_static`, and
TLS), and runs the per-class matrix with the servers pinned to one CPU set and
the generator to a disjoint one.

```bash
bash scripts/run_nginx_comparison.sh
# -> benchmarks/nginx_comparison.csv and benchmarks/nginx_comparison.json
```

Tunables are environment variables documented at the top of the script, e.g.
`HPNGX_SERVER_CPUS=0-3 HPNGX_CLIENT_CPUS=4-7 HPNGX_WORKERS=4 HPNGX_DURATION=8
HPNGX_REPEATS=2`. Defaults assume an 8-core host.

## What the script does

1. Generates `benchmarks/corpus` with `--with-gzip` (`benchmark_corpus.py`).
2. Generates a throwaway self-signed certificate when one is absent.
3. Builds nginx into `$HPNGX_WORK/nginx-prefix` from source if missing
   (`--without-pcre`, HTTP/1.1 static serving + `gzip_static` + TLS, `-O2`).
4. Builds this server at `-O2` with `EL_THREAD_COUNT=$HPNGX_WORKERS` so the
   event-loop count matches nginx's worker count. (The checked-in CMake build
   now defaults to `Release`/`-O2`, but its loop count is auto = online cores.)
5. Renders `scripts/nginx_reference.conf` with the corpus root, cert, CPU
   affinity masks, and ports, then starts it.
6. Runs `scripts/compare_servers.py` three times into one CSV, tagged by mode:
   `identity` (8082), `gzip-precompressed` (8084, `gzip_static`), and `tls`
   (8445). Each run appends via `--mode ... --append`.

Requirements: `wrk`, `taskset`, `openssl`, `curl`, `cc`/`make`, and network
access for the nginx tarball. This is an on-demand comparison, not a default
test.

## Recorded results

Best of 2, `wrk -t4`, servers on CPUs 0-3, wrk on 4-7, identity/gzip at `-c100`
and TLS at `-c32`. Ratio = nginx / ours (below 1.0 means ours is faster).
Evidence: `benchmarks/nginx_comparison.csv` + `benchmarks/nginx_comparison.json`.

| mode              | class    | ours RPS | nginx RPS | ratio |
|-------------------|----------|---------:|----------:|------:|
| identity          | tiny     |   91,068 |   121,473 | 1.33  |
| identity          | small    |   87,957 |   111,560 | 1.27  |
| identity          | medium   |   20,870 |    30,636 | 1.47  |
| identity          | binary   |   16,976 |    35,870 | 2.11  |
| identity          | streamed |    4,821 |     5,254 | 1.09  |
| identity          | large    |      429 |       359 | 0.84  |
| gzip-precompressed| tiny     |  103,883 |   153,786 | 1.48  |
| gzip-precompressed| small    |  106,234 |   132,958 | 1.25  |
| gzip-precompressed| medium   |  112,338 |   140,689 | 1.25  |
| tls               | tiny     |   55,613 |    45,532 | 0.82  |
| tls               | small    |   46,410 |    61,821 | 1.33  |
| tls               | medium   |    7,172 |     7,505 | 1.05  |
| tls               | binary   |    7,176 |     6,764 | 0.94  |
| tls               | streamed |      950 |       968 | 1.02  |
| tls               | large    |      102 |        99 | 0.97  |

Reading:

- Plaintext cached files: nginx leads 1.3–2.1×. The gap is largest for the
  incompressible `binary` class and tracks nginx's `sendfile` from page cache
  versus our heap `write()` for cached bodies.
- Plaintext `streamed`: near parity (1.09×); `large` is bandwidth-bound and ours
  is slightly ahead (0.84×), inside run-to-run noise.
- gzip: like-for-like (ours cached gzip vs nginx `gzip_static`) nginx leads
  1.25–1.48×.
- TLS: roughly parity across classes. TLS cannot `sendfile` for either server,
  so nginx's plaintext file-serving edge disappears; ours wins `tiny` and
  `binary`, nginx wins `small`.

## Caveats

- **`wrk` has no coordinated-omission correction.** Its p99 is approximate; use
  `wrk2` at a fixed rate for a latency claim. Throughput is unaffected.
- **Loopback generator bound.** `medium`/`binary` identity move hundreds of
  MiB/s and approach the loopback limit; a row whose client CPU is pegged is a
  client-side bound.
- **Compression must be like-for-like.** Our server caches the compressed body,
  so compare against nginx `gzip_static`, never nginx dynamic `gzip on` (which
  recompresses per request). `make corpus` writes the `.gz` siblings.
- **`large` is bandwidth-bound**; its difference is within run-to-run noise.
- **Warm cache.** Every class is measured after the other classes have already
  run; label cold-start results separately if you measure them.
- **Do not compare across kernels/governors.** Keep the servers and generator on
  the same host as the recorded artifact.
