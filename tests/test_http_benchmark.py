#!/usr/bin/env python3
"""Unit tests for the dependency-free benchmark's hardware metrics."""

import csv
import io
import json
import os
import sys
import tempfile
import unittest
from collections import Counter
from contextlib import redirect_stdout
from types import SimpleNamespace
from unittest import mock


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, REPO_ROOT)

from scripts import http_benchmark


class HardwareBenchmarkTests(unittest.TestCase):
    def test_hardware_fingerprint_is_stable_and_complete(self):
        first = http_benchmark.hardware_fingerprint()
        second = http_benchmark.hardware_fingerprint()

        self.assertEqual(first["machine_id"], second["machine_id"])
        self.assertEqual(len(first["machine_id"]), 16)
        for field in (
            "cpu_model", "cpu_logical_cores", "cpu_physical_cores",
            "memory_total_kb", "os_kernel", "compiler_flags",
            "ulimit_nofile_soft", "ulimit_nofile_hard", "page_size_kb",
        ):
            self.assertIn(field, first)

    def test_calibration_returns_positive_index_and_uses_cache(self):
        hardware = {"machine_id": "test-machine"}
        with tempfile.TemporaryDirectory() as cache:
            with mock.patch.dict(os.environ, {"XDG_CACHE_HOME": cache}):
                fresh = http_benchmark.calibration_for_machine(
                    hardware, mode="on", force=True, seconds=0.01
                )
                cached = http_benchmark.calibration_for_machine(
                    hardware, mode="on", force=False, seconds=0.01
                )

            self.assertTrue(fresh["calibrated"])
            self.assertGreater(fresh["machine_index"], 0.0)
            self.assertEqual(
                fresh["operations_per_second"], cached["operations_per_second"]
            )
            cache_path = os.path.join(cache, "http_server_bench", "test-machine.json")
            with open(cache_path, encoding="utf-8") as source:
                record = json.load(source)
            self.assertEqual(record["machine_id"], "test-machine")

    def test_summary_exposes_hardware_agnostic_metric_and_normalization(self):
        args = SimpleNamespace(
            specs=[{"expect": [200]}],
            host="127.0.0.1",
            port=8080,
            keep_alive=False,
            rate=100.0,
            concurrency=4,
            requests=100,
        )
        outcome = {
            "elapsed": 2.0,
            "latencies": [1.0, 2.0, 4.0, 8.0],
            "statuses": Counter({200: 100}),
            "errors": Counter(),
            "connections": 100,
            "client_cpu_seconds": 0.25,
        }
        server_summary = {
            "server_cpu_percent": 50.0,
            "server_cpu_seconds": 1.0,
            "server_cpu_cores": 0.5,
            "server_rss_kb": 1,
            "server_open_fds": 2,
            "ctx_switches_voluntary": 0,
            "ctx_switches_involuntary": 0,
            "tcp_retransmits": 0,
        }
        server_metrics = {
            "server_completed_requests": 100,
            "server_request_failures": 0,
            "server_active_workers_max": 1,
            "server_queue_depth_max": 0,
            "server_accepted_connections": 100,
            "server_rejected_tasks": 0,
        }
        calibration = {
            "machine_index": 2.0,
            "operations_per_second": 20000.0,
            "reference_operations_per_second": 10000.0,
            "algorithm": "sha256-ramped-buffer",
            "seconds": 2.0,
            "calibrated": True,
        }
        hardware = {"machine_id": "test-machine", "cpu_logical_cores": 2}

        result = http_benchmark.summarize(
            args, "test", outcome, server_summary, server_metrics,
            hardware, calibration
        )

        self.assertEqual(result["hardware_agnostic_rps"], 100.0)
        self.assertEqual(result["server_cpu_seconds_per_1000_requests"], 10.0)
        self.assertEqual(result["throughput_rps_normalized"], 25.0)
        self.assertEqual(result["successful_rps_normalized"], 25.0)
        self.assertEqual(result["latency_p99_ms_normalized"], 16.0)
        self.assertEqual(result["machine_id"], "test-machine")

    def test_legacy_csv_is_migrated_before_new_result_is_appended(self):
        old_fields = list(
            http_benchmark.CSV_FIELDS[:http_benchmark.CSV_FIELDS.index("server_cpu_seconds")]
        )
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False,
                                         newline="") as output:
            path = output.name
            writer = csv.DictWriter(output, fieldnames=old_fields)
            writer.writeheader()
            writer.writerow({field: "old" for field in old_fields})

        try:
            result = {field: "" for field in http_benchmark.CSV_FIELDS}
            result["scenario"] = "new"
            http_benchmark.log_result(path, result)
            with open(path, newline="", encoding="utf-8") as source:
                rows = list(csv.reader(source))
            self.assertEqual(rows[0], list(http_benchmark.CSV_FIELDS))
            self.assertEqual(len(rows), 3)
            self.assertEqual(rows[1][old_fields.index("scenario")], "old")
            self.assertEqual(rows[2][http_benchmark.CSV_FIELDS.index("scenario")], "new")
        finally:
            os.remove(path)

    def test_calibrate_only_does_not_need_a_server(self):
        with tempfile.TemporaryDirectory() as cache:
            output = io.StringIO()
            with mock.patch.dict(os.environ, {"XDG_CACHE_HOME": cache}):
                with redirect_stdout(output):
                    exit_code = http_benchmark.main([
                        "--calibrate", "only",
                        "--calibrate-force",
                        "--calibration-seconds", "0.01",
                    ])
            self.assertEqual(exit_code, 0)
            record = json.loads(output.getvalue().splitlines()[-1])
            self.assertTrue(record["calibrated"])
            self.assertGreater(record["machine_index"], 0.0)


if __name__ == "__main__":
    unittest.main()
