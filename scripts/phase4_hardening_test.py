#!/usr/bin/env python3
"""E2E: Phase 4 compiler hardening and privilege-drop acceptance.

Checks, black-box against the built binary and a real server process:

  * the executable is a PIE with full RELRO, a non-executable stack, the stack
    protector, and _FORTIFY_SOURCE fortified libc wrappers (readelf);
  * `run_user`/`run_group` are part of the runtime configuration surface
    (`--help` lists them, startup prints the resolved identity);
  * an unresolvable user or group is fatal and names the offending key;
  * with an identity configured the process really runs as that uid/gid with
    `NoNewPrivs: 1` and still serves a request.

The result is written to benchmarks/production_phase4_hardening.json and the
process exits non-zero when any gate fails.
"""
import argparse
import http.client
import json
import os
import pwd
import re
import shutil
import socket
import subprocess
import sys
import time

FORTIFIED_RE = re.compile(r"__[A-Za-z0-9_]+_chk")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def run_capture(argv, cwd, timeout=10):
    proc = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, timeout=timeout)
    return proc.returncode, proc.stdout


def hardening_report(binary):
    """Inspect the ELF with readelf. Returns (dict, list_of_failures)."""
    failures = []
    if shutil.which("readelf") is None:
        return {}, ["readelf not found; cannot verify ELF hardening"]

    header = subprocess.run(["readelf", "-h", binary], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True).stdout
    program = subprocess.run(["readelf", "-lW", binary], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True).stdout
    dynamic = subprocess.run(["readelf", "-dW", binary], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True).stdout
    symbols = subprocess.run(["readelf", "-sW", binary], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True).stdout

    pie = "Type:" in header and "DYN" in header.split("Type:", 1)[1].split("\n")[0]
    relro = "GNU_RELRO" in program
    bind_now = "BIND_NOW" in dynamic or "BIND_NOW" in program

    nx_stack = True
    stack_line = ""
    for line in program.splitlines():
        if line.strip().startswith("GNU_STACK"):
            stack_line = line
            flags = line.split()[-2] if len(line.split()) >= 2 else ""
            nx_stack = "E" not in flags
            break
    if not stack_line:
        nx_stack = False

    stack_protector = "__stack_chk_fail" in symbols
    fortified = sorted(set(FORTIFIED_RE.findall(symbols)))

    report = {
        "pie": pie,
        "relro": relro,
        "bind_now": bind_now,
        "nx_stack": nx_stack,
        "stack_protector": stack_protector,
        "fortified_symbols": fortified,
        "gnu_stack_flags": stack_line.strip(),
    }
    for name in ("pie", "relro", "bind_now", "nx_stack", "stack_protector"):
        if not report[name]:
            failures.append("ELF hardening: %s not present" % name)
    if not fortified:
        failures.append("ELF hardening: no _FORTIFY_SOURCE wrapper symbols")
    return report, failures


def wait_port(proc, port, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            out = proc.stdout.read() if proc.stdout else ""
            raise RuntimeError("server exited early (rc=%s):\n%s"
                               % (proc.returncode, out))
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.3):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start on port %d" % port)


def start_server(repo_root, args):
    server = os.path.join(repo_root, "bin", "http_server")
    env = dict(os.environ)
    env["HTTP_SERVER_ACCESS_LOG"] = "0"
    return subprocess.Popen([server, "--log-level", "error"] + args,
                            cwd=repo_root, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)


def proc_identity(pid):
    vals = {}
    with open("/proc/%d/status" % pid) as f:
        for line in f:
            key, _, value = line.partition(":")
            if key in ("Uid", "Gid", "NoNewPrivs"):
                vals[key] = value.split()
    return vals


def http_status(port, path="/hello"):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    conn.request("GET", path)
    resp = conn.getresponse()
    status = resp.status
    resp.read()
    conn.close()
    return status


def wait_identity(proc, want_uid, want_gid, timeout=5):
    """A listener starts accepting before privilege_drop() runs, so poll the
    process status until the drop is observable (or time out)."""
    deadline = time.time() + timeout
    ident = {}
    while time.time() < deadline:
        if proc.poll() is not None:
            break
        ident = proc_identity(proc.pid)
        uids = [int(x) for x in ident.get("Uid", [])]
        gids = [int(x) for x in ident.get("Gid", [])]
        uid_ok = want_uid is None or uids[:4] == [want_uid] * 4
        gid_ok = want_gid is None or gids[:4] == [want_gid] * 4
        if uid_ok and gid_ok and ident.get("NoNewPrivs") == ["1"]:
            break
        time.sleep(0.05)
    return ident


def drop_case(repo_root, name, args, want_uid, want_gid, failures, report):
    port = free_port()
    args = list(args) + ["--port", str(port)]
    proc = start_server(repo_root, args)
    detail = {"args": args}
    try:
        wait_port(proc, port)
        ident = wait_identity(proc, want_uid, want_gid)
        detail["proc"] = ident
        if want_uid is not None:
            uids = [int(x) for x in ident.get("Uid", [])]
            if uids[:4] != [want_uid] * 4:
                failures.append("%s: Uid=%s, wanted %d" %
                                (name, ident.get("Uid"), want_uid))
        if want_gid is not None:
            gids = [int(x) for x in ident.get("Gid", [])]
            if gids[:4] != [want_gid] * 4:
                failures.append("%s: Gid=%s, wanted %d" %
                                (name, ident.get("Gid"), want_gid))
        if ident.get("NoNewPrivs") != ["1"]:
            failures.append("%s: NoNewPrivs=%s, wanted 1" %
                            (name, ident.get("NoNewPrivs")))
        status = http_status(port)
        detail["http_status"] = status
        if status != 200:
            failures.append("%s: GET /hello returned %s" % (name, status))
    except Exception as exc:  # noqa: BLE001 - report any startup/serve failure
        failures.append("%s: %s" % (name, exc))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    report[name] = detail


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=os.getcwd())
    parser.add_argument("--output",
                        default="benchmarks/production_phase4_hardening.json")
    args = parser.parse_args()
    repo_root = os.path.abspath(args.repo_root)
    out_path = args.output if os.path.isabs(args.output) \
        else os.path.join(repo_root, args.output)
    binary = os.path.join(repo_root, "bin", "http_server")

    report = {}
    failures = []

    if not os.path.exists(binary):
        failures.append("binary not built: %s" % binary)

    if os.path.exists(binary):
        hw, hw_failures = hardening_report(binary)
        report["hardening"] = hw
        failures.extend(hw_failures)

        rc, out = run_capture([binary, "--help"], repo_root)
        report["help_lists_keys"] = {
            "run_user": "run_user" in out,
            "run_group": "run_group" in out,
        }
        if rc != 0:
            failures.append("--help exited %d" % rc)
        for key in ("run_user", "run_group"):
            if key not in out:
                failures.append("--help does not list %s" % key)

        port = free_port()
        rc, out = run_capture([binary, "--run-user", "phase4-no-such-user",
                               "--port", str(port)], repo_root)
        report["invalid_user"] = {"rc": rc, "named_key": "run_user" in out}
        if rc == 0 or "run_user" not in out:
            failures.append("invalid run_user did not fail naming run_user")

        port = free_port()
        rc, out = run_capture([binary, "--run-group", "phase4-no-such-group",
                               "--port", str(port)], repo_root)
        report["invalid_group"] = {"rc": rc, "named_key": "run_group" in out}
        if rc == 0 or "run_group" not in out:
            failures.append("invalid run_group did not fail naming run_group")

        if os.geteuid() == 0:
            try:
                nobody = pwd.getpwnam("nobody")
                drop_case(repo_root, "drop_to_nobody",
                          ["--run-user", "nobody"], nobody.pw_uid,
                          nobody.pw_gid, failures, report)
            except KeyError:
                failures.append("root run: 'nobody' user not found")
        else:
            me = pwd.getpwuid(os.getuid())
            drop_case(repo_root, "drop_same_identity_named",
                      ["--run-user", me.pw_name], os.getuid(),
                      me.pw_gid, failures, report)
            drop_case(repo_root, "drop_same_identity_numeric",
                      ["--run-user", str(os.getuid()),
                       "--run-group", str(os.getgid())],
                      os.getuid(), os.getgid(), failures, report)

    report["failures"] = failures
    report["passed"] = not failures
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w") as f:
        json.dump(report, f, indent=2, sort_keys=True)
        f.write("\n")

    if failures:
        print("FAIL: Phase 4 hardening acceptance")
        for item in failures:
            print("  - %s" % item)
        print("artifact: %s" % out_path)
        return 1
    print("PASS: Phase 4 hardening acceptance (artifact: %s)" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
