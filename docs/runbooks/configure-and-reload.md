# Runbook: configure and reload

All runtime settings come from one validated surface with precedence
**defaults < config file < environment < command line**. `./bin/http_server
--help` prints every key and its matching environment variable; `docs/env-vars.md`
is the full reference.

## 1. Choose a layer

- **Built-in defaults** → the checked-in `http_server.conf` at the repository
  root is compiled in as `DEFAULT_CONFIG_FILE` and loaded automatically when
  neither `--config` nor `HTTP_SERVER_CONFIG` is given. Edit it to change the
  out-of-the-box settings (it ships with `document_root = root`). Point
  `DEFAULT_CONFIG_FILE` elsewhere at build time with
  `-DDEFAULT_CONFIG_FILE=<path>`; set it empty to disable the fallback.
- **Persistent host settings** → a config file, selected with
  `HTTP_SERVER_CONFIG=/etc/http_server.conf` or `--config`.
- **Ad-hoc/benchmark overrides** → environment variables or CLI flags; higher
  precedence than the file.

Config file format is `key = value`, `#`/`;` line comments, optional quotes:

```ini
port = 8081
max_connections = 4096
max_keepalive_requests = 1000
idle_timeout = 10
shutdown_drain_timeout = 15
log_level = info
log_file = /var/log/http_server.jsonl
access_log = 0
document_root = /srv/www
cache_budget_bytes = 33554432
# Phase 3 TLS (optional); tls_port must differ from port.
tls = 1
tls_port = 8443
tls_cert_file = /etc/http_server/tls/cert.pem
tls_key_file = /etc/http_server/tls/key.pem
```

Keys may be written with `-` or `_`; they map to the same field.

## 2. Validate before deploy

An unknown key or an out-of-range value is fatal at startup and names the exact
key, so a bad file never runs half-configured:

```bash
./bin/http_server --config /etc/http_server.conf --port 1   # exits non-zero, key named
./bin/http_server --config /etc/http_server.conf             # prints effective values
```

The resolved configuration is printed under `=== Server Configuration ===` and
recorded as an `effective_config` JSON record in the log.

## 3. Apply changes

- Startup-only settings (ports, capacity, timeouts, keep-alive limit, logging
  level/target): restart or `systemctl restart`.
- Log file rotation: `SIGHUP` reopens `log_file` in place without dropping
  connections (for example after `logrotate`).
- TLS certificate rotation: replace `tls_cert_file`/`tls_key_file` in place and
  send `SIGHUP`. The certificate is reloaded into the live context; existing
  sessions keep their certificate and no connection is dropped. A failed load
  keeps the previous certificate and logs an error, so rotate by writing the new
  files and then signalling, never by removing them first. The private key must
  stay non-world-readable (`chmod 600`).
- Configuration is *not* reloaded by `SIGHUP`; a config change needs a restart.

## 4. Stop cleanly

`SIGTERM`/`SIGINT` stop accepting and drain in-flight responses within
`shutdown_drain_timeout`; the process exits 0 when the drain completes. When
launched by systemd (`Type=notify`, `NOTIFY_SOCKET` set) the server sends
`READY=1` after binding and `STOPPING=1` during shutdown.

## 5. Verify

```bash
make phase0-lifecycle     # invalid-config rejection + SIGTERM drain
make phase3-tls           # TLS handshake/cipher/ALPN, bodies, SIGHUP cert reload
```

See also `docs/runbooks/change-a-limit.md` for adding a new limit to the
surface.
