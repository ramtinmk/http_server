#!/usr/bin/env python3
"""Run short clean libFuzzer corpus passes and record their evidence."""
import argparse
import json
import os
import shutil
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=os.getcwd())
    parser.add_argument("--runs", type=int, default=1000)
    parser.add_argument("--output",
                        default="benchmarks/production_phase4_fuzz.json")
    args = parser.parse_args()
    root = os.path.abspath(args.repo_root)
    output = args.output if os.path.isabs(args.output) else os.path.join(root, args.output)
    cases = [
        ("http_parser", "bin/http_parser_fuzzer", "fuzz/corpus/http_parser"),
        ("connection_state", "bin/connection_state_fuzzer",
         "fuzz/corpus/connection_state"),
    ]
    report = {"runs_per_target": args.runs, "targets": {}, "failures": []}
    with tempfile.TemporaryDirectory(prefix="http-server-fuzz-") as temp_root:
        for name, binary, corpus in cases:
            path = os.path.join(root, binary)
            corpus_path = os.path.join(root, corpus)
            run_corpus = os.path.join(temp_root, name)
            shutil.copytree(corpus_path, run_corpus)
            started = time.time()
            result = subprocess.run(
                [path, run_corpus, "-runs=%d" % args.runs, "-close_fd_mask=3"],
                cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, timeout=120)
            report["targets"][name] = {
                "binary": binary,
                "corpus": corpus,
                "returncode": result.returncode,
                "seconds": round(time.time() - started, 3),
                "output_tail": result.stdout[-2000:],
            }
            if result.returncode != 0:
                report["failures"].append("%s exited %d" %
                                         (name, result.returncode))
    report["passed"] = not report["failures"]
    os.makedirs(os.path.dirname(output), exist_ok=True)
    with open(output, "w") as artifact:
        json.dump(report, artifact, indent=2, sort_keys=True)
        artifact.write("\n")
    if report["failures"]:
        print("FAIL: Phase 4 fuzz smoke; artifact: %s" % output)
        return 1
    print("PASS: Phase 4 fuzz smoke; artifact: %s" % output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
