# Runbook: install and operate the systemd service

Trigger: deploy the built server on a Linux host with systemd, or operate an
installed instance. Everything after build is reversible with `make uninstall`.

## Preconditions

- A C11 compiler, CMake, `zlib1g-dev`, and `libssl-dev`.
- Root (or `sudo`) for the install, the service account, and systemd.
- The tree builds: `cmake -S . -B . && make`.

## 1. Install

```bash
cmake -S . -B .
make
sudo make install          # or: sudo cmake --install .
```

`make install` (prefix `/usr/local`) lays out:

| Path | Contents |
| --- | --- |
| `/usr/local/bin/http_server` | hardened server binary |
| `/usr/local/share/http-server/root/` | shipped `/home`, `/hello` alias assets |
| `/usr/local/share/doc/http-server/` | `readme.md`, `CHANGELOG.md`, `LICENSE` |
| `/usr/local/lib/systemd/system/http-server.service` | hardened unit |
| `/etc/http-server/http_server.conf` | installed config (edit this) |
| `/etc/logrotate.d/http-server` | log rotation |
| `/usr/lib/sysusers.d/http-server.conf` | service account definition |
| `/usr/lib/tmpfiles.d/http-server.conf` | runtime/state/log directories |

Installing to a staged root (packaging) is supported:
`make install DESTDIR=/tmp/stage`. Note the unit and config hardcode the
`/usr/local` prefix; changing `CMAKE_INSTALL_PREFIX` means editing
`deploy/http-server.service` and `deploy/http_server.conf` too.

## 2. Create the account and directories, then start

```bash
sudo systemd-sysusers /usr/lib/sysusers.d/http-server.conf     # create the user/group
sudo systemd-tmpfiles --create /usr/lib/tmpfiles.d/http-server.conf
sudo systemctl daemon-reload
sudo systemctl enable --now http-server
```

Verify:

```bash
systemctl status http-server
systemctl is-active http-server            # active
curl -s http://127.0.0.1:8081/healthz      # ok
curl -s http://127.0.0.1:8081/readyz       # ready
journalctl -u http-server -n 50 --no-pager # startup log + READY
```

Static validation of the unit (before starting) is a good gate:

```bash
systemd-analyze verify /usr/local/lib/systemd/system/http-server.service
```

Serve your own content by pointing `document_root` in
`/etc/http-server/http_server.conf` at a directory readable by the
`http-server` user (the shipped default is the read-only
`/usr/local/share/http-server/root`). A writable root can live under
`/var/lib/http-server/www` (created by the tmpfiles snippet).

## 3. Configure and reload

Configuration precedence is defaults < config file < environment < command
line; the full key list is `docs/env-vars.md` and `http_server --help`. The
unit reads `/etc/http-server/http_server.conf`.

Zero-downtime reload (`SIGHUP`) applies the log level, reopens the log file,
and reloads the TLS certificate. Per-connection limits and timeouts are
restart-only.

```bash
sudo systemctl reload http-server      # SIGHUP
sudo systemctl restart http-server     # limits/timeouts changed
```

## 4. Certificate rotation

1. Drop the new chain and key under `/etc/http-server/tls/` (key mode `0600`,
   owned/readable by `http-server`; a world-accessible key is fatal).
2. Keep `tls=1` and the `tls_cert_file`/`tls_key_file` paths in the config.
3. `sudo systemctl reload http-server` — established sessions keep the old
   certificate; new handshakes use the new one. A failed reload keeps the
   previous certificate and logs `tls_cert_reload failed`.

## 5. Logs and metrics

- Structured JSON logs go to `/var/log/http-server/http-server.log`; logrotate
  rotates them and calls `systemctl reload`.
- With `observability = 1`, `/metrics`, `/healthz`, and `/readyz` are served on
  the data listeners. See `docs/runbooks/observability-and-reload.md`.

## 6. Troubleshooting

| Symptom | Check |
| --- | --- |
| Service exits immediately | `journalctl -u http-server`; the startup error names the failing key/path. A missing `/usr/local/share/http-server/root/home.html` fails the static-asset preflight; a low `RLIMIT_NOFILE` fails the FD preflight. |
| `systemctl start` times out | The unit is `Type=notify`; the server must reach `sd_notify(READY)`. Check that it bound its port (another process on 8081?). |
| `reload` keeps connections but old cert | The new key was not readable by `http-server`, or the paths are wrong; the reload is atomic and keeps the old cert on failure. |
| `403` for expected files | Hidden-file or symlink policy, or the file/ directory is not readable by the `http-server` user. |
| Per-IP limits closing clients | `per_ip_connections`/`per_ip_requests_per_minute` are set; see `docs/env-vars.md`. |

## 7. Uninstall

```bash
sudo make uninstall        # removes exactly what install recorded
sudo systemctl daemon-reload
```

## 8. Reproduce the capacity evidence

```bash
make phase6-capacity    # fresh plaintext + TLS + 2x overload report
make capacity-smoke     # short gate vs benchmarks/ci_baseline.json
make soak               # 3-hour RSS/FD drift soak (run out of band)
```

See `docs/benchmarks.md` for the measurement contract and artifact fields.
