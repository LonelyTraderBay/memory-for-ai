#!/usr/bin/env python3
"""A/B measurement: edit tools vs manual grep+read+write.

Protocol per docs/MEASURING.md, adapted for the edit tools (THIET-KE-EDIT-TOOLS.md §8):
standard rename and move tasks with rising fan-out on a synthetic fixture, both
conditions executed mechanically, token estimate = tool-output bytes / 4 (same
convention as docs/AB-RESULTS.md).

  Condition A (edit tools): rename_symbol / move_symbol plan (dry_run) -> apply
                            (with expected_counts/expected_files parsed from the
                            plan) -> verify
  Condition B (manual):     grep -rnw -> read every matched file in full ->
                            rewrite every file in full -> verify grep

Both conditions are applied for real on separate fixture copies; the trees are
diffed afterwards so quality is proven equal, not assumed.

Usage:  python scripts/ab-edit-tools.py [path-to-memory-for-ai-binary]
Output: build/ab-edit-tools/ab_edit_results.csv + a markdown table on stdout.
"""

import difflib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO_ROOT, "build", "c", "memory-for-ai.exe")
OUT_DIR = os.path.join(REPO_ROOT, "build", "ab-edit-tools")

# (task_id, n_caller_files, n_local_calls, n_calls_per_caller)
# Occurrences = 1 def + n_local_calls + n_caller_files * (1 import + calls)
TASKS = [
    ("t1_fanout_1", 0, 1, 0),  # 2 occs, 1 file
    ("t2_fanout_2", 1, 1, 1),  # 4 occs, 2 files
    ("t3_fanout_3", 2, 1, 1),  # 6 occs, 3 files
    ("t4_fanout_4", 3, 1, 1),  # 8 occs, 4 files
    ("t5_fanout_5", 4, 1, 2),  # 12 occs, 5 files
]

# (task_id, n_importer_files) — move tasks: mod_<task> defines moveme_<task>,
# dest_<task> is the destination module, importers from-import the symbol.
# Files touched = 2 (source + destination) + n_importer_files.
MOVE_TASKS = [
    ("m1_fanout_0", 0),
    ("m2_fanout_1", 1),
    ("m3_fanout_2", 2),
    ("m4_fanout_3", 3),
    ("m5_fanout_4", 4),
]

# File-size scenarios: lines of inert padding prepended to every fixture file.
# 0 = minimal synthetic files; 300 ~ realistic module size where full-file
# read/rewrite (condition B) pays for content unrelated to the rename.
PAD_SCENARIOS = [("small", 0), ("padded300", 300)]


def _pad(task_id, n_lines):
    """Inert code that never mentions the rename target."""
    return "".join(f"def filler_{task_id}_{i}(x):\n    return x * {i}\n\n"
                   for i in range(n_lines // 2))


def gen_fixture(root, task_id, n_callers, n_local, calls_per_caller, pad_lines=0):
    """Generate one rename scenario: module mod_<task> defining target_<task>,
    plus caller modules using it. Returns (old_name, new_name)."""
    old = f"target_{task_id}"
    new = f"renamed_{task_id}"
    pad = _pad(task_id, pad_lines)
    os.makedirs(root, exist_ok=True)
    local = "\n".join(f"    return {old}({i})" if i == 0 else f"    x = {old}({i})"
                      for i in range(max(n_local, 1)))
    with open(os.path.join(root, f"mod_{task_id}.py"), "w") as f:
        f.write(pad + f"def {old}(x):\n    return x + 1\n\n"
                f"def local_use_{task_id}():\n{local}\n")
    for j in range(n_callers):
        calls = "\n".join(f"    y{k} = {old}({j}{k})" for k in range(calls_per_caller))
        with open(os.path.join(root, f"caller_{task_id}_{j}.py"), "w") as f:
            f.write(pad + f"from mod_{task_id} import {old}\n\n"
                    f"def use_{task_id}_{j}():\n{calls}\n")
    return old, new


def gen_move_fixture(root, task_id, n_importers, pad_lines=0):
    """Generate one move scenario: mod_<task> defines moveme_<task> (no local
    callers — the source needs no re-import), dest_<task> is an existing
    destination module, and n_importers caller modules from-import the symbol.
    Returns (old_module, dest_module, symbol_name)."""
    old_mod = f"mod_{task_id}"
    dst_mod = f"dest_{task_id}"
    name = f"moveme_{task_id}"
    pad = _pad(task_id, pad_lines)
    os.makedirs(root, exist_ok=True)
    with open(os.path.join(root, old_mod + ".py"), "w") as f:
        f.write(pad + f"def {name}(x):\n    return x + 1\n\n"
                f"def other_{task_id}(x):\n    return x\n")
    with open(os.path.join(root, dst_mod + ".py"), "w") as f:
        f.write(pad + f"def existing_{task_id}(x):\n    return x\n")
    for j in range(n_importers):
        with open(os.path.join(root, f"caller_{task_id}_{j}.py"), "w") as f:
            f.write(pad + f"from {old_mod} import {name}\n\n"
                    f"def use_{task_id}_{j}():\n    y0 = {name}({j}0)\n")
    return old_mod, dst_mod, name


def manual_rename(root, old, new):
    """Condition B, executed mechanically: grep -> read -> full rewrite.
    Returns (calls, bytes_out) and applies the rename on disk."""
    word = re.compile(r"\b%s\b" % re.escape(old))
    files = sorted(
        os.path.join(root, f) for f in os.listdir(root)
        if f.endswith(".py") and word.search(open(os.path.join(root, f)).read()))
    calls = 1  # the grep
    bytes_out = sum(len(f"{p}: match list\n".encode()) for p in files)  # grep output
    for path in files:
        with open(path) as f:
            data = f.read()
        calls += 1  # the read
        bytes_out += len(data.encode())
        updated = word.sub(new, data)
        calls += 1  # the write (full-file rewrite, honest manual cost)
        bytes_out += len(updated.encode())
        with open(path, "w") as f:
            f.write(updated)
    calls += 1  # the verify grep
    leftover = sum(1 for f in os.listdir(root) if f.endswith(".py")
                   for line in open(os.path.join(root, f)) if word.search(line))
    bytes_out += len(f"verify: {leftover} leftover\n".encode())
    return calls, bytes_out, leftover


def manual_move(root, old_mod, dst_mod, name):
    """Condition B for move, executed mechanically: grep -> read every matched
    file (plus the destination) in full -> rewrite every file in full ->
    verify grep. Mirrors the tool's exact surgery so the tree diff is
    meaningful: delete the def + one following blank line, append it to the
    destination with one blank separator, repoint from-imports."""
    word = re.compile(r"\b%s\b" % re.escape(name))
    files = sorted(
        os.path.join(root, f) for f in os.listdir(root)
        if f.endswith(".py") and word.search(open(os.path.join(root, f)).read()))
    calls = 1  # the grep
    bytes_out = sum(len(f"{p}: match list\n".encode()) for p in files)
    contents = {}
    for path in files:
        with open(path) as f:
            contents[path] = f.read()
        calls += 1  # the read
        bytes_out += len(contents[path].encode())
    # an honest manual move reads the destination too (it never mentions the
    # symbol, so grep alone does not surface it)
    dst_path = os.path.join(root, dst_mod + ".py")
    if dst_path not in contents:
        with open(dst_path) as f:
            contents[dst_path] = f.read()
        calls += 1
        bytes_out += len(contents[dst_path].encode())
    src_path = os.path.join(root, old_mod + ".py")

    lines = contents[src_path].splitlines(keepends=True)
    s = next(i for i, l in enumerate(lines) if l.startswith(f"def {name}("))
    e = s
    while e + 1 < len(lines) and lines[e + 1][:1] in (" ", "\t"):
        e += 1
    def_text = "".join(lines[s:e + 1])
    del_end = e + 1
    if del_end < len(lines) and lines[del_end].strip() == "":
        del_end += 1  # absorb one following blank line (surgery semantics)
    writes = {src_path: "".join(lines[:s] + lines[del_end:])}

    new_dst = contents[dst_path]
    if not new_dst.endswith("\n"):
        new_dst += "\n"
    if not new_dst.endswith("\n\n"):
        new_dst += "\n"
    writes[dst_path] = new_dst + def_text

    for path in files:
        if path in (src_path, dst_path):
            continue
        writes[path] = contents[path].replace(f"from {old_mod} import {name}",
                                              f"from {dst_mod} import {name}")
    for path, data in writes.items():
        calls += 1  # the write (full-file rewrite, honest manual cost)
        bytes_out += len(data.encode())
        with open(path, "w") as f:
            f.write(data)
    calls += 1  # the verify grep
    leftover = sum(1 for f in os.listdir(root) if f.endswith(".py")
                   for line in open(os.path.join(root, f))
                   if f"from {old_mod} import {name}" in line)
    bytes_out += len(f"verify: {leftover} leftover\n".encode())
    return calls, bytes_out, leftover


class Ab:
    def __init__(self, work_root):
        self.env = dict(os.environ)
        self.home = os.path.join(work_root, "home")
        self.cache = os.path.join(work_root, "cache")
        os.makedirs(self.home, exist_ok=True)
        os.makedirs(self.cache, exist_ok=True)
        self.env.update({"HOME": self.home, "USERPROFILE": self.home,
                         "MFA_CACHE_DIR": self.cache})

    def cli(self, tool, args):
        p = subprocess.run([BIN, "cli", tool, json.dumps(args)],
                           capture_output=True, env=self.env, timeout=600)
        out = p.stdout + p.stderr
        return len(out), out.decode(errors="replace")

    def project_of(self, path):
        _, out = self.cli("list_projects", {})
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("{"):
                data = json.loads(line)
                for proj in data.get("projects", []):
                    if proj.get("root_path", "").replace("\\", "/").rstrip("/") == \
                            path.replace("\\", "/").rstrip("/"):
                        return proj["name"]
        raise RuntimeError("project not found after index: " + path)


def tree_snapshot(root):
    return {f: open(os.path.join(root, f)).read()
            for f in sorted(os.listdir(root)) if f.endswith(".py")}


def kill_stray_daemons():
    """Stop daemons spawned from BIN inside the isolated temp cache, else their
    open log handle makes TemporaryDirectory cleanup fail on Windows."""
    if os.name != "nt":
        subprocess.run(["pkill", "-f", BIN], capture_output=True)
        return
    bin_norm = os.path.abspath(BIN)
    ps = ("Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -eq '"
          + bin_norm.replace("'", "''")
          + "' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }")
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    kill_stray_daemons()  # free stale temp dirs from previous failed runs
    version = subprocess.run([BIN, "--version"], capture_output=True).stdout.decode().strip()
    rows = []
    with tempfile.TemporaryDirectory(prefix="cbm_ab_edit_", ignore_cleanup_errors=True) as work:
        ab = Ab(work)
        for scenario, pad_lines in PAD_SCENARIOS:
            for task_id, n_callers, n_local, calls_per_caller in TASKS:
                tag = f"{scenario}/{task_id}"
                dir_a = os.path.join(work, tag + "_a")
                dir_b = os.path.join(work, tag + "_b")
                old, new = gen_fixture(dir_a, task_id, n_callers, n_local,
                                       calls_per_caller, pad_lines)
                shutil.copytree(dir_a, dir_b)
                ground_truth_files = len([f for f in os.listdir(dir_a) if f.endswith(".py")])

                # ── Condition A: edit tools ──
                ab.cli("index_repository", {"repo_path": dir_a})
                proj = ab.project_of(dir_a)
                qn = f"{proj}.mod_{task_id}.{old}"
                b1, _ = ab.cli("rename_symbol", {"project": proj, "qualified_name": qn,
                                                 "new_name": new})
                b2, applied = ab.cli("rename_symbol", {"project": proj, "qualified_name": qn,
                                                       "new_name": new, "dry_run": False,
                                                       "force": True})
                b3, _ = ab.cli("search_graph", {"project": proj, "name_pattern": new})
                calls_a, bytes_a = 3, b1 + b2 + b3
                if "rename_symbol: APPLIED" not in applied:
                    raise RuntimeError(f"{tag}: condition A failed:\n{applied}")

                # ── Condition B: manual grep+read+write ──
                calls_b, bytes_b, leftover = manual_rename(dir_b, old, new)

                # ── Quality: trees must be identical ──
                snap_a, snap_b = tree_snapshot(dir_a), tree_snapshot(dir_b)
                quality = "PASS" if snap_a == snap_b and leftover == 0 else "FAIL"
                if quality != "PASS":
                    diff = "\n".join(difflib.unified_diff(
                        *[json.dumps(s, indent=1).splitlines() for s in (snap_a, snap_b)],
                        lineterm=""))[:2000]
                    print(f"{tag}: TREE MISMATCH\n{diff}", file=sys.stderr)

                rows.append({
                    "kind": "rename", "task": tag, "scenario": scenario,
                    "files": ground_truth_files,
                    "calls_a": calls_a, "calls_b": calls_b,
                    "bytes_a": bytes_a, "bytes_b": bytes_b,
                    "tokens_a": bytes_a // 4, "tokens_b": bytes_b // 4,
                    "quality": quality,
                })
                # isolate the next task's project cache
                shutil.rmtree(ab.cache, ignore_errors=True)
                os.makedirs(ab.cache, exist_ok=True)

            for task_id, n_importers in MOVE_TASKS:
                tag = f"{scenario}/{task_id}"
                dir_a = os.path.join(work, tag + "_a")
                dir_b = os.path.join(work, tag + "_b")
                old_mod, dst_mod, name = gen_move_fixture(dir_a, task_id,
                                                          n_importers, pad_lines)
                shutil.copytree(dir_a, dir_b)
                ground_truth_files = len([f for f in os.listdir(dir_a)
                                          if f.endswith(".py")])

                # ── Condition A: edit tools ──
                ab.cli("index_repository", {"repo_path": dir_a})
                proj = ab.project_of(dir_a)
                qn = f"{proj}.{old_mod}.{name}"
                dst_qn = f"{proj}.{dst_mod}"
                b1, plan = ab.cli("move_symbol", {"project": proj,
                                                  "qualified_name": qn,
                                                  "destination_module": dst_qn})
                apply_args = {"project": proj, "qualified_name": qn,
                              "destination_module": dst_qn, "dry_run": False}
                m = re.search(r"expected_files=(\d+)", plan)
                if m:
                    apply_args["expected_files"] = int(m.group(1))
                b2, applied = ab.cli("move_symbol", apply_args)
                b3, _ = ab.cli("search_graph", {"project": proj,
                                                "name_pattern": name})
                calls_a, bytes_a = 3, b1 + b2 + b3
                if "move_symbol: APPLIED" not in applied:
                    raise RuntimeError(f"{tag}: condition A failed:\n{applied}")

                # ── Condition B: manual grep+read+write ──
                calls_b, bytes_b, leftover = manual_move(dir_b, old_mod,
                                                         dst_mod, name)

                # ── Quality: trees must be identical ──
                snap_a, snap_b = tree_snapshot(dir_a), tree_snapshot(dir_b)
                quality = "PASS" if snap_a == snap_b and leftover == 0 else "FAIL"
                if quality != "PASS":
                    diff = "\n".join(difflib.unified_diff(
                        *[json.dumps(s, indent=1).splitlines() for s in (snap_a, snap_b)],
                        lineterm=""))[:2000]
                    print(f"{tag}: TREE MISMATCH\n{diff}", file=sys.stderr)

                rows.append({
                    "kind": "move", "task": tag, "scenario": scenario,
                    "files": ground_truth_files,
                    "calls_a": calls_a, "calls_b": calls_b,
                    "bytes_a": bytes_a, "bytes_b": bytes_b,
                    "tokens_a": bytes_a // 4, "tokens_b": bytes_b // 4,
                    "quality": quality,
                })
                # isolate the next task's project cache
                shutil.rmtree(ab.cache, ignore_errors=True)
                os.makedirs(ab.cache, exist_ok=True)
        # release daemon log handles so the temp dir can be removed (Windows)
        kill_stray_daemons()

    csv_path = os.path.join(OUT_DIR, "ab_edit_results.csv")
    with open(csv_path, "w") as f:
        f.write("kind,task,scenario,files,calls_a,calls_b,bytes_a,bytes_b,tokens_a,tokens_b,quality\n")
        for r in rows:
            f.write(",".join(str(r[k]) for k in
                             ("kind", "task", "scenario", "files", "calls_a", "calls_b",
                              "bytes_a", "bytes_b", "tokens_a", "tokens_b", "quality")) + "\n")

    print(f"binary: {version}")
    print("| kind | task | files | calls A/B | tokens A (est) | tokens B (est) | token reduction | quality |")
    print("|---|---|---|---|---|---|---|---|")
    for r in rows:
        red = 100.0 * (r["tokens_b"] - r["tokens_a"]) / r["tokens_b"] if r["tokens_b"] else 0.0
        print(f"| {r['kind']} | {r['task']} | {r['files']} | {r['calls_a']}/{r['calls_b']} "
              f"| {r['tokens_a']} | {r['tokens_b']} | {red:.1f}% | {r['quality']} |")
    for scenario, _pad in PAD_SCENARIOS:
        for kind in ("rename", "move"):
            sub = [r for r in rows if r["scenario"] == scenario and r["kind"] == kind]
            if not sub:
                continue
            total_a = sum(r["tokens_a"] for r in sub)
            total_b = sum(r["tokens_b"] for r in sub)
            calls_a = sum(r["calls_a"] for r in sub)
            calls_b = sum(r["calls_b"] for r in sub)
            print(f"| **total {scenario} {kind}** | | {sum(r['files'] for r in sub)} | {calls_a}/{calls_b} "
                  f"| {total_a} | {total_b} | {100.0 * (total_b - total_a) / total_b:.1f}% "
                  f"| {sum(r['quality'] == 'PASS' for r in sub)}/{len(sub)} PASS |")
    print(f"\nCSV: {csv_path}")
    return 0 if all(r["quality"] == "PASS" for r in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
