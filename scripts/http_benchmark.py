#!/usr/bin/env python3
"""Small, dependency-free HTTP load generator for this project.

The client deliberately validates HTTP framing instead of treating an idle
socket timeout as the end of a response.  By default each request uses a new
connection, which measures the server's accept and worker-pool path. Use
--keep-alive to reuse one connection per worker.
"""

import argparse
import collections
import csv
import math
import os
import signal
import socket
import subprocess
import sys
import threading
import time


class RequestError(Exception):
    pass


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(math.ceil(fraction * len(ordered))) - 1)
    return ordered[index]


def git_commit_id():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        return subprocess.check_output(
            ["git", "-C", repo_root, "rev-parse", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def log_result(path, commit_id, rps, completed, errors, elapsed):
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    write_header = not os.path.exists(path) or os.path.getsize(path) == 0
    with open(path, "a", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=("timestamp_utc", "commit_id", "throughput_rps",
                        "completed", "errors", "elapsed_seconds"),
        )
        if write_header:
            writer.writeheader()
        writer.writerow({
            "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "commit_id": commit_id,
            "throughput_rps": "%.2f" % rps,
            "completed": completed,
            "errors": errors,
            "elapsed_seconds": "%.3f" % elapsed,
        })


def read_exact(sock, size):
    result = bytearray()
    while len(result) < size:
        chunk = sock.recv(min(65536, size - len(result)))
        if not chunk:
            raise RequestError("connection closed before Content-Length")
        result.extend(chunk)
    return bytes(result)


def read_response(sock):
    head = bytearray()
    while b"\r\n\r\n" not in head:
        chunk = sock.recv(4096)
        if not chunk:
            raise RequestError("connection closed before response headers")
        head.extend(chunk)
        if len(head) > 65536:
            raise RequestError("response headers exceed 64 KiB")

    header_bytes, remainder = bytes(head).split(b"\r\n\r\n", 1)
    lines = header_bytes.split(b"\r\n")
    try:
        version, status_text, reason = lines[0].decode("ascii").split(" ", 2)
        status = int(status_text)
    except (ValueError, UnicodeDecodeError):
        raise RequestError("malformed HTTP status line")
    if version != "HTTP/1.1":
        raise RequestError("unexpected HTTP version")

    headers = {}
    for line in lines[1:]:
        if b":" not in line:
            raise RequestError("malformed response header")
        name, value = line.split(b":", 1)
        headers[name.decode("ascii").lower()] = value.strip().decode("ascii").lower()

    if "content-length" in headers:
        try:
            length = int(headers["content-length"])
        except ValueError:
            raise RequestError("invalid Content-Length")
        if length < 0:
            raise RequestError("negative Content-Length")
        body = remainder + read_exact(sock, max(0, length - len(remainder)))
        if len(body) != length:
            raise RequestError("response body length mismatch")
    elif headers.get("transfer-encoding") == "chunked":
        body = bytearray()
        pending = bytearray(remainder)
        while True:
            while b"\r\n" not in pending:
                chunk = sock.recv(4096)
                if not chunk:
                    raise RequestError("connection closed before chunk header")
                pending.extend(chunk)
            line, _, pending = pending.partition(b"\r\n")
            try:
                chunk_size = int(line.split(b";", 1)[0], 16)
            except ValueError:
                raise RequestError("invalid chunk size")
            if chunk_size == 0:
                while len(pending) < 2:
                    chunk = sock.recv(4096)
                    if not chunk:
                        raise RequestError("connection closed before chunk trailer")
                    pending.extend(chunk)
                if pending[:2] != b"\r\n":
                    raise RequestError("invalid chunk terminator")
                break
            while len(pending) < chunk_size + 2:
                chunk = sock.recv(4096)
                if not chunk:
                    raise RequestError("connection closed inside chunk")
                pending.extend(chunk)
            body.extend(pending[:chunk_size])
            if pending[chunk_size:chunk_size + 2] != b"\r\n":
                raise RequestError("invalid chunk terminator")
            del pending[:chunk_size + 2]
    else:
        raise RequestError("response has neither Content-Length nor chunked framing")

    return status, bytes(body)


def one_request(host, port, path, timeout, keep_alive, sock=None):
    own_socket = sock is None
    if own_socket:
        sock = socket.create_connection((host, port), timeout)
        sock.settimeout(timeout)
    request = ("GET %s HTTP/1.1\r\nHost: %s\r\nAccept-Encoding: gzip\r\n"
               "Connection: %s\r\n\r\n") % (path, host, "keep-alive" if keep_alive else "close")
    try:
        sock.sendall(request.encode("ascii"))
        status, body = read_response(sock)
        if status == 200 and not body:
            raise RequestError("successful response has an empty body")
        return status, body
    finally:
        if own_socket:
            sock.close()


def worker(args, state, worker_id):
    latencies = []
    statuses = collections.Counter()
    errors = []
    sock = None
    if args.keep_alive:
        try:
            sock = socket.create_connection((args.host, args.port), args.timeout)
            sock.settimeout(args.timeout)
        except OSError as exc:
            state["startup_error"] = str(exc)
            return latencies, statuses, errors

    while True:
        with state["lock"]:
            if state["next_request"] >= state["request_limit"]:
                break
            request_number = state["next_request"]
            state["next_request"] += 1
        if state["start"] is None:
            state["start"] = time.monotonic()
        if args.rate > 0:
            due = state["start"] + request_number / args.rate
            delay = due - time.monotonic()
            if delay > 0:
                time.sleep(delay)
        started = time.monotonic()
        try:
            status, _ = one_request(args.host, args.port, args.path, args.timeout,
                                    args.keep_alive, sock)
            statuses[status] += 1
        except (OSError, RequestError) as exc:
            errors.append(str(exc))
            if sock is not None:
                sock.close()
                sock = None
                try:
                    sock = socket.create_connection((args.host, args.port), args.timeout)
                    sock.settimeout(args.timeout)
                except OSError:
                    pass
        latencies.append((time.monotonic() - started) * 1000.0)

    if sock is not None:
        sock.close()
    return latencies, statuses, errors


def run(args):
    state = {"lock": threading.Lock(), "next_request": 0,
             "request_limit": args.requests, "start": None, "startup_error": None}
    threads = []
    results = []
    barrier = threading.Barrier(args.concurrency)

    def run_worker(worker_id):
        barrier.wait()
        results.append(worker(args, state, worker_id))

    for worker_id in range(args.concurrency):
        thread = threading.Thread(target=run_worker, args=(worker_id,))
        thread.start()
        threads.append(thread)
    for thread in threads:
        thread.join()

    elapsed = max(0.000001, time.monotonic() - state["start"])
    latencies = [latency for result in results for latency in result[0]]
    statuses = collections.Counter()
    errors = []
    for _, result_statuses, result_errors in results:
        statuses.update(result_statuses)
        errors.extend(result_errors)
    completed = sum(statuses.values())
    rps = completed / elapsed
    print("requests=%d completed=%d errors=%d elapsed=%.3fs" %
          (args.requests, completed, len(errors), elapsed))
    print("throughput=%.2f req/s target=%.2f req/s concurrency=%d" %
          (rps, args.rate, args.concurrency))
    print("latency_ms min=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f" %
          (min(latencies) if latencies else 0.0, percentile(latencies, .50),
           percentile(latencies, .95), percentile(latencies, .99),
           max(latencies) if latencies else 0.0))
    print("statuses=%s" % dict(sorted(statuses.items())))
    if errors:
        print("sample_error=%s" % errors[0])

    if args.log_file:
        try:
            log_result(args.log_file, git_commit_id(), rps, completed,
                       len(errors), elapsed)
            print("result_log=%s" % args.log_file)
        except OSError as exc:
            print("FAIL: could not write result log: %s" % exc, file=sys.stderr)
            return 2

    if state["startup_error"] or errors or statuses.get(200, 0) != completed:
        return 1
    if args.min_rps and rps < args.min_rps:
        print("FAIL: throughput %.2f is below minimum %.2f req/s" % (rps, args.min_rps))
        return 1
    return 0


def wait_for_server(host, port, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), 0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start on %s:%d" % (host, port))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--path", default="/home")
    parser.add_argument("--duration", type=float, default=10.0)
    parser.add_argument("--rate", type=float, default=100.0,
                        help="target request rate; 0 means closed-loop")
    parser.add_argument("--requests", type=int, default=0,
                        help="total requests (defaults to ceil(rate * duration))")
    parser.add_argument("--concurrency", type=int, default=16)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--min-rps", type=float, default=0.0)
    parser.add_argument("--log-file", help="append results and commit ID to CSV")
    parser.add_argument("--keep-alive", action="store_true")
    parser.add_argument("--start-server", action="store_true")
    parser.add_argument("--server", default="./bin/http_server")
    args = parser.parse_args()
    if args.concurrency < 1 or args.duration <= 0 or args.rate < 0:
        parser.error("duration, rate, and concurrency values are invalid")
    if args.requests <= 0:
        if args.rate <= 0:
            parser.error("--requests is required when --rate=0")
        args.requests = int(math.ceil(args.rate * args.duration))

    server = None
    try:
        if args.start_server:
            server = subprocess.Popen([args.server], cwd=os.getcwd(),
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            wait_for_server(args.host, args.port, 5.0)
        else:
            wait_for_server(args.host, args.port, 1.0)
        return run(args)
    except (OSError, RuntimeError) as exc:
        print("FAIL: %s" % exc, file=sys.stderr)
        return 2
    finally:
        if server is not None:
            server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


if __name__ == "__main__":
    sys.exit(main())
