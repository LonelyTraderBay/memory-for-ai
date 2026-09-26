"""Failure-injection contracts for the actual benchmark entry points."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("benchmark_response", ROOT / "scripts/benchmark-response.py")
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)


class BenchmarkContract(unittest.TestCase):
    def test_response_rejects_errors_at_every_envelope(self):
        for bad in ({"error": "failed"}, {"result": {"isError": True}},
                    {"result": {"content": [{"type": "text", "text": "not json"}]}},
                    {"nodes": 1, "edges": -1, "project": "p"},
                    {"nodes": True, "edges": 0, "project": "p"}, {}):
            with self.subTest(bad=bad), self.assertRaises((ValueError, KeyError)):
                validator.validate("index", bad)

    def test_empty_success_is_valid(self):
        self.assertEqual(validator.validate("search", {"total": 0})["total"], 0)
        self.assertEqual(validator.validate("index", {"nodes": 0, "edges": 0, "project": "p"})["nodes"], 0)

    def test_scripts_propagate_exit_and_protocol_failures(self):
        bash = os.environ.get("CBM_TEST_BASH") or shutil.which("bash")
        self.assertIsNotNone(bash, "Bash is required for benchmark script contracts")
        with tempfile.TemporaryDirectory(prefix="cbm-benchmark-contract-") as tmp:
            work = Path(tmp)
            repo = work / "fixture"
            repo.mkdir()
            (repo / "sample.c").write_text("int main(void) { return 0; }\n")
            fake = work / "fake-binary"
            fake.write_text('#!/usr/bin/env bash\nprintf "%s\\n" "$BENCHMARK_RESPONSE"\nexit "$BENCHMARK_EXIT"\n', newline="\n")
            fake.chmod(0o700)
            for kind in ("index", "search-graph"):
                for number, (exit_code, response, success) in enumerate([
                    (23, {}, False), (0, {"error": {"message": "failed"}}, False),
                    (0, {"result": {"isError": True, "content": []}}, False),
                    (0, "malformed", False),
                    (0, {"nodes": 0, "edges": 0, "project": "p", "total": 0}, True),
                ]):
                    with self.subTest(kind=kind, exit_code=exit_code, response=response):
                        result_dir = work / f"results-{kind}-{number}"
                        args = [str(bash), str(ROOT / f"scripts/benchmark-{kind}.sh"), fake.as_posix()]
                        args += ["probe", repo.as_posix(), result_dir.as_posix()] if kind == "index" else ["probe"]
                        env = dict(os.environ, BENCHMARK_EXIT=str(exit_code),
                                   BENCHMARK_RESPONSE=json.dumps(response))
                        run = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
                        self.assertEqual(run.returncode == 0, success, run.stdout + run.stderr)
                        if kind == "index":
                            self.assertEqual((result_dir / "probe/index-time.txt").exists(), success)
                            if success:
                                env["BENCHMARK_RESPONSE"] = '{"error":"failed rerun"}'
                                rerun = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
                                self.assertNotEqual(rerun.returncode, 0)
                                self.assertFalse((result_dir / "probe/index-time.txt").exists())


if __name__ == "__main__":
    unittest.main()
