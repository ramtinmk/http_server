#!/usr/bin/env python3
"""Deterministic benchmark corpus for file-class-aware comparisons.

A single request-rate number hides the fact that file type selects a different
server code path. This generator materializes a fixed, reproducible document
root plus a manifest so that every server under comparison is driven against
*byte-identical* assets, and results can be reported per class rather than as
one average.

Classes map onto the server's cached-vs-streamed boundary (see
`include/server_config.h`, `CACHE_MAX_FILE_BYTES`):

  tiny      <= 1 KiB, compressible   tiny asset; in-memory write path
  small     ~8 KiB, compressible     cached doc-root asset
  medium    256 KiB, compressible    cached asset; gzip CPU cost
  binary    256 KiB, incompressible  cached incompressible asset
  streamed  2 MiB, incompressible    sendfile streaming path
  large     16 MiB, incompressible   sendfile bandwidth-bound path
  huge      256 MiB, opt-in          sendfile bandwidth-bound path

Content is generated from the seed with hash-based (incompressible) or
token-cycled (compressible) streams, so a fixed seed reproduces the same bytes
on any host and any Python 3.8+. The directory tree defaults to
`benchmarks/corpus/` (git-ignored) and the manifest is the committed-able
description of it; only the manifest is small enough to keep as evidence.

Usage:
  scripts/benchmark_corpus.py --list
  scripts/benchmark_corpus.py --output-dir benchmarks/corpus
  scripts/benchmark_corpus.py --output-dir benchmarks/corpus --with-gzip
  scripts/benchmark_corpus.py --output-dir benchmarks/corpus --verify
"""

import argparse
import gzip
import hashlib
import io
import json
import os
import struct
import sys


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCHEMA_VERSION = 1
GENERATOR_NAME = "benchmark_corpus.py"
DEFAULT_SEED = 0x5EED2024

# Mirrors include/server_config.h: files at or above this size stream with
# sendfile instead of entering the in-memory cache.
CACHE_MAX_FILE_BYTES = 1024 * 1024
MIB = 1024 * 1024

# Per-class metadata: default traffic weight for the weighted rollup and the
# server path the class is intended to exercise.
CLASS_META = {
    "tiny": {
        "weight": 0.40,
        "exercises": "tiny asset; in-memory write path",
    },
    "small": {
        "weight": 0.30,
        "exercises": "cached doc-root asset below CACHE_MAX_FILE_BYTES",
    },
    "medium": {
        "weight": 0.15,
        "exercises": "cached medium asset; gzip CPU cost",
    },
    "binary": {
        "weight": 0.05,
        "exercises": "cached incompressible asset",
    },
    "streamed": {
        "weight": 0.07,
        "exercises": "sendfile streaming path (at/above CACHE_MAX_FILE_BYTES)",
    },
    "large": {
        "weight": 0.03,
        "exercises": "sendfile bandwidth-bound path",
    },
    "huge": {
        "weight": 0.00,
        "exercises": "sendfile bandwidth-bound path (opt-in)",
    },
}

# name, bytes, content_type, compressible, class
_PLAN = (
    ("tiny/hello.html", 256, "text/html", True, "tiny"),
    ("small/page.html", 8 * 1024, "text/html", True, "small"),
    ("medium/app.css", 256 * 1024, "text/css", True, "medium"),
    ("binary/photo.png", 256 * 1024, "image/png", False, "binary"),
    ("streamed/movie.bin", 2 * MIB, "application/octet-stream", False, "streamed"),
    ("large/disk.img", 16 * MIB, "application/octet-stream", False, "large"),
    ("huge/backup.img", 256 * MIB, "application/octet-stream", False, "huge"),
)

_OPT_IN_CLASSES = frozenset({"huge"})

# Comparison ranges are also reported so a caller can cover the partial-read
# path without inventing its own offsets.
_RANGES = (
    {"path": "/streamed/movie.bin", "range": "bytes=0-65535", "length": 65536},
)

# Highly compressible tokens cycled to fill the compressible classes. Content is
# arbitrary but stable; only the size and entropy class matter to the server.
_TOKENS = (
    b"<div class=\"card\">",
    b"<p>alpha bravo charlie delta echo</p>",
    b"body { margin: 0; padding: 0; font-family: sans-serif; }\n",
    b"function render(state) { return state.items.map((i) => i.id); }\n",
)

_CHUNK = 64 * 1024


def plan(include_huge=False):
    """Return the ordered corpus plan (no content hashes)."""
    entries = []
    for name, size, content_type, compressible, class_name in _PLAN:
        if class_name in _OPT_IN_CLASSES and not include_huge:
            continue
        entries.append({
            "name": name,
            "path": "/" + name,
            "class": class_name,
            "bytes": size,
            "content_type": content_type,
            "compressible": compressible,
            "exercises": CLASS_META[class_name]["exercises"],
        })
    return entries


def _incompressible_chunks(seed, length):
    prefix = struct.pack("<Q", seed & 0xFFFFFFFFFFFFFFFF)
    counter = 0
    written = 0
    buffer = bytearray()
    while written < length:
        block = hashlib.sha256(prefix + struct.pack("<Q", counter)).digest()
        take = min(len(block), length - written)
        buffer += block[:take]
        written += take
        counter += 1
        if len(buffer) >= _CHUNK:
            yield bytes(buffer)
            del buffer[:]
    if buffer:
        yield bytes(buffer)


def _compressible_chunks(seed, length):
    index = seed % len(_TOKENS) if _TOKENS else 0
    written = 0
    buffer = bytearray()
    while written < length:
        token = _TOKENS[index % len(_TOKENS)]
        take = min(len(token), length - written)
        buffer += token[:take]
        written += take
        index += 1
        if len(buffer) >= _CHUNK:
            yield bytes(buffer)
            del buffer[:]
    if buffer:
        yield bytes(buffer)


def _chunks(entry, seed):
    if entry["compressible"]:
        return _compressible_chunks(seed, entry["bytes"])
    return _incompressible_chunks(seed, entry["bytes"])


def write_entry(output_dir, entry, seed):
    """Materialize one entry and return its SHA-256 hex digest."""
    path = os.path.join(output_dir, entry["name"])
    os.makedirs(os.path.dirname(path), exist_ok=True)
    digest = hashlib.sha256()
    with open(path, "wb") as handle:
        for chunk in _chunks(entry, seed):
            digest.update(chunk)
            handle.write(chunk)
    return digest.hexdigest()


def _gzip_bytes(entry, seed):
    """Deterministic gzip of a compressible entry (mtime 0, no filename)."""
    buffer = io.BytesIO()
    with gzip.GzipFile(fileobj=buffer, mode="wb", compresslevel=6, mtime=0) as gz:
        for chunk in _chunks(entry, seed):
            gz.write(chunk)
    return buffer.getvalue()


def write_gzip_entry(output_dir, entry, seed):
    """Write <name>.gz for a compressible entry; return (size, sha256)."""
    payload = _gzip_bytes(entry, seed)
    path = os.path.join(output_dir, entry["name"] + ".gz")
    with open(path, "wb") as handle:
        handle.write(payload)
    return len(payload), hashlib.sha256(payload).hexdigest()


def _build_manifest(entries, seed, include_huge, with_gzip):
    classes = {}
    traffic_mix = {}
    for entry in entries:
        class_name = entry["class"]
        record = classes.setdefault(class_name, {
            "count": 0,
            "total_bytes": 0,
            "default_weight": CLASS_META[class_name]["weight"],
            "exercises": entry["exercises"],
        })
        record["count"] += 1
        record["total_bytes"] += entry["bytes"]
        traffic_mix[class_name] = CLASS_META[class_name]["weight"]
    return {
        "schema_version": SCHEMA_VERSION,
        "generator": GENERATOR_NAME,
        "seed": seed,
        "include_huge": include_huge,
        "with_gzip": with_gzip,
        "cache_max_file_bytes": CACHE_MAX_FILE_BYTES,
        "entries": entries,
        "classes": classes,
        "traffic_mix": traffic_mix,
        "ranges": [dict(r) for r in _RANGES],
        "total_bytes": sum(entry["bytes"] for entry in entries),
    }


def generate(output_dir, manifest_path=None, seed=DEFAULT_SEED,
             include_huge=False, with_gzip=False):
    """Write the corpus and its manifest; return the manifest dict."""
    os.makedirs(output_dir, exist_ok=True)
    entries = plan(include_huge=include_huge)
    for entry in entries:
        entry["sha256"] = write_entry(output_dir, entry, seed)
        if with_gzip and entry["compressible"]:
            size, digest = write_gzip_entry(output_dir, entry, seed)
            entry["gzip_name"] = entry["name"] + ".gz"
            entry["gzip_bytes"] = size
            entry["gzip_sha256"] = digest
    manifest = _build_manifest(entries, seed, include_huge, with_gzip)

    if manifest_path is None:
        manifest_path = os.path.join(output_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")
    return manifest


def load_manifest(manifest_path):
    with open(manifest_path, encoding="utf-8") as handle:
        return json.load(handle)


def _sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(_CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify(output_dir, manifest_path=None):
    """Return a list of human-readable mismatches (empty means consistent)."""
    if manifest_path is None:
        manifest_path = os.path.join(output_dir, "manifest.json")
    manifest = load_manifest(manifest_path)

    problems = []
    for entry in manifest.get("entries", []):
        path = os.path.join(output_dir, entry["name"])
        if not os.path.isfile(path):
            problems.append("missing: %s" % entry["name"])
            continue
        size = os.path.getsize(path)
        if size != entry["bytes"]:
            problems.append("size mismatch: %s (got %d, want %d)" %
                            (entry["name"], size, entry["bytes"]))
            continue
        if _sha256_file(path) != entry["sha256"]:
            problems.append("sha256 mismatch: %s" % entry["name"])
            continue
        gzip_name = entry.get("gzip_name")
        if not gzip_name:
            continue
        gz_path = os.path.join(output_dir, gzip_name)
        if not os.path.isfile(gz_path):
            problems.append("missing: %s" % gzip_name)
            continue
        gz_size = os.path.getsize(gz_path)
        if gz_size != entry["gzip_bytes"]:
            problems.append("size mismatch: %s (got %d, want %d)" %
                            (gzip_name, gz_size, entry["gzip_bytes"]))
        elif _sha256_file(gz_path) != entry["gzip_sha256"]:
            problems.append("sha256 mismatch: %s" % gzip_name)
    return problems


def describe(include_huge=False):
    """Plan plus class/traffic summary, without hashing any content."""
    entries = plan(include_huge=include_huge)
    return _build_manifest(entries, DEFAULT_SEED, include_huge, False)


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir",
                        default=os.path.join(REPO_ROOT, "benchmarks", "corpus"),
                        help="directory to write the corpus into "
                             "(default: benchmarks/corpus)")
    parser.add_argument("--manifest", default=None,
                        help="manifest path (default: <output-dir>/manifest.json)")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED,
                        help="content seed (default: %d)" % DEFAULT_SEED)
    parser.add_argument("--include-huge", action="store_true",
                        help="also generate the 256 MiB opt-in class")
    parser.add_argument("--with-gzip", action="store_true",
                        help="also write <name>.gz for compressible classes "
                             "(for gzip_static peers such as nginx)")
    parser.add_argument("--list", action="store_true",
                        help="print the plan and class summary, write nothing")
    parser.add_argument("--verify", action="store_true",
                        help="verify an existing corpus against its manifest")
    parser.add_argument("--quiet", action="store_true",
                        help="suppress the human-readable summary")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)

    if args.list:
        print(json.dumps(describe(include_huge=args.include_huge),
                         indent=2, sort_keys=True))
        return 0

    if args.verify:
        try:
            problems = verify(args.output_dir, args.manifest)
        except (OSError, ValueError) as exc:
            print("FAIL: cannot verify corpus: %s" % exc, file=sys.stderr)
            return 2
        for problem in problems:
            print("FAIL: %s" % problem, file=sys.stderr)
        if problems:
            return 1
        if not args.quiet:
            print("corpus at %s matches its manifest" % args.output_dir)
        return 0

    try:
        manifest = generate(args.output_dir, manifest_path=args.manifest,
                            seed=args.seed, include_huge=args.include_huge,
                            with_gzip=args.with_gzip)
    except OSError as exc:
        print("FAIL: cannot write corpus: %s" % exc, file=sys.stderr)
        return 2
    if not args.quiet:
        manifest_path = args.manifest or os.path.join(
            args.output_dir, "manifest.json")
        print("wrote %d files (%d bytes) to %s" %
              (len(manifest["entries"]), manifest["total_bytes"],
               args.output_dir))
        print("manifest: %s" % manifest_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
