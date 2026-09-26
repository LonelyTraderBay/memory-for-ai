"""Supported-target boundaries must reject a target before publishing bytes."""
import ast
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


class WindowsX64Contract(unittest.TestCase):
    def test_release_boundaries_agree_on_exact_target(self):
        for script in ("stage-release-candidates", "select-release-candidates", "verify-release-selection"):
            tree = ast.parse((ROOT / "scripts/ci" / (script + ".py")).read_text(encoding="utf-8"))
            targets = [ast.literal_eval(node.value) for node in tree.body
                       if isinstance(node, ast.Assign)
                       and any(isinstance(target, ast.Name) and target.id == "TARGETS"
                               for target in node.targets)]
            self.assertEqual(targets, [("windows-amd64",)], script)

    def test_npm_platform_metadata_is_exact(self):
        package = json.loads((ROOT / "pkg/npm/package.json").read_text(encoding="utf-8"))
        self.assertEqual(package["os"], ["win32"])
        self.assertEqual(package["cpu"], ["x64"])

    def test_memory_gate_does_not_drop_windows_drive_diagnostics(self):
        for suffix in ("", ",-warnings-as-errors"):
            diagnostic = f"C:/missing/fixture.c:7:2: warning: Potential leak [clang-analyzer-unix.Malloc{suffix}]\n"
            result = subprocess.run([sys.executable, str(ROOT / "scripts/lint-mem-gate.py")],
                                    input=diagnostic, text=True, capture_output=True, cwd=ROOT)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("fixture.c", result.stdout + result.stderr)

    def test_memory_gate_excludes_vendored_analyzer_diagnostics(self):
        vendored = (ROOT / "internal/cbm/vendored/verstable/verstable.h").as_posix()
        diagnostic = f"{vendored}:1191:24: warning: uninitialized key [clang-analyzer-core.CallAndMessage]\n"
        result = subprocess.run([sys.executable, str(ROOT / "scripts/lint-mem-gate.py")],
                                input=diagnostic, text=True, capture_output=True, cwd=ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_packaging_refuses_unsupported_target_before_output(self):
        for target in (("windows", "arm64"), ("linux", "amd64"), ("darwin", "arm64")):
            result = subprocess.run(
                ["bash", "scripts/package-release.sh", *target,
                 "--selected-binary", "missing-fixture", "--expected-sha256", "0" * 64,
                 "--third-party-notices", "missing-notices", "--out-dir", "build/must-not-publish-x64-test"],
                cwd=ROOT, capture_output=True, text=True)
            details = (f"target={target}, exit={result.returncode}, "
                       f"stdout={result.stdout!r}, stderr={result.stderr!r}")
            self.assertNotEqual(result.returncode, 0, details)
            self.assertIn("unsupported release target", result.stderr, details)
        self.assertFalse((ROOT / "build/must-not-publish-x64-test").exists())

    def test_pypi_rejects_other_platforms_and_architectures(self):
        spec = importlib.util.spec_from_file_location("x64_cli", ROOT / "pkg/pypi/src/memory_for_ai/_cli.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        for host in ("linux", "darwin"):
            with patch.object(sys, "platform", host), self.assertRaises(SystemExit):
                module._os_name()
        for machine in ("arm64", "aarch64", "i686"):
            with patch.object(module.platform, "machine", return_value=machine), self.assertRaises(SystemExit):
                module._arch()
        with patch.object(sys, "platform", "win32"), patch.object(module.platform, "machine", return_value="AMD64"):
            self.assertEqual((module._os_name(), module._arch()), ("windows", "amd64"))

    @unittest.skipUnless(sys.platform == "win32", "Windows installer contract")
    def test_installer_refuses_arm64_before_network(self):
        env = dict(os.environ, CBM_ARCH="arm64")
        shell_candidates = [
            shutil.which("pwsh"),
            os.environ.get("CBM_PWSH"),
            os.path.join(os.environ.get("USERPROFILE", ""), ".cache", "codex-runtimes",
                         "codex-primary-runtime", "dependencies", "native", "powershell", "pwsh.exe"),
        ]
        shell = next((candidate for candidate in shell_candidates
                      if candidate and os.path.isfile(candidate)), None)
        if shell is None:
            self.skipTest("PowerShell 7 is required for the installer contract")
        result = subprocess.run([shell, "-NoProfile", "-File", str(ROOT / "install.ps1")],
                                env=env, capture_output=True, timeout=20)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"Windows x64 only", result.stderr)


if __name__ == "__main__":
    unittest.main()
