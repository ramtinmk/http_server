#!/usr/bin/env python3
"""E2E: static assets are precompressed and cached at startup, and startup
fails fast when an asset cannot be loaded.

The server opens home.html/hello.html via relative paths, so running the binary
from a directory without those files must exit non-zero with a clear message
instead of accepting connections and failing per request later.
"""
import argparse
import os
import subprocess
import sys
import tempfile


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default=None,
                        help="server binary (default: <repo>/bin/http_server)")
    args = parser.parse_args(argv)

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    server = args.server or os.path.join(repo_root, "bin", "http_server")
    if not os.path.exists(server):
        print("FAIL: server binary not found at %s" % server, file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="http_failfast_") as empty_dir:
        proc = subprocess.run([server], cwd=empty_dir,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, timeout=10)
    output = proc.stdout
    if proc.returncode == 0:
        print("FAIL: server started without its static assets", file=sys.stderr)
        return 1
    if "Failed to cache" not in output:
        print("FAIL: server exited without a clear asset-load error:\n%s" % output,
              file=sys.stderr)
        return 1

    print("PASS: startup fails fast when a static asset is missing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
