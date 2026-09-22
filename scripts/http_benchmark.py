#!/usr/bin/env python3
"""Dependency-free HTTP load generator implementing the measurement contract in
plans/scaling-plan.md.

The client validates HTTP framing (Content-Length or chunked encoding) instead
of treating an idle socket timeout as response completion. It records offered
load, completed and failed requests, status distribution, latency percentiles,
connection and keep-alive request rates, and (when it owns or is told about the
server process) CPU, RSS, open file descriptors, context switches, server
thread-pool queue depth, active workers, and rejected tasks.

Run the scenario matrix described by the plan through --scenario, or drive a
custom run with --rate/--duration/--keep-alive/--path. Results are written to
CSV or JSON Lines while human-readable output is retained.
"""

import argparse
import collections
import csv
import hashlib
import json
import math
import os
import platform
import resource
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time


CALIBRATION_ALGORITHM = "sha256-ramped-buffer"
CALIBRATION_DEFAULT_SECONDS = 2.0
# This is a stable reporting unit, not a claim about the speed of a particular
# host. Ratios between machine indexes are what make normalized results useful.
CALIBRATION_REFERENCE_OPS_PER_SEC = 10000.0
CALIBRATION_CACHE_VERSION = 1
CALIBRATION_PAYLOAD_MAX = 1024 * 1024
CALIBRATION_PAYLOAD_MIN = 1024


CSV_FIELDS = (
    "timestamp_utc", "commit_id", "scenario", "host", "port", "mode",
    "target_rate", "warmup_seconds", "steady_state_seconds",
    "duration_seconds", "drain_seconds", "concurrency", "requests_planned",
    "warmup_requests_offered", "steady_requests_offered",
    "unoffered_requests", "steady_responses_completed", "drain_responses_completed",
    "total_responses_completed", "total_failed", "per_second_rates",
    "responses_received", "successful", "failed", "throughput_rps",
    "successful_rps", "offered_rps", "connection_rate", "keepalive_request_rate",
    "requests_per_connection", "status_200", "status_400", "status_404",
    "status_413", "status_431", "status_500", "status_501", "status_other",
    "latency_min_ms", "latency_p50_ms", "latency_p95_ms", "latency_p99_ms",
    "latency_max_ms", "fail_connect", "fail_send", "fail_receive",
    "fail_framing", "fail_status", "fail_timeout", "fail_other",
    "server_cpu_percent", "server_rss_kb", "server_open_fds",
    "server_active_workers_max", "server_queue_depth_max",
    "server_accepted_connections", "server_rejected_tasks",
    "server_completed_requests", "server_request_failures",
    "ctx_switches_voluntary", "ctx_switches_involuntary", "tcp_retransmits",
    "server_cpu_seconds", "server_cpu_seconds_per_1000_requests",
    "rps_per_server_cpu_second", "server_cpu_cores", "client_cpu_seconds",
    "client_cpu_percent", "hardware_agnostic_rps", "limited_by",
    "machine_index", "calibration_ops_per_sec", "calibration_reference_ops_per_sec",
    "calibration_algorithm", "calibration_seconds", "calibrated",
    "throughput_rps_normalized", "successful_rps_normalized",
    "latency_p50_ms_normalized", "latency_p95_ms_normalized",
    "latency_p99_ms_normalized",
    "machine_id", "host_name", "cpu_model", "cpu_logical_cores",
    "cpu_physical_cores", "cpu_mhz", "cpu_max_mhz", "memory_total_kb",
    "os_kernel", "compiler_flags", "ulimit_nofile_soft", "ulimit_nofile_hard",
    "page_size_kb",
    # Phase 4 saturation / event-loop counters (trailing additions preserve
    # backward compatibility; older rows migrate with empty values).
    "server_connection_capacity", "server_active_connections",
    "server_active_connections_max", "server_listener_disabled",
    "server_listener_disabled_ms", "server_admission_rejected",
    "server_overload_responses", "server_connection_resets",
    "server_buffer_bytes_current", "server_buffer_bytes_max",
    "server_input_buffer_limit", "server_el_wakeups",
    "server_el_readable_events", "server_el_writable_events",
    "server_el_eagain", "server_el_partial_writes",
    "server_el_deadline_closes", "server_el_pipeline_full",
    "server_el_output_drained", "server_el_connections_opened",
    "server_el_connections_closed",
    "server_admission_rejected_capacity", "server_admission_rejected_table_full",
    "server_backlog_depth", "server_backlog_depth_max",
    "server_el_loops", "server_el_loop_wakeups", "server_el_loop_accepted",
)

SCENARIOS = {
    "new-connection-1000": {
        "rate": 1000.0,
        "duration": 10.0,
        "keep_alive": False,
        "specs": [{"path": "/home"}],
    },
    "new-connection-5000": {
        "rate": 5000.0,
        "duration": 10.0,
        "keep_alive": False,
        "specs": [{"path": "/home"}],
    },
    "keep-alive-5000": {
        "rate": 5000.0,
        "duration": 10.0,
        "keep_alive": True,
        "specs": [{"path": "/home"}],
    },
    "mixed-paths-5000": {
        "rate": 5000.0,
        "duration": 10.0,
        "keep_alive": False,
        "specs": [{"path": "/home"}, {"path": "/hello"}],
    },
    "gzip-500": {
        "rate": 500.0,
        "duration": 10.0,
        "keep_alive": False,
        "gzip": True,
        "specs": [{"path": "/home"}],
    },
    "error-paths": {
        "rate": 1000.0,
        "duration": 10.0,
        "keep_alive": False,
        "diagnostic": True,
        "specs": [
            {"method": "GET", "path": "/benchmark_not_found", "expect": [404]},
            {"method": "POST", "path": "/home", "expect": [501]},
        ],
    },
    "slow-clients": {
        "rate": 500.0,
        "duration": 10.0,
        "keep_alive": False,
        "diagnostic": True,
        "slow_clients": 8,
        "specs": [{"path": "/home"}],
    },
}


def _read_text(path):
    try:
        with open(path, encoding="utf-8") as source:
            return source.read()
    except (OSError, UnicodeError):
        return ""


def _cpuinfo_records():
    records = []
    current = {}
    for line in _read_text("/proc/cpuinfo").splitlines():
        if not line.strip():
            if current:
                records.append(current)
                current = {}
            continue
        key, separator, value = line.partition(":")
        if separator:
            current[key.strip()] = value.strip()
    if current:
        records.append(current)
    return records


def _read_mem_total_kb():
    for line in _read_text("/proc/meminfo").splitlines():
        key, separator, value = line.partition(":")
        if key == "MemTotal" and separator:
            try:
                return int(value.strip().split()[0])
            except (IndexError, ValueError):
                break
    return 0


def _read_cpu_max_mhz():
    value = _read_text(
        "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq"
    ).strip()
    if not value:
        return 0.0
    try:
        # cpufreq exposes kHz; accepting MHz as a fallback keeps this useful on
        # kernels that expose a human-readable value instead.
        numeric = float(value)
        return round(numeric / 1000.0 if numeric > 10000 else numeric, 3)
    except ValueError:
        return 0.0


def _read_compiler_flags():
    """Return normalized server compiler flags when compile_commands is present."""
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    compile_commands = os.path.join(repo_root, "compile_commands.json")
    try:
        with open(compile_commands, encoding="utf-8") as source:
            commands = json.load(source)
    except (OSError, ValueError, TypeError):
        commands = []

    flags = []
    seen = set()
    for entry in commands:
        command = entry.get("arguments") or entry.get("command")
        if not command:
            continue
        try:
            tokens = list(command) if isinstance(command, list) else shlex.split(command)
        except ValueError:
            continue
        skip_next = False
        for token in tokens[1:]:
            if skip_next:
                skip_next = False
                continue
            if token == "-o":
                skip_next = True
                continue
            if token.startswith("-") and token not in seen:
                flags.append(token)
                seen.add(token)
    if flags:
        return " ".join(flags)
    return os.environ.get("CFLAGS", "unknown")


def hardware_fingerprint():
    records = _cpuinfo_records()
    logical_cores = os.cpu_count() or len(records) or 1
    model = next((record.get("model name") or record.get("Processor")
                  for record in records
                  if record.get("model name") or record.get("Processor")), "unknown")
    try:
        cpu_mhz = float(next(record["cpu MHz"] for record in records if "cpu MHz" in record))
    except (StopIteration, ValueError):
        cpu_mhz = 0.0

    physical_pairs = {
        (record.get("physical id"), record.get("core id"))
        for record in records
        if record.get("physical id") is not None and record.get("core id") is not None
    }
    if physical_pairs:
        physical_cores = len(physical_pairs)
    else:
        core_counts = [record.get("cpu cores") for record in records if record.get("cpu cores")]
        socket_ids = {
            record.get("physical id") for record in records
            if record.get("physical id") is not None
        }
        try:
            physical_cores = (
                int(core_counts[0]) * max(1, len(socket_ids))
                if core_counts else logical_cores
            )
        except ValueError:
            physical_cores = logical_cores

    try:
        nofile_soft, nofile_hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    except (AttributeError, OSError, ValueError):
        nofile_soft, nofile_hard = 0, 0
    try:
        page_size_kb = int(os.sysconf("SC_PAGE_SIZE")) // 1024
    except (AttributeError, OSError, ValueError):
        page_size_kb = 0

    fingerprint = {
        "host_name": platform.node() or "unknown",
        "cpu_model": model,
        "cpu_logical_cores": logical_cores,
        "cpu_physical_cores": physical_cores,
        "cpu_mhz": round(cpu_mhz, 3),
        "cpu_max_mhz": _read_cpu_max_mhz(),
        "memory_total_kb": _read_mem_total_kb(),
        "os_kernel": platform.release() or "unknown",
        "compiler_flags": _read_compiler_flags(),
        "ulimit_nofile_soft": nofile_soft,
        "ulimit_nofile_hard": nofile_hard,
        "page_size_kb": page_size_kb,
    }
    machine_material = "|".join(str(fingerprint[key]) for key in (
        "cpu_model", "cpu_logical_cores", "cpu_physical_cores",
        "memory_total_kb", "page_size_kb",
    ))
    fingerprint["machine_id"] = hashlib.sha256(
        machine_material.encode("utf-8", "replace")
    ).hexdigest()[:16]
    return fingerprint


class RequestError(Exception):
    category = "framing"


class ConnectError(RequestError):
    category = "connect"


class SendError(RequestError):
    category = "send"


class ReceiveError(RequestError):
    category = "receive"


class FramingError(RequestError):
    category = "framing"


class TimeoutError_(RequestError):
    category = "timeout"


class StatusError(RequestError):
    category = "status"


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(math.ceil(fraction * len(ordered))) - 1)
    return ordered[index]


def _calibration_payload():
    """Choose a fixed hash input whose one-shot cost is below 50 ms."""
    size = CALIBRATION_PAYLOAD_MAX
    while True:
        payload = b"http-server-benchmark\\0" * max(1, size // 23)
        started = time.perf_counter()
        hashlib.sha256(payload).digest()
        elapsed = time.perf_counter() - started
        if elapsed < 0.05 or size <= CALIBRATION_PAYLOAD_MIN:
            return payload
        size //= 2


def _calibrate_ops_per_second(seconds=CALIBRATION_DEFAULT_SECONDS):
    if seconds <= 0:
        raise ValueError("calibration duration must be greater than zero")
    payload = _calibration_payload()
    deadline = time.perf_counter() + seconds
    operations = 0
    while True:
        hashlib.sha256(payload).digest()
        operations += 1
        if time.perf_counter() >= deadline:
            break
    elapsed = max(1e-9, seconds + (time.perf_counter() - deadline))
    return operations / elapsed


def calibrate(seconds=CALIBRATION_DEFAULT_SECONDS):
    """Return the machine speed index for the fixed SHA-256 workload."""
    return _calibrate_ops_per_second(seconds) / CALIBRATION_REFERENCE_OPS_PER_SEC


def _calibration_cache_path(machine_id):
    cache_root = os.environ.get("XDG_CACHE_HOME")
    if not cache_root:
        cache_root = os.path.join(os.path.expanduser("~"), ".cache")
    return os.path.join(cache_root, "http_server_bench", machine_id + ".json")


def _calibration_record(machine_id, seconds, operations_per_second):
    return {
        "version": CALIBRATION_CACHE_VERSION,
        "machine_id": machine_id,
        "algorithm": CALIBRATION_ALGORITHM,
        "seconds": round(seconds, 3),
        "operations_per_second": round(operations_per_second, 3),
        "reference_operations_per_second": CALIBRATION_REFERENCE_OPS_PER_SEC,
        "machine_index": operations_per_second / CALIBRATION_REFERENCE_OPS_PER_SEC,
    }


def _load_calibration_cache(machine_id):
    path = _calibration_cache_path(machine_id)
    try:
        with open(path, encoding="utf-8") as source:
            record = json.load(source)
    except (OSError, ValueError, TypeError):
        return None
    if not isinstance(record, dict):
        return None
    try:
        reference_operations_per_second = float(
            record.get("reference_operations_per_second", 0.0)
        )
    except (TypeError, ValueError):
        return None
    if (
        record.get("version") != CALIBRATION_CACHE_VERSION
        or record.get("machine_id") != machine_id
        or record.get("algorithm") != CALIBRATION_ALGORITHM
        or reference_operations_per_second != CALIBRATION_REFERENCE_OPS_PER_SEC
    ):
        return None
    try:
        operations_per_second = float(record["operations_per_second"])
    except (KeyError, TypeError, ValueError):
        return None
    if operations_per_second <= 0:
        return None
    return record


def _save_calibration_cache(record):
    path = _calibration_cache_path(record["machine_id"])
    temporary = path + ".tmp-%d" % os.getpid()
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(temporary, "w", encoding="utf-8") as output:
            json.dump(record, output, sort_keys=True)
            output.write("\n")
        os.replace(temporary, path)
    except OSError:
        try:
            os.remove(temporary)
        except OSError:
            pass


def calibration_for_machine(hardware, mode="on", force=False,
                            seconds=CALIBRATION_DEFAULT_SECONDS):
    """Return calibration metadata, using a per-machine cache when enabled."""
    base = {
        "machine_id": hardware.get("machine_id", "unknown"),
        "algorithm": CALIBRATION_ALGORITHM,
        "seconds": 0.0,
        "operations_per_second": 0.0,
        "reference_operations_per_second": CALIBRATION_REFERENCE_OPS_PER_SEC,
        "machine_index": 0.0,
        "calibrated": False,
    }
    if mode == "off":
        return base
    if mode not in ("on", "only"):
        raise ValueError("calibration mode must be on, off, or only")
    record = None if force else _load_calibration_cache(base["machine_id"])
    if record is None:
        operations_per_second = _calibrate_ops_per_second(seconds)
        record = _calibration_record(
            base["machine_id"], seconds, operations_per_second
        )
        _save_calibration_cache(record)
    result = dict(base)
    result.update({
        "seconds": float(record.get("seconds", seconds)),
        "operations_per_second": float(record["operations_per_second"]),
        "machine_index": float(record["operations_per_second"]) /
        CALIBRATION_REFERENCE_OPS_PER_SEC,
        "calibrated": True,
    })
    return result


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


def read_exact(sock, size):
    result = bytearray()
    while len(result) < size:
        try:
            chunk = sock.recv(min(65536, size - len(result)))
        except socket.timeout:
            raise TimeoutError_("timeout while reading response body")
        except OSError as exc:
            raise ReceiveError("receive failed: %s" % exc)
        if not chunk:
            raise FramingError("connection closed before Content-Length")
        result.extend(chunk)
    return bytes(result)


def read_response(sock):
    head = bytearray()
    while b"\r\n\r\n" not in head:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            raise TimeoutError_("timeout while reading response headers")
        except OSError as exc:
            raise ReceiveError("receive failed: %s" % exc)
        if not chunk:
            raise FramingError("connection closed before response headers")
        head.extend(chunk)
        if len(head) > 65536:
            raise FramingError("response headers exceed 64 KiB")

    header_bytes, remainder = bytes(head).split(b"\r\n\r\n", 1)
    lines = header_bytes.split(b"\r\n")
    try:
        version, status_text, _reason = lines[0].decode("ascii").split(" ", 2)
        status = int(status_text)
    except (ValueError, UnicodeDecodeError):
        raise FramingError("malformed HTTP status line")
    if version != "HTTP/1.1":
        raise FramingError("unexpected HTTP version")

    headers = {}
    for line in lines[1:]:
        if b":" not in line:
            raise FramingError("malformed response header")
        name, value = line.split(b":", 1)
        headers[name.decode("ascii").lower()] = value.strip().decode("ascii").lower()

    if "content-length" in headers:
        try:
            length = int(headers["content-length"])
        except ValueError:
            raise FramingError("invalid Content-Length")
        if length < 0:
            raise FramingError("negative Content-Length")
        body = remainder + read_exact(sock, max(0, length - len(remainder)))
        if len(body) != length:
            raise FramingError("response body length mismatch")
    elif headers.get("transfer-encoding") == "chunked":
        body = bytearray()
        pending = bytearray(remainder)
        while True:
            while b"\r\n" not in pending:
                try:
                    chunk = sock.recv(4096)
                except socket.timeout:
                    raise TimeoutError_("timeout while reading chunk header")
                except OSError as exc:
                    raise ReceiveError("receive failed: %s" % exc)
                if not chunk:
                    raise FramingError("connection closed before chunk header")
                pending.extend(chunk)
            line, _, pending = pending.partition(b"\r\n")
            try:
                chunk_size = int(line.split(b";", 1)[0], 16)
            except ValueError:
                raise FramingError("invalid chunk size")
            if chunk_size == 0:
                while len(pending) < 2:
                    try:
                        chunk = sock.recv(4096)
                    except socket.timeout:
                        raise TimeoutError_("timeout while reading chunk trailer")
                    except OSError as exc:
                        raise ReceiveError("receive failed: %s" % exc)
                    if not chunk:
                        raise FramingError("connection closed before chunk trailer")
                    pending.extend(chunk)
                if pending[:2] != b"\r\n":
                    raise FramingError("invalid chunk terminator")
                break
            while len(pending) < chunk_size + 2:
                try:
                    chunk = sock.recv(4096)
                except socket.timeout:
                    raise TimeoutError_("timeout while reading chunk body")
                except OSError as exc:
                    raise ReceiveError("receive failed: %s" % exc)
                if not chunk:
                    raise FramingError("connection closed inside chunk")
                pending.extend(chunk)
            body.extend(pending[:chunk_size])
            if pending[chunk_size:chunk_size + 2] != b"\r\n":
                raise FramingError("invalid chunk terminator")
            del pending[:chunk_size + 2]
    elif headers.get("connection") == "close" or version == "HTTP/1.0":
        # A close-delimited body is valid HTTP/1.1 when the server signals that
        # it will close the connection; used by the error templates.
        body = bytearray(remainder)
        while True:
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                raise TimeoutError_("timeout while reading close-delimited body")
            except OSError as exc:
                raise ReceiveError("receive failed: %s" % exc)
            if not chunk:
                break
            body.extend(chunk)
    else:
        raise FramingError("response has neither Content-Length nor chunked framing")

    # The server may end a keep-alive connection (e.g. its per-connection
    # request limit) while still delivering a complete, valid response.  The
    # caller must drop the socket instead of sending another request on it.
    should_close = headers.get("connection") == "close" or version == "HTTP/1.0"
    return status, bytes(body), should_close


def send_request(args, spec, sock=None, counters=None):
    """Send one request, return (status, body, server_keep_alive).

    Counts connection attempts."""
    own_socket = sock is None
    if own_socket:
        if counters is not None:
            counters["connections"] += 1
        try:
            sock = socket.create_connection((args.host, args.port), args.timeout)
        except OSError as exc:
            raise ConnectError("connect failed: %s" % exc)
        sock.settimeout(args.timeout)
    request = (
        "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: %s\r\n\r\n"
        % (spec["method"], spec["path"], args.host,
           "keep-alive" if args.keep_alive else "close")
    )
    if spec.get("gzip"):
        request = request.replace(
            "\r\n\r\n", "\r\nAccept-Encoding: gzip\r\n\r\n"
        )
    try:
        try:
            sock.sendall(request.encode("ascii"))
        except socket.timeout:
            raise TimeoutError_("timeout while sending request")
        except OSError as exc:
            raise SendError("send failed: %s" % exc)
        status, body, should_close = read_response(sock)
        return status, body, not should_close
    finally:
        if own_socket:
            sock.close()


def worker(args, state, results, worker_id):
    events = []
    counters = {"connections": 0}
    sock = None
    specs = state["specs"]

    def connect():
        counters["connections"] += 1
        new_sock = socket.create_connection((args.host, args.port), args.timeout)
        new_sock.settimeout(args.timeout)
        return new_sock

    if args.keep_alive:
        try:
            sock = connect()
        except OSError as exc:
            state["startup_error"] = "connect failed: %s" % exc
            return

    while True:
        with state["lock"]:
            if state["next_request"] >= state["request_limit"]:
                break
            request_number = state["next_request"]
            state["next_request"] += 1
        due = state["start"] + request_number / args.rate if args.rate > 0 else time.monotonic()
        # Do not turn a slow server into a burst-and-catch-up pass. Requests
        # whose fixed-rate due time is outside the measurement window are not
        # offered; requests already in flight are allowed to drain.
        if due >= state["steady_end"]:
            break
        delay = due - time.monotonic()
        if delay > 0:
            time.sleep(delay)

        spec = specs[request_number % len(specs)]
        started = time.monotonic()
        status = None
        error = None
        server_keep_alive = True
        connection_lost = False
        try:
            status, body, server_keep_alive = send_request(args, spec, sock, counters)
            connection_lost = not server_keep_alive
            if status not in spec["expect"]:
                error = "status"
            elif status == 200 and not body:
                error = "framing"
        except RequestError as exc:
            error = exc.category
            connection_lost = True
        except OSError:
            error = "other"
            connection_lost = True
        completed = time.monotonic()

        # A server that ends the connection after a complete response (its
        # keep-alive request limit, or an HTTP/1.0 close) is not a failure;
        # drop the socket and reconnect rather than reusing a dead one.
        if sock is not None and connection_lost:
            try:
                sock.close()
            except OSError:
                pass
            sock = None
            try:
                sock = connect()
            except OSError:
                pass

        events.append({
            "started": started, "completed": completed,
            "status": status, "error": error,
            "latency_ms": (completed - started) * 1000.0,
        })

    if sock is not None:
        sock.close()

    with state["results_lock"]:
        results.append((events, counters["connections"]))


# --- Server process monitoring (CPU, RSS, fds, context switches) ------------

try:
    CLK_TCK = os.sysconf("SC_CLK_TCK")
except (AttributeError, OSError, ValueError):
    CLK_TCK = 100


def read_proc_cpu_seconds(pid):
    if not pid:
        return 0.0
    try:
        with open("/proc/%d/stat" % pid, encoding="utf-8") as source:
            data = source.read()
        end = data.rfind(")")
        fields = data[end + 2:].split()
        return (int(fields[11]) + int(fields[12])) / CLK_TCK
    except (OSError, ValueError, IndexError, ZeroDivisionError):
        return 0.0


def read_proc_tcp_retransmits():
    try:
        with open("/proc/net/snmp") as fh:
            lines = fh.read().splitlines()
    except OSError:
        return None
    keys = None
    for i, line in enumerate(lines):
        if line.startswith("Tcp:") and "RetransSegs" in line:
            keys = line.split()[1:]
            if i + 1 < len(lines):
                values = lines[i + 1].split()[1:]
                if len(values) == len(keys):
                    return int(values[keys.index("RetransSegs")])
    return None


class ServerMonitor:
    def __init__(self, pid, interval=0.25):
        self.pid = pid
        self.interval = interval
        self.samples = []
        self.max_rss_kb = 0
        self.max_open_fds = 0
        self.tcp_start = read_proc_tcp_retransmits()
        self.tcp_end = self.tcp_start
        self.start_time = None
        self.end_time = None
        self._stop = threading.Event()
        self._thread = None

    def _read(self):
        base = "/proc/%d" % self.pid
        try:
            with open(base + "/stat") as fh:
                data = fh.read()
            end = data.rfind(")")
            fields = data[end + 2:].split()
            utime = int(fields[11])
            stime = int(fields[12])
            with open(base + "/status") as fh:
                status = {}
                for line in fh:
                    key, _, value = line.partition(":")
                    status[key.strip()] = value.strip()
            rss_kb = int(status.get("VmRSS", "0 kB").split()[0])
            voluntary = int(status.get("voluntary_ctxt_switches", "0"))
            involuntary = int(status.get("nonvoluntary_ctxt_switches", "0"))
            try:
                open_fds = len(os.listdir(base + "/fd"))
            except OSError:
                open_fds = 0
            return {
                "t": time.monotonic(),
                "cpu_ticks": utime + stime,
                "rss_kb": rss_kb,
                "open_fds": open_fds,
                "voluntary": voluntary,
                "involuntary": involuntary,
            }
        except (OSError, ValueError, IndexError):
            return None

    def _loop(self):
        while not self._stop.wait(self.interval):
            sample = self._read()
            if sample:
                self.samples.append(sample)

    def start(self):
        if not self.pid:
            return
        sample = self._read()
        if sample:
            self.samples.append(sample)
            self.start_time = sample["t"]
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def stop(self):
        if not self.pid:
            return
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)
        sample = self._read()
        if sample:
            self.samples.append(sample)
            self.end_time = sample["t"]
        self.tcp_end = read_proc_tcp_retransmits()
        for sample in self.samples:
            self.max_rss_kb = max(self.max_rss_kb, sample["rss_kb"])
            self.max_open_fds = max(self.max_open_fds, sample["open_fds"])

    def summary(self):
        result = {
            "server_cpu_percent": 0.0,
            "server_cpu_seconds": 0.0,
            "server_cpu_cores": 0.0,
            "server_rss_kb": self.max_rss_kb,
            "server_open_fds": self.max_open_fds,
            "ctx_switches_voluntary": 0,
            "ctx_switches_involuntary": 0,
            "tcp_retransmits": 0,
        }
        if len(self.samples) >= 2:
            first = self.samples[0]
            last = self.samples[-1]
            elapsed = max(1e-6, last["t"] - first["t"])
            cpu_ticks = max(0, last["cpu_ticks"] - first["cpu_ticks"])
            cpu_seconds = cpu_ticks / CLK_TCK
            result["server_cpu_seconds"] = round(cpu_seconds, 6)
            result["server_cpu_cores"] = round(cpu_seconds / elapsed, 6)
            result["server_cpu_percent"] = round(
                100.0 * cpu_seconds / elapsed, 2
            )
            result["ctx_switches_voluntary"] = last["voluntary"] - first["voluntary"]
            result["ctx_switches_involuntary"] = last["involuntary"] - first["involuntary"]
        if self.tcp_start is not None and self.tcp_end is not None:
            result["tcp_retransmits"] = max(0, self.tcp_end - self.tcp_start)
        return result


def read_server_metrics(path):
    defaults = {
        "server_active_workers_max": 0,
        "server_queue_depth_max": 0,
        "server_accepted_connections": 0,
        "server_rejected_tasks": 0,
        "server_completed_requests": 0,
        "server_request_failures": 0,
        "server_connection_capacity": 0,
        "server_active_connections": 0,
        "server_active_connections_max": 0,
        "server_listener_disabled": 0,
        "server_listener_disabled_ms": 0,
        "server_admission_rejected": 0,
        "server_overload_responses": 0,
        "server_connection_resets": 0,
        "server_buffer_bytes_current": 0,
        "server_buffer_bytes_max": 0,
        "server_input_buffer_limit": 0,
        "server_el_wakeups": 0,
        "server_el_readable_events": 0,
        "server_el_writable_events": 0,
        "server_el_eagain": 0,
        "server_el_partial_writes": 0,
        "server_el_deadline_closes": 0,
        "server_el_pipeline_full": 0,
        "server_el_output_drained": 0,
        "server_el_connections_opened": 0,
        "server_el_connections_closed": 0,
        "server_admission_rejected_capacity": 0,
        "server_admission_rejected_table_full": 0,
        "server_backlog_depth": 0,
        "server_backlog_depth_max": 0,
        "server_el_loops": 0,
        "server_el_loop_wakeups": "",
        "server_el_loop_accepted": "",
    }
    if not path or not os.path.exists(path):
        return defaults
    try:
        with open(path) as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return defaults
    mapping = {
        "server_active_workers_max": "active_workers_max",
        "server_queue_depth_max": "queue_depth_max",
        "server_accepted_connections": "accepted_connections",
        "server_rejected_tasks": "rejected_tasks",
        "server_completed_requests": "completed_requests",
        "server_request_failures": "request_failures",
        "server_connection_capacity": "connection_capacity",
        "server_active_connections": "active_connections",
        "server_active_connections_max": "active_connections_max",
        "server_listener_disabled": "listener_disabled_count",
        "server_listener_disabled_ms": "listener_disabled_ms",
        "server_admission_rejected": "admission_rejected",
        "server_overload_responses": "overload_responses",
        "server_connection_resets": "connection_resets",
        "server_buffer_bytes_current": "buffer_bytes_current",
        "server_buffer_bytes_max": "buffer_bytes_max",
        "server_input_buffer_limit": "input_buffer_limit",
        "server_el_wakeups": "el_wakeups",
        "server_el_readable_events": "el_readable_events",
        "server_el_writable_events": "el_writable_events",
        "server_el_eagain": "el_eagain",
        "server_el_partial_writes": "el_partial_writes",
        "server_el_deadline_closes": "el_deadline_closes",
        "server_el_pipeline_full": "el_pipeline_full",
        "server_el_output_drained": "el_output_drained",
        "server_el_connections_opened": "el_connections_opened",
        "server_el_connections_closed": "el_connections_closed",
        "server_admission_rejected_capacity": "admission_rejected_capacity",
        "server_admission_rejected_table_full": "admission_rejected_table_full",
        "server_backlog_depth": "backlog_depth",
        "server_backlog_depth_max": "backlog_depth_max",
        "server_el_loops": "el_loops",
    }
    for field, key in mapping.items():
        if key in data:
            defaults[field] = data[key]
    for field, key in (("server_el_loop_wakeups", "el_loop_wakeups"),
                       ("server_el_loop_accepted", "el_loop_accepted")):
        value = data.get(key)
        if isinstance(value, list):
            defaults[field] = "|".join(str(item) for item in value)
    return defaults


# --- Load generation --------------------------------------------------------


def run_load(args, specs, slow_clients=0):
    start = time.monotonic()
    state = {
        "lock": threading.Lock(),
        "results_lock": threading.Lock(),
        "next_request": 0,
        "request_limit": args.requests,
        "start": start,
        "warmup_end": start + args.warmup,
        "steady_end": start + args.warmup + args.duration,
        "startup_error": None,
        "specs": specs,
    }
    results = []
    threads = []
    barrier = threading.Barrier(args.concurrency)
    idle = []

    if slow_clients:
        for i in range(slow_clients):
            try:
                sock = socket.create_connection((args.host, args.port), args.timeout)
                sock.settimeout(args.timeout)
                if i % 2 == 0:
                    sock.sendall(b"GET /home HTTP/1.1\r\nHost: benchmark\r\n")
                else:
                    spec = {"method": "GET", "path": "/home", "gzip": False}
                    args_keep = args.keep_alive
                    args.keep_alive = True
                    send_request(args, spec, sock)
                    args.keep_alive = args_keep
                idle.append(sock)
            except OSError:
                break

    def run_worker(_worker_id):
        barrier.wait()
        worker(args, state, results, _worker_id)

    for worker_id in range(args.concurrency):
        thread = threading.Thread(target=run_worker, args=(worker_id,))
        thread.start()
        threads.append(thread)
    for thread in threads:
        thread.join()

    elapsed = max(0.000001, time.monotonic() - state["start"])

    for sock in idle:
        try:
            sock.close()
        except OSError:
            pass

    all_events = [event for result in results for event in result[0]]
    steady_events = [event for event in all_events
                     if state["warmup_end"] <= event["started"] < state["steady_end"]
                     and event["completed"] < state["steady_end"]]
    drain_events = [event for event in all_events
                    if event["started"] < state["steady_end"]
                    and event["completed"] >= state["steady_end"]]
    warmup_events = [event for event in all_events
                     if event["started"] < state["warmup_end"]]

    def event_summary(events):
        statuses = collections.Counter(event["status"] for event in events
                                       if event["status"] is not None)
        errors = collections.Counter(event["error"] for event in events
                                     if event["error"] is not None)
        return statuses, errors

    statuses, errors = event_summary(steady_events)
    total_statuses, total_errors = event_summary(all_events)
    connections = sum(result[1] for result in results)
    per_second = {}
    for event in all_events:
        if state["warmup_end"] <= event["started"] < state["steady_end"]:
            second = int(event["started"] - state["warmup_end"])
            bucket = per_second.setdefault(second, {"offered": 0, "completed": 0})
            bucket["offered"] += 1
            if event["completed"] < state["steady_end"]:
                bucket["completed"] += 1

    return {
        "elapsed": elapsed,
        "latencies": [event["latency_ms"] for event in steady_events],
        "statuses": statuses,
        "errors": errors,
        "total_statuses": total_statuses,
        "total_errors": total_errors,
        "connections": connections,
        "warmup_offered": len(warmup_events),
        "steady_offered": len([event for event in all_events
                                if state["warmup_end"] <= event["started"] < state["steady_end"]]),
        "steady_completed": len(steady_events),
        "drain_completed": len(drain_events),
        "per_second": per_second,
        "drain_seconds": max(0.0, time.monotonic() - state["steady_end"]),
        "startup_error": state["startup_error"],
        "idle_connections": len(idle),
    }


def classify_limiter(client_cpu_seconds, server_cpu_seconds, elapsed):
    """Return a coarse saturation verdict; treat it as diagnostic, not a gate."""
    elapsed = max(1e-6, elapsed)
    client_percent = 100.0 * client_cpu_seconds / elapsed
    server_percent = 100.0 * server_cpu_seconds / elapsed
    if server_cpu_seconds <= 0:
        return "unknown"
    if client_percent >= 80.0 and client_percent > server_percent * 1.25:
        return "client"
    if server_percent >= 80.0 and server_percent >= client_percent * 0.75:
        return "server"
    return "neither/unknown"


def summarize(args, spec_names, outcome, server_summary, server_metrics,
              hardware=None, calibration=None):
    elapsed = outcome["elapsed"]
    steady_elapsed = max(0.000001, args.duration)
    latencies = outcome["latencies"]
    statuses = outcome["statuses"]
    errors = outcome["errors"]
    connections = outcome["connections"]

    expected = set()
    for spec in args.specs:
        expected.update(spec["expect"])

    responses_received = sum(
        count for status, count in statuses.items()
        if isinstance(status, int)
    )
    successful = sum(
        count for status, count in statuses.items()
        if isinstance(status, int) and status in expected
    )
    unexpected = responses_received - successful
    failed = unexpected + sum(errors.values())

    status_fields = {
        "status_200": statuses.get(200, 0),
        "status_400": statuses.get(400, 0),
        "status_404": statuses.get(404, 0),
        "status_413": statuses.get(413, 0),
        "status_431": statuses.get(431, 0),
        "status_500": statuses.get(500, 0),
        "status_501": statuses.get(501, 0),
    }
    status_fields["status_other"] = responses_received - sum(status_fields.values())

    result = collections.OrderedDict()
    result["timestamp_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    result["commit_id"] = git_commit_id()
    result["scenario"] = spec_names
    result["host"] = args.host
    result["port"] = args.port
    result["mode"] = "keep-alive" if args.keep_alive else "new-connection"
    result["target_rate"] = args.rate
    result["warmup_seconds"] = round(args.warmup, 3)
    result["steady_state_seconds"] = round(args.duration, 3)
    result["duration_seconds"] = round(args.duration, 3)
    result["drain_seconds"] = round(outcome["drain_seconds"], 3)
    result["concurrency"] = args.concurrency
    result["requests_planned"] = args.requests
    result["warmup_requests_offered"] = outcome["warmup_offered"]
    result["steady_requests_offered"] = outcome["steady_offered"]
    result["unoffered_requests"] = max(
        0, args.requests - outcome["warmup_offered"] - outcome["steady_offered"]
    )
    result["steady_responses_completed"] = outcome["steady_completed"]
    result["drain_responses_completed"] = outcome["drain_completed"]
    result["total_responses_completed"] = sum(outcome["total_statuses"].values())
    result["total_failed"] = (
        sum(outcome["total_errors"].values()) + sum(
            count for status, count in outcome["total_statuses"].items()
            if status not in expected
        )
    )
    result["per_second_rates"] = json.dumps({
        str(second): outcome["per_second"][second]
        for second in sorted(outcome["per_second"])
    }, sort_keys=True, separators=(",", ":"))
    result["responses_received"] = responses_received
    result["successful"] = successful
    result["failed"] = failed
    result["throughput_rps"] = round(responses_received / steady_elapsed, 2)
    result["successful_rps"] = round(successful / steady_elapsed, 2)
    result["offered_rps"] = args.rate
    result["connection_rate"] = round(connections / steady_elapsed, 2)
    if args.keep_alive and connections:
        result["keepalive_request_rate"] = round(successful / steady_elapsed, 2)
        result["requests_per_connection"] = round(responses_received / connections, 2)
    else:
        result["keepalive_request_rate"] = 0.0
        result["requests_per_connection"] = (
            round(responses_received / connections, 2) if connections else 0.0
        )
    result.update(status_fields)
    result["latency_min_ms"] = round(min(latencies) if latencies else 0.0, 3)
    result["latency_p50_ms"] = round(percentile(latencies, 0.50), 3)
    result["latency_p95_ms"] = round(percentile(latencies, 0.95), 3)
    result["latency_p99_ms"] = round(percentile(latencies, 0.99), 3)
    result["latency_max_ms"] = round(max(latencies) if latencies else 0.0, 3)
    result["fail_connect"] = errors.get("connect", 0)
    result["fail_send"] = errors.get("send", 0)
    result["fail_receive"] = errors.get("receive", 0)
    result["fail_framing"] = errors.get("framing", 0)
    result["fail_status"] = errors.get("status", 0)
    result["fail_timeout"] = errors.get("timeout", 0)
    result["fail_other"] = errors.get("other", 0)
    result.update(server_summary)
    result.update(server_metrics)

    hardware = hardware or hardware_fingerprint()
    calibration = calibration or {
        "machine_index": 0.0,
        "operations_per_second": 0.0,
        "reference_operations_per_second": CALIBRATION_REFERENCE_OPS_PER_SEC,
        "algorithm": CALIBRATION_ALGORITHM,
        "seconds": 0.0,
        "calibrated": False,
    }
    server_cpu_seconds = float(result.get("server_cpu_seconds", 0.0) or 0.0)
    server_completed = int(result.get("server_completed_requests", 0) or 0)
    if server_completed <= 0:
        server_completed = responses_received
    server_cpu_cores = server_cpu_seconds / max(1e-6, elapsed)
    client_cpu_seconds = float(outcome.get("client_cpu_seconds", 0.0) or 0.0)
    result["server_cpu_seconds"] = round(server_cpu_seconds, 6)
    result["server_cpu_seconds_per_1000_requests"] = round(
        server_cpu_seconds / server_completed * 1000.0, 6
    ) if server_completed else 0.0
    result["rps_per_server_cpu_second"] = round(
        successful / server_cpu_seconds, 3
    ) if server_cpu_seconds > 0 else 0.0
    result["server_cpu_cores"] = round(server_cpu_cores, 6)
    result["client_cpu_seconds"] = round(client_cpu_seconds, 6)
    result["client_cpu_percent"] = round(
        100.0 * client_cpu_seconds / max(1e-6, elapsed), 2
    )
    result["hardware_agnostic_rps"] = result["rps_per_server_cpu_second"]
    result["limited_by"] = classify_limiter(
        client_cpu_seconds, server_cpu_seconds, elapsed
    )

    machine_index = float(calibration.get("machine_index", 0.0) or 0.0)
    calibrated = bool(calibration.get("calibrated", False)) and machine_index > 0
    result["machine_index"] = round(machine_index, 6) if calibrated else 0.0
    result["calibration_ops_per_sec"] = round(float(calibration.get(
        "operations_per_second", 0.0
    ) or 0.0), 3)
    result["calibration_reference_ops_per_sec"] = (
        CALIBRATION_REFERENCE_OPS_PER_SEC
    )
    result["calibration_algorithm"] = calibration.get(
        "algorithm", CALIBRATION_ALGORITHM
    )
    result["calibration_seconds"] = round(float(calibration.get(
        "seconds", 0.0
    ) or 0.0), 3)
    result["calibrated"] = calibrated
    if calibrated:
        result["throughput_rps_normalized"] = round(
            result["throughput_rps"] / machine_index, 3
        )
        result["successful_rps_normalized"] = round(
            result["successful_rps"] / machine_index, 3
        )
        result["latency_p50_ms_normalized"] = round(
            result["latency_p50_ms"] * machine_index, 3
        )
        result["latency_p95_ms_normalized"] = round(
            result["latency_p95_ms"] * machine_index, 3
        )
        result["latency_p99_ms_normalized"] = round(
            result["latency_p99_ms"] * machine_index, 3
        )
    else:
        result["throughput_rps_normalized"] = 0.0
        result["successful_rps_normalized"] = 0.0
        result["latency_p50_ms_normalized"] = 0.0
        result["latency_p95_ms_normalized"] = 0.0
        result["latency_p99_ms_normalized"] = 0.0
    result.update(hardware)
    return result


# --- Result logging ---------------------------------------------------------


def _migrate_csv_schema(path, existing_fields):
    """Append the new schema without corrupting rows written by old clients."""
    if not existing_fields:
        return
    if not set(existing_fields).issubset(set(CSV_FIELDS)):
        raise ValueError("result CSV contains unknown columns")
    temporary = path + ".tmp-%d" % os.getpid()
    try:
        with open(path, "r", newline="", encoding="utf-8") as source:
            reader = csv.DictReader(source)
            with open(temporary, "w", newline="", encoding="utf-8") as output:
                writer = csv.DictWriter(output, fieldnames=CSV_FIELDS)
                writer.writeheader()
                for row in reader:
                    writer.writerow(row)
        os.replace(temporary, path)
    except OSError:
        try:
            os.remove(temporary)
        except OSError:
            pass
        raise


def log_result(path, result):
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    if path.endswith(".json") or path.endswith(".jsonl"):
        with open(path, "a", encoding="utf-8") as output:
            output.write(json.dumps(result) + "\n")
    else:
        write_header = not os.path.exists(path) or os.path.getsize(path) == 0
        if not write_header:
            with open(path, "r", newline="", encoding="utf-8") as existing:
                existing_fields = next(csv.reader(existing), [])
            if existing_fields != list(CSV_FIELDS):
                _migrate_csv_schema(path, existing_fields)
        with open(path, "a", newline="", encoding="utf-8") as output:
            writer = csv.DictWriter(output, fieldnames=CSV_FIELDS)
            if write_header:
                writer.writeheader()
            writer.writerow(result)


def wait_for_server(host, port, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), 0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start on %s:%d" % (host, port))


def port_in_use(host, port):
    try:
        with socket.create_connection((host, port), 0.2):
            return True
    except OSError:
        return False


def build_specs(args):
    if args.expect_status:
        expected = [int(code) for code in args.expect_status.split(",") if code]
    else:
        expected = None
    specs = []
    base_gzip = args.gzip if args.gzip is not None else False
    base_method = args.method or "GET"
    if args.paths:
        for path in args.paths:
            specs.append({
                "method": base_method,
                "path": path,
                "gzip": base_gzip,
                "expect": expected or [200],
            })
    return specs, expected


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8081)
    parser.add_argument("--scenario", choices=sorted(SCENARIOS.keys()),
                        help="run one of the scenarios from the measurement contract")
    parser.add_argument("--path", dest="paths", action="append",
                        help="request path; repeat for a path mix")
    parser.add_argument("--method", default=None, help="HTTP method (default GET)")
    parser.add_argument("--duration", type=float, default=None,
                        help="steady-state measurement duration in seconds")
    parser.add_argument("--warmup", type=float, default=None,
                        help="fixed-rate warmup duration in seconds (default 10)")
    parser.add_argument("--rate", type=float, default=None,
                        help="target request rate; 0 means closed-loop")
    parser.add_argument("--requests", type=int, default=0,
                        help="total requests (defaults to warmup plus steady rate window)")
    parser.add_argument("--concurrency", type=int, default=None)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--min-rps", type=float, default=0.0,
                        help="minimum raw successful throughput (legacy gate)")
    parser.add_argument("--min-hardware-agnostic-rps", type=float, default=0.0,
                        help="minimum successful requests per server CPU-second")
    parser.add_argument("--min-normalized-rps", type=float, default=0.0,
                        help="minimum calibration-normalized successful throughput")
    parser.add_argument("--expect-status", default=None,
                        help="comma-separated expected status codes (default 200)")
    parser.add_argument("--log-file", help="append results and commit ID to CSV or JSON")
    parser.add_argument("--keep-alive", action="store_true", default=None)
    parser.add_argument("--gzip", action="store_true", default=None,
                        help="send Accept-Encoding: gzip")
    parser.add_argument("--slow-clients", type=int, default=None,
                        help="number of idle/partial connections held during the run")
    parser.add_argument("--start-server", action="store_true")
    parser.add_argument("--server-pid", type=int, default=None,
                        help="sample an already-running server process")
    parser.add_argument("--server", default="./bin/http_server")
    parser.add_argument("--server-metrics-file", default=None,
                        help="JSON metrics snapshot written by the server")
    parser.add_argument("--print-hardware", action="store_true",
                        help="print hardware metadata and exit without load")
    parser.add_argument("--calibrate", choices=("on", "off", "only"), default="on",
                        help="calibrate the host, skip calibration, or calibrate and exit")
    parser.add_argument("--calibrate-force", action="store_true",
                        help="ignore the cached calibration for this machine")
    parser.add_argument("--calibration-seconds", type=float,
                        default=CALIBRATION_DEFAULT_SECONDS,
                        help="calibration duration when no cache is available")
    args = parser.parse_args(argv)

    scenario = SCENARIOS.get(args.scenario, {}) if args.scenario else {}

    def pick(value, key, fallback):
        if value is not None:
            return value
        if key in scenario:
            return scenario[key]
        return fallback

    args.rate = pick(args.rate, "rate", 100.0)
    args.duration = pick(args.duration, "duration", 10.0)
    args.warmup = pick(args.warmup, "warmup", 10.0)
    args.concurrency = pick(args.concurrency, "concurrency", 16)
    args.keep_alive = bool(pick(args.keep_alive, "keep_alive", False))
    args.gzip = bool(pick(args.gzip, "gzip", False))
    args.slow_clients = pick(args.slow_clients, "slow_clients", 0)
    args.diagnostic = bool(scenario.get("diagnostic", False))

    if not args.paths:
        scenario_specs = scenario.get("specs")
        if scenario_specs:
            args.specs = []
            expected = None
            if args.expect_status:
                expected = [int(c) for c in args.expect_status.split(",") if c]
            for spec in scenario_specs:
                merged = {
                    "method": spec.get("method", args.method or "GET"),
                    "path": spec["path"],
                    "gzip": spec.get("gzip", args.gzip),
                    "expect": expected or spec.get("expect", [200]),
                }
                args.specs.append(merged)
        else:
            args.specs = [{
                "method": args.method or "GET",
                "path": "/home",
                "gzip": args.gzip,
                "expect": [int(c) for c in (args.expect_status or "").split(",") if c] or [200],
            }]
    else:
        args.specs, _ = build_specs(args)

    if args.print_hardware or args.calibrate == "only":
        if args.calibration_seconds <= 0:
            parser.error("--calibration-seconds must be greater than zero")
        return args
    if args.concurrency < 1 or args.duration <= 0 or args.warmup < 0 or args.rate < 0:
        parser.error("warmup, duration, rate, and concurrency values are invalid")
    if args.calibration_seconds <= 0:
        parser.error("--calibration-seconds must be greater than zero")
    if args.requests <= 0:
        if args.rate <= 0:
            parser.error("--requests is required when --rate=0")
        args.requests = int(math.ceil(args.rate * (args.warmup + args.duration)))
    return args


def main(argv=None):
    args = parse_args(argv)
    hardware = hardware_fingerprint()
    calibration = None
    server = None
    metrics_file = args.server_metrics_file
    created_metrics_file = None
    monitor = None
    try:
        if args.print_hardware:
            print(json.dumps(hardware, sort_keys=True))
            if args.calibrate != "only":
                return 0
        calibration = calibration_for_machine(
            hardware,
            mode=args.calibrate,
            force=args.calibrate_force,
            seconds=args.calibration_seconds,
        )
        if args.calibrate == "only":
            print(json.dumps({
                "algorithm": calibration["algorithm"],
                "calibrated": calibration["calibrated"],
                "machine_id": hardware["machine_id"],
                "machine_index": calibration["machine_index"],
                "operations_per_second": calibration["operations_per_second"],
                "reference_operations_per_second": (
                    calibration["reference_operations_per_second"]
                ),
                "seconds": calibration["seconds"],
            }, sort_keys=True))
            return 0

        if not metrics_file:
            fd, metrics_file = tempfile.mkstemp(prefix="http_server_metrics_", suffix=".json")
            os.close(fd)
            created_metrics_file = metrics_file

        if args.start_server:
            if port_in_use(args.host, args.port):
                raise RuntimeError(
                    "a server is already listening on %s:%d; stop it or drop "
                    "--start-server" % (args.host, args.port))
            env = dict(os.environ)
            env["HTTP_SERVER_ACCESS_LOG"] = "0"
            env["HTTP_SERVER_METRICS_FILE"] = metrics_file
            server = subprocess.Popen(
                [args.server], cwd=os.getcwd(), env=env,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            wait_for_server(args.host, args.port, 5.0)
            if server.poll() is not None:
                raise RuntimeError(
                    "server exited during startup (exit code %s)" % server.returncode)
            server_pid = server.pid
        else:
            wait_for_server(args.host, args.port, 1.0)
            server_pid = args.server_pid

        monitor = ServerMonitor(server_pid)
        monitor.start()
        client_cpu_start = read_proc_cpu_seconds(os.getpid())
        outcome = run_load(args, args.specs, slow_clients=args.slow_clients)
        client_cpu_end = read_proc_cpu_seconds(os.getpid())
        outcome["client_cpu_seconds"] = max(0.0, client_cpu_end - client_cpu_start)
        monitor.stop()

        server_summary = monitor.summary()
        server_metrics = read_server_metrics(metrics_file)
        result = summarize(args, args.scenario or "custom", outcome,
                           server_summary, server_metrics, hardware, calibration)

        print("requests=%d warmup=%d steady=%d drain=%d failed=%d" %
              (args.requests, result["warmup_requests_offered"],
               result["steady_responses_completed"],
               result["drain_responses_completed"], result["failed"]))
        print("phases=warmup:%.3fs steady:%.3fs drain:%.3fs" %
              (result["warmup_seconds"], result["steady_state_seconds"],
               result["drain_seconds"]))
        print("throughput=%.2f req/s successful=%.2f req/s target=%.2f req/s mode=%s" %
              (result["throughput_rps"], result["successful_rps"], args.rate,
               result["mode"]))
        print("latency_ms min=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f" %
              (result["latency_min_ms"], result["latency_p50_ms"],
               result["latency_p95_ms"], result["latency_p99_ms"],
               result["latency_max_ms"]))
        print("statuses=%s failures=%s" % (
            dict(sorted((k, v) for k, v in outcome["statuses"].items() if isinstance(k, int))),
            dict(sorted(outcome["errors"].items()))))
        print("connections=%d conn_rate=%.2f req_per_conn=%.2f server_cpu=%.2f%% "
              "server_rss=%dkB server_fds=%d queue_depth_max=%d rejected=%d" % (
                  outcome["connections"], result["connection_rate"],
                  result["requests_per_connection"], result["server_cpu_percent"],
                  result["server_rss_kb"], result["server_open_fds"],
                  result["server_queue_depth_max"], result["server_rejected_tasks"]))
        print("hardware_agnostic_rps=%.3f cpu_seconds_per_1000_requests=%.6f "
              "client_cpu=%.2f%% limited_by=%s" % (
                  result["hardware_agnostic_rps"],
                  result["server_cpu_seconds_per_1000_requests"],
                  result["client_cpu_percent"], result["limited_by"]))
        print("calibration=%s machine_index=%.6f normalized_successful_rps=%.3f" % (
            "on" if result["calibrated"] else "off",
            result["machine_index"], result["successful_rps_normalized"]))

        if args.log_file:
            try:
                log_result(args.log_file, result)
                print("result_log=%s" % args.log_file)
            except OSError as exc:
                print("FAIL: could not write result log: %s" % exc, file=sys.stderr)
                return 2

        if outcome["startup_error"]:
            print("FAIL: %s" % outcome["startup_error"], file=sys.stderr)
            return 1
        if not args.diagnostic and result["failed"]:
            print("FAIL: %d requests failed" % result["failed"], file=sys.stderr)
            return 1
        if args.min_rps and result["successful_rps"] < args.min_rps:
            print("FAIL: throughput %.2f is below minimum %.2f req/s" %
                  (result["successful_rps"], args.min_rps), file=sys.stderr)
            return 1
        if (args.min_hardware_agnostic_rps and
                result["hardware_agnostic_rps"] < args.min_hardware_agnostic_rps):
            print("FAIL: hardware-agnostic throughput %.3f is below minimum %.3f "
                  "requests per server CPU-second" % (
                      result["hardware_agnostic_rps"],
                      args.min_hardware_agnostic_rps), file=sys.stderr)
            return 1
        if (args.min_normalized_rps and
                (not result["calibrated"] or
                 result["successful_rps_normalized"] < args.min_normalized_rps)):
            print("FAIL: normalized throughput %.3f is below minimum %.3f req/s" %
                  (result["successful_rps_normalized"], args.min_normalized_rps),
                  file=sys.stderr)
            return 1
        return 0
    except (OSError, RuntimeError, ValueError) as exc:
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
        if created_metrics_file and os.path.exists(created_metrics_file):
            os.remove(created_metrics_file)


if __name__ == "__main__":
    sys.exit(main())
