#!/usr/bin/env python3
"""E2E tests for the deterministic benchmark corpus generator.

These drive the real generator against a temporary directory and assert on the
bytes it writes and the manifest it emits: the manifest must describe exactly
the files on disk (size and SHA-256), generation must be reproducible for a
fixed seed, verification must catch corruption, and the class taxonomy must stay
aligned with the server's cached-vs-streamed boundary.
"""

import hashlib
import json
import os
import sys
import tempfile
import unittest


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, REPO_ROOT)

from scripts import benchmark_corpus


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


class BenchmarkCorpusTests(unittest.TestCase):
    def test_generate_manifest_describes_files_on_disk(self):
        with tempfile.TemporaryDirectory() as root:
            manifest = benchmark_corpus.generate(root, seed=1234)

            self.assertEqual(manifest["schema_version"],
                             benchmark_corpus.SCHEMA_VERSION)
            self.assertGreater(len(manifest["entries"]), 0)

            total = 0
            for entry in manifest["entries"]:
                path = os.path.join(root, entry["name"])
                self.assertTrue(os.path.isfile(path), entry["name"])
                self.assertEqual(os.path.getsize(path), entry["bytes"])
                self.assertEqual(sha256_file(path), entry["sha256"])
                total += entry["bytes"]
            self.assertEqual(manifest["total_bytes"], total)

            self.assertEqual(benchmark_corpus.verify(root), [])

    def test_generation_is_reproducible_for_a_seed(self):
        manifests = []
        for _ in range(2):
            with tempfile.TemporaryDirectory() as root:
                manifests.append(benchmark_corpus.generate(root, seed=7))

        first, second = manifests
        self.assertEqual(
            {e["name"]: e["sha256"] for e in first["entries"]},
            {e["name"]: e["sha256"] for e in second["entries"]},
        )

        with tempfile.TemporaryDirectory() as root:
            other = benchmark_corpus.generate(root, seed=8)
        first_hashes = {e["name"]: e["sha256"] for e in first["entries"]}
        changed = {e["name"] for e in first["entries"] if not e["compressible"]}
        self.assertTrue(changed)
        for entry in other["entries"]:
            if entry["name"] in changed:
                self.assertNotEqual(entry["sha256"], first_hashes[entry["name"]])

    def test_verify_detects_tampering(self):
        with tempfile.TemporaryDirectory() as root:
            manifest = benchmark_corpus.generate(root, seed=99)
            target = manifest["entries"][0]["name"]
            with open(os.path.join(root, target), "r+b") as handle:
                handle.seek(0)
                handle.write(b"\x00\x00\x00\x00")

            mismatches = benchmark_corpus.verify(root)
            self.assertTrue(any(target in m for m in mismatches), mismatches)

    def test_taxonomy_matches_cached_vs_streamed_boundary(self):
        with tempfile.TemporaryDirectory() as root:
            manifest = benchmark_corpus.generate(root, seed=1)
        limit = manifest["cache_max_file_bytes"]

        by_class = {}
        for entry in manifest["entries"]:
            by_class.setdefault(entry["class"], []).append(entry)

        for name in ("tiny", "small", "medium", "binary"):
            self.assertIn(name, by_class)
            for entry in by_class[name]:
                self.assertLess(entry["bytes"], limit,
                                "%s should be cacheable" % entry["name"])
        for name in ("streamed", "large"):
            self.assertIn(name, by_class)
            for entry in by_class[name]:
                self.assertGreaterEqual(entry["bytes"], limit,
                                        "%s should stream" % entry["name"])

        self.assertEqual(set(manifest["traffic_mix"]), set(by_class))
        self.assertAlmostEqual(sum(manifest["traffic_mix"].values()), 1.0, places=6)

    def test_with_gzip_adds_matching_precompressed_siblings(self):
        import gzip
        with tempfile.TemporaryDirectory() as root:
            manifest = benchmark_corpus.generate(root, seed=5, with_gzip=True)
            self.assertTrue(manifest["with_gzip"])

            compressible = [e for e in manifest["entries"] if e["compressible"]]
            self.assertTrue(compressible)
            for entry in manifest["entries"]:
                if not entry["compressible"]:
                    self.assertNotIn("gzip_name", entry)
                    continue
                gz_path = os.path.join(root, entry["gzip_name"])
                self.assertTrue(os.path.isfile(gz_path))
                self.assertEqual(os.path.getsize(gz_path), entry["gzip_bytes"])
                self.assertLess(entry["gzip_bytes"], entry["bytes"])
                with open(os.path.join(root, entry["name"]), "rb") as f:
                    plain = f.read()
                with gzip.open(gz_path, "rb") as f:
                    self.assertEqual(f.read(), plain)

            self.assertEqual(benchmark_corpus.verify(root), [])

    def test_huge_class_is_opt_in(self):
        default_names = {e["name"] for e in benchmark_corpus.plan()}
        huge_names = {e["name"] for e in benchmark_corpus.plan(include_huge=True)}
        huge = huge_names - default_names
        self.assertEqual(len(huge), 1)

        entry = [e for e in benchmark_corpus.plan(include_huge=True)
                 if e["name"] in huge][0]
        self.assertGreaterEqual(entry["bytes"], 256 * 1024 * 1024)
        self.assertEqual(entry["class"], "huge")


if __name__ == "__main__":
    unittest.main()
