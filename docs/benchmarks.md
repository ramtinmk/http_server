# Benchmarks: file-class methodology

A single request-rate number is not a fair way to compare HTTP servers because
file type selects a different code path. In this server the path depends on
size and entropy:

| Class      | Size      | Content        | Code path                                             |
|------------|-----------|----------------|-------------------------------------------------------|
| `tiny`     | 256 B     | compressible   | in-memory write (`/home`/`/hello`-style cached asset) |
| `small`    | 8 KiB     | compressible   | cached doc-root asset                                 |
| `medium`   | 256 KiB   | compressible   | cached asset; gzip CPU cost                           |
| `binary`   | 256 KiB   | incompressible | cached incompressible asset                           |
| `streamed` | 2 MiB     | incompressible | `sendfile` streaming (`>= CACHE_MAX_FILE_BYTES`)      |
| `large`    | 16 MiB    | incompressible | `sendfile`, bandwidth-bound                           |
| `huge`     | 256 MiB   | incompressible | `sendfile`, bandwidth-bound (opt-in)                  |

`CACHE_MAX_FILE_BYTES` is 1 MiB (`include/server_config.h`); files at or above
it bypass the cache and stream. TLS and keep-alive are transport axes layered on
top of every class, not classes themselves.

## Deterministic corpus

`scripts/benchmark_corpus.py` materializes byte-identical assets for every
class plus a manifest describing them (`bytes`, `sha256`, `content_type`,
`compressible`, `class`, `exercises`). Content is generated from a seed with
hash-based (incompressible) and token-cycled (compressible) streams, so a fixed
seed reproduces the same bytes on any host and any Python 3.8+.

```bash
make corpus                     # -> benchmarks/corpus/ + manifest.json (+ .gz)
python3 scripts/benchmark_corpus.py --list             # plan, no writes
python3 scripts/benchmark_corpus.py --verify           # files vs manifest
python3 scripts/benchmark_corpus.py --include-huge     # add the 256 MiB class
python3 scripts/benchmark_corpus.py --with-gzip        # add .gz siblings
```

`--with-gzip` writes a deterministic `<name>.gz` next to each compressible file
(and records `gzip_name`/`gzip_bytes`/`gzip_sha256`), so a peer such as nginx
`gzip_static` can serve the same precompressed bytes. `make corpus` enables it.

## Driving a class

Start the server on the corpus document root and hit one class at a time:

```bash
python3 scripts/benchmark_corpus.py --output-dir benchmarks/corpus
python3 scripts/http_benchmark.py --start-server \
    --document-root benchmarks/corpus \
    --path /small/page.html --keep-alive --rate 5000 --duration 10 \
    --log-file benchmarks/corpus_matrix.csv
```

`--document-root` sets `HTTP_SERVER_DOCUMENT_ROOT` for the server the harness
starts, so the same command works whether the corpus lives in a temp dir or in
`benchmarks/corpus`. Repeat per class (and per transport: add `--tls`) rather
than averaging across classes.

## Reporting

- Report a result **grid**, not one number: one row per
  `class x size x gzip x keep-alive x TLS x concurrency` cell.
- Class-specific metric: RPS and p99 for `tiny`/`small`/`medium`/`binary`; MB/s
  for `streamed`/`large`/`huge` (RPS is a client/generator bound there).
- Include the `limited_by` column and the host fingerprint already emitted by
  the harness; a client-bound row is not a server result.
- The corpus manifest carries a default `traffic_mix` (weights sum to 1.0) for a
  single weighted rollup. The weighting is an assumption and must be published
  alongside the rollup so it can be challenged.
- `Cache warm` (post-startup) and `cache cold` are different measurements; label
  them, and state access-log and keep-alive settings and the pinned CPU sets.

See `docs/runbooks/reproduce-a-benchmark.md` for the host preflight and the
pinned matrix runner, and `plans/scaling-plan.md` for the measurement contract.

## Peer comparison

`scripts/compare_servers.py` drives `wrk` against one or more already-running
servers, one class at a time, and writes a long-form CSV:

```bash
python3 scripts/compare_servers.py --corpus benchmarks/corpus \
  --target ours=http://127.0.0.1:8081 --target nginx=http://127.0.0.1:8082 \
  --wrk-cpus 4-7 --output benchmarks/nginx_comparison.csv
```

`scripts/run_nginx_comparison.sh` wraps the whole thing: it builds nginx and this
server with matched worker counts and `-O2`, starts both (plaintext,
`gzip_static`, and TLS), and runs identity, gzip, and TLS modes into
`benchmarks/nginx_comparison.csv` + `.json`. The recipe, fairness rules, and
recorded results (including TLS) are in
`docs/runbooks/compare-against-nginx.md`.
