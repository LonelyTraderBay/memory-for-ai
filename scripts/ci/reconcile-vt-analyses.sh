#!/usr/bin/env bash
# Make the verify-stage VirusTotal surface scan rerun-safe.
#
# The upload action (crazy-max/ghaction-virustotal) POSTs every object to
# VirusTotal; stable release members (installers, licenses, manifests) keep
# byte-identical content across releases, so VirusTotal answers 409 Conflict
# on every attempt after the first and the action aborts the step (upstream
# issue #220, unfixed at v5.0.0). Those objects already HAVE reports, so the
# correct recovery is not a re-upload but VirusTotal's documented reanalysis
# endpoint: POST /api/v3/files/{sha256}/analyse returns a fresh analysis
# object for the known bytes. This script merges whatever analysis URLs the
# action did produce with reanalysis URLs for everything it could not, and
# emits the same "name=analysisURL,..." format check-virustotal.sh consumes.
# The downstream gate stays fully authoritative: it still polls every
# analysis to completion and applies the verdict policy itself.
set -euo pipefail

: "${VT_API_KEY:?reconcile-vt-analyses: VT_API_KEY is required}"
: "${VT_EXPECTED_SCAN_SET:?reconcile-vt-analyses: VT_EXPECTED_SCAN_SET is required}"
VT_ACTION_ANALYSIS="${VT_ACTION_ANALYSIS:-}"
VT_WITHHELD="${VT_WITHHELD:-}"

# shellcheck disable=SC2016
python3 - "$VT_API_KEY" "$VT_EXPECTED_SCAN_SET" "$VT_WITHHELD" "$VT_ACTION_ANALYSIS" <<'PY'
from __future__ import annotations

import json
import os
import pathlib
import re
import subprocess
import sys
import time
from typing import Dict, List, Optional, Tuple

SHA256_RE = re.compile(r"[0-9a-f]{64}\Z")
SCAN_FIELDS = ("scan_path", "sha256", "size", "association_count", "association_kinds")
ANALYSIS_URL = "https://www.virustotal.com/gui/file-analysis/{identifier}/detection"
ANALYSE_ENDPOINT = "https://www.virustotal.com/api/v3/files/{sha256}/analyse"


def fail(message: str) -> None:
    raise SystemExit(f"reconcile-vt-analyses: {message}")


def read_scan_set(path_text: str) -> List[Tuple[str, str]]:
    path = pathlib.Path(path_text).absolute()
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 16 * 1024 * 1024:
        fail(f"missing, unsafe or oversized scan set: {path}")
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != "# cbm-release-scan-set-v2":
        fail("scan-set marker is missing")
    cursor = 1
    while cursor < len(lines) and lines[cursor].startswith("# "):
        cursor += 1
    if cursor >= len(lines) or lines[cursor] != "\t".join(SCAN_FIELDS):
        fail("scan-set header is malformed")
    objects: List[Tuple[str, str]] = []
    seen: set[str] = set()
    for line in lines[cursor + 1 :]:
        cells = line.split("\t")
        if len(cells) != len(SCAN_FIELDS):
            fail(f"scan-set row is malformed: {line}")
        row = dict(zip(SCAN_FIELDS, cells))
        scan_path, sha256 = row["scan_path"], row["sha256"]
        pure = pathlib.PurePosixPath(scan_path)
        if (
            pure.is_absolute()
            or len(pure.parts) != 2
            or pure.parts[0] != "objects"
            or scan_path in seen
            or SHA256_RE.fullmatch(sha256) is None
        ):
            fail(f"scan-set row is invalid: {line}")
        seen.add(scan_path)
        objects.append((scan_path, sha256))
    if not objects:
        fail("scan set is empty")
    return objects


def read_withheld(path_text: str) -> set[str]:
    if not path_text:
        return set()
    path = pathlib.Path(path_text).absolute()
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 1024 * 1024:
        fail(f"missing or unsafe withheld manifest: {path}")
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != "# cbm-virustotal-withheld-v1":
        fail("withheld manifest marker is missing")
    cursor = 1
    while cursor < len(lines) and lines[cursor].startswith("# "):
        cursor += 1
    if cursor >= len(lines) or lines[cursor] != "sha256\tobject":
        fail("withheld manifest header is malformed")
    hashes: set[str] = set()
    for line in lines[cursor + 1 :]:
        sha256 = line.partition("\t")[0]
        if SHA256_RE.fullmatch(sha256) is None:
            fail(f"withheld row is malformed: {line}")
        hashes.add(sha256)
    return hashes


def parse_action_output(raw: str, aliases: Dict[str, str]) -> Dict[str, str]:
    covered: Dict[str, str] = {}
    if not raw:
        return covered
    for entry in raw.split(","):
        name, separator, url = entry.partition("=")
        if not separator or not name or not url or entry != entry.strip():
            fail(f"malformed VirusTotal action output entry: {entry}")
        scan_path = aliases.get(name)
        if scan_path is None:
            fail(f"VirusTotal action returned an unexpected path: {name}")
        if scan_path in covered:
            fail(f"VirusTotal action returned a duplicate path: {name}")
        covered[scan_path] = url
    return covered


def request_reanalysis(sha256: str, api_key: str) -> str:
    endpoint = ANALYSE_ENDPOINT.format(sha256=sha256)
    for attempt in range(4):
        if attempt:
            time.sleep(60)
        result = subprocess.run(
            [
                "curl",
                "-sS",
                "--max-time",
                "120",
                "-w",
                "\n%{http_code}",
                "-X",
                "POST",
                "-H",
                f"x-apikey: {api_key}",
                endpoint,
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            fail(f"reanalysis request failed for {sha256}: {result.stderr.strip()}")
        body, _, status = result.stdout.rpartition("\n")
        if status == "200":
            try:
                identifier = json.loads(body)["data"]["id"]
            except (ValueError, KeyError, TypeError):
                fail(f"reanalysis response is malformed for {sha256}")
            if not isinstance(identifier, str) or not identifier:
                fail(f"reanalysis response has no analysis id for {sha256}")
            return identifier
        if status == "404":
            fail(
                f"object is unknown to VirusTotal and was never uploaded: {sha256} "
                "(the upload step failed for a new object; see its log)"
            )
        if status != "429":
            fail(f"reanalysis request rejected for {sha256}: HTTP {status}: {body[:200]}")
    fail(f"reanalysis stayed rate-limited for {sha256}")


api_key = sys.argv[1]
objects = read_scan_set(sys.argv[2])
withheld = read_withheld(sys.argv[3])

aliases: Dict[str, str] = {}
for scan_path, sha256 in objects:
    candidates = (scan_path, pathlib.PurePosixPath(scan_path).name, f"binaries/{scan_path}")
    for alias in candidates:
        previous = aliases.get(alias)
        if previous is not None and previous != scan_path:
            fail(f"ambiguous action-output alias: {alias}")
        aliases[alias] = scan_path

expected = {sha256: scan_path for scan_path, sha256 in objects if sha256 not in withheld}
if not expected:
    fail("every expected object is withheld; nothing to reconcile")
covered = parse_action_output(sys.argv[4], aliases)

urls: Dict[str, str] = {}
for scan_path, url in covered.items():
    urls[scan_path] = url
for sha256, scan_path in sorted(expected.items()):
    if scan_path in urls:
        continue
    time.sleep(15)
    urls[scan_path] = ANALYSIS_URL.format(identifier=request_reanalysis(sha256, api_key))

if set(urls) < set(expected.values()):
    missing = sorted(set(expected.values()) - set(urls))
    fail(f"coverage is incomplete after reconciliation: {missing}")
distinct = set(urls.values())
if len(distinct) != len(urls):
    fail("one analysis was reused for multiple scan objects")

with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
    output.write("analysis=" + ",".join(f"{path}={url}" for path, url in sorted(urls.items())) + "\n")
print(
    f"reconcile-vt-analyses: {len(covered)} action analyses kept, "
    f"{len(urls) - len(covered)} reanalysis URL(s) added"
)
PY
