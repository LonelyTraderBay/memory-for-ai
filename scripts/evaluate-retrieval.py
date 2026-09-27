#!/usr/bin/env python3
"""Fixed source-grounded retrieval smoke; byte/4 estimates are NOT model usage."""
import argparse
import ast
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

spec = importlib.util.spec_from_file_location("response", Path(__file__).with_name("benchmark-response.py"))
response = importlib.util.module_from_spec(spec)
spec.loader.exec_module(response)

SOURCE = """def alpha(value):
    return helper_double(value)

def helper_double(value):
    return value * 2

def helper_zero():
    return 0
"""
PATTERNS = ["^alpha$", "^helper_.*", "^not_present$"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    binary = str(args.binary.resolve(strict=True))
    artifacts = []
    with tempfile.TemporaryDirectory(prefix="mfa-retrieval-") as tmp:
        root = Path(tmp)
        env = dict(os.environ)
        for key in ("HOME", "USERPROFILE", "APPDATA", "LOCALAPPDATA", "XDG_CONFIG_HOME",
                    "XDG_CACHE_HOME", "XDG_DATA_HOME", "MFA_CACHE_DIR"):
            directory = root / key.lower()
            directory.mkdir()
            env[key] = str(directory)
        repo = root / "fixture"
        repo.mkdir()
        (repo / "sample.py").write_text(SOURCE, encoding="utf-8")

        def run(argv, stdin=None):
            result = subprocess.run([binary, *argv], input=stdin, capture_output=True,
                                    encoding="utf-8", env=env, cwd=root, timeout=120)
            if result.returncode:
                raise RuntimeError(f"command failed ({result.returncode}): {result.stderr}")
            return result.stdout

        def tool(name, values):
            request = json.dumps(values)
            started = time.perf_counter_ns()
            raw = run(["cli", name, request])
            elapsed = (time.perf_counter_ns() - started) / 1_000_000
            payload = response.response_payload(json.loads(raw))
            artifacts.append({"tool": name, "arguments": values, "response": json.loads(raw)})
            return payload, len((name + request + raw).encode("utf-8")), elapsed

        try:
            indexed, _, _ = tool("index_repository", {"repo_path": str(repo), "mode": "full"})
            response.validate("index", indexed)
            project = indexed["project"]
            status, _, _ = tool("index_status", {"project": project})
            messages = [
                {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                    "protocolVersion": "2025-11-25", "capabilities": {},
                    "clientInfo": {"name": "retrieval-evaluation", "version": "1"}}},
                {"jsonrpc": "2.0", "method": "notifications/initialized"},
                {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}},
            ]
            raw_manifest = run([], "".join(json.dumps(message) + "\n" for message in messages))
            manifest = next(json.loads(line) for line in raw_manifest.splitlines()
                            if json.loads(line).get("id") == 2)
            tools = manifest["result"]["tools"]
            if not tools:
                raise ValueError("empty tools/list manifest")
            manifest_bytes = len(json.dumps(manifest, ensure_ascii=False).encode("utf-8"))
            definitions = [node.name for node in ast.parse(SOURCE).body if isinstance(node, ast.FunctionDef)]
            tasks = []
            for pattern in PATTERNS:
                expected = sorted(name for name in definitions if re.search(pattern, name))
                result, byte_count, elapsed = tool("search_graph", {
                    "project": project, "name_pattern": pattern, "label": "Function",
                    "limit": 20, "format": "json"})
                response.validate("search", result)
                actual = sorted(row[0] for group in result["groups"] for row in group["rows"])
                passed = actual == expected and result["total"] == len(expected) and not result["has_more"]
                tasks.append({"pattern": pattern, "expected": expected, "actual": actual,
                              "pass": passed, "wall_ms": round(elapsed, 3), "request_response_bytes": byte_count})
            query_bytes = sum(task["request_response_bytes"] for task in tasks)
            report = {
                "scope": "3 Python definition-retrieval tasks; no model answer or broad language-quality claim",
                "source": SOURCE, "source_sha256": hashlib.sha256(SOURCE.encode()).hexdigest(),
                "manifest": manifest, "manifest_bytes": manifest_bytes, "index_status": status,
                "tasks": tasks, "passed": sum(task["pass"] for task in tasks),
                "token_estimate_method": "UTF-8 bytes / 4, manifest once per session; not provider token usage",
                "retrieval_bytes": query_bytes,
                "session_retrieval_estimated_tokens": (manifest_bytes + query_bytes) / 4,
                "setup_excluded_from_retrieval": ["index_repository", "index_status", "MCP initialize"],
                "artifacts": artifacts,
            }
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
            print(f"{report['passed']}/{len(tasks)} PASS; manifest={manifest_bytes} bytes; report={args.output}")
            return 0 if all(task["pass"] for task in tasks) else 1
        finally:
            run(["daemon", "stop"])


if __name__ == "__main__":
    raise SystemExit(main())
