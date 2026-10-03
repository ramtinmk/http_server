# Security Policy

## Supported versions

Security fixes land on the latest release and on `main`. Use a tagged release
or the tip of `main`; older revisions are not maintained.

## Reporting a vulnerability

Do **not** open a public issue for a security problem. Report it privately
through GitHub's private vulnerability reporting (the **Security → Report a
vulnerability** tab of this repository), or by contacting the maintainer named
in [`LICENSE`](LICENSE) if you cannot use GitHub.

Please include:

- the affected version or commit — `build-manifest.json` records the exact
  source revision and toolchain;
- the resolved configuration, from the startup "Server Configuration" log
  block or `./bin/http_server --help`, and the document root in effect;
- a minimal request or reproducer, and the observed versus expected behavior.

## What to expect

- Acknowledgement within 5 business days.
- An assessment, and where a fix is warranted, a patched release and a
  published advisory. Reporters are credited unless they ask otherwise.
- Please allow time for a fix before public disclosure.

## Scope

In scope: the server binary and the code in `src/` and `include/` — the HTTP
parser, path resolution and static-file serving, TLS termination, the
privilege-drop and Landlock/seccomp sandbox, and the shipped systemd unit and
install layout.

Out of scope: vulnerabilities in third-party dependencies (report those
upstream; OSV-Scanner runs in CI to track them), misconfiguration that the
startup preflight already rejects, and denial of service from a host already
beyond the documented capacity envelope.

## Hardening already in place

The default build is PIE with full RELRO, a non-executable stack,
`-fstack-protector-strong`, and `_FORTIFY_SOURCE=2`. At runtime the server can
drop irreversibly to an unprivileged `run_user`/`run_group` with `no_new_privs`,
optionally apply Landlock/seccomp, and enforce bounded admission control. The
shipped systemd unit adds further sandboxing. See `docs/architecture.md` and
[`deploy/http-server.service`](deploy/http-server.service).
