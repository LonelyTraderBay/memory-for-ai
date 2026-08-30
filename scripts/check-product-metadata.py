#!/usr/bin/env python3
"""Check product identity and published capability metadata.

The native source, package manifests, documentation, and smoke tests used to
carry independent copies of the product name and MCP tool count. This check
keeps values that are safe to derive from source synchronized without requiring
a native compiler.

Run from anywhere:

    python3 scripts/check-product-metadata.py

Use --write after adding a tool or changing the release version to refresh the
generated metadata file and the active documentation claims. Release-specific
package manifests containing checksums are deliberately not rewritten.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PRODUCT_HEADER = ROOT / "src" / "foundation" / "product.h"
MCP_SOURCE = ROOT / "src" / "mcp" / "mcp.c"
SERVER_JSON = ROOT / "server.json"
METADATA_JSON = ROOT / "docs" / "product-metadata.json"


def fail(message: str) -> None:
    raise RuntimeError(message)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        fail(f"cannot read {path.relative_to(ROOT)}: {exc}")


def product_macros() -> dict[str, str]:
    text = read_text(PRODUCT_HEADER)
    values = dict(
        re.findall(
            r'^#define\s+(CBM_PRODUCT_[A-Z0-9_]+)\s+"([^"]*)"',
            text,
            re.MULTILINE,
        )
    )
    required = {
        "CBM_PRODUCT_NAME",
        "CBM_PRODUCT_REPOSITORY",
        "CBM_PRODUCT_REPOSITORY_URL",
        "CBM_PRODUCT_CACHE_ENV",
        "CBM_PRODUCT_RUNTIME_ENV",
        "CBM_PRODUCT_INDEX_LOG_ENV",
    }
    missing = sorted(required - values.keys())
    if missing:
        fail(f"product.h is missing macros: {', '.join(missing)}")
    return values


def tool_names() -> list[str]:
    text = read_text(MCP_SOURCE)
    match = re.search(
        r"static const tool_def_t TOOLS\[\] = \{(?P<body>.*?)\n\};",
        text,
        re.DOTALL,
    )
    if not match:
        fail("could not locate TOOLS[] in src/mcp/mcp.c")
    names = re.findall(r'^\s*\{"([^"]+)"', match.group("body"), re.MULTILINE)
    if not names:
        fail("TOOLS[] in src/mcp/mcp.c is empty or has an unexpected format")
    if len(names) != len(set(names)):
        fail("TOOLS[] contains duplicate tool names")
    return names


def server_metadata() -> dict:
    try:
        return json.loads(read_text(SERVER_JSON))
    except json.JSONDecodeError as exc:
        fail(f"server.json is invalid JSON: {exc}")


def canonical_metadata() -> dict:
    macros = product_macros()
    server = server_metadata()
    names = tool_names()
    repository = macros["CBM_PRODUCT_REPOSITORY"]
    repository_url = macros["CBM_PRODUCT_REPOSITORY_URL"]
    expected_mcp_name = f"io.github.{repository}"

    if server.get("name") != expected_mcp_name:
        fail(f"server.json name is {server.get('name')!r}, expected {expected_mcp_name!r}")
    if server.get("repository", {}).get("url") != repository_url:
        fail("server.json repository URL does not match product.h")
    if server.get("websiteUrl", "").rstrip("/") != repository_url:
        fail("server.json websiteUrl does not match product.h")
    version = server.get("version")
    if not isinstance(version, str) or not re.fullmatch(r"\d+\.\d+\.\d+", version):
        fail(f"server.json has invalid semantic version: {version!r}")

    return {
        "product_name": macros["CBM_PRODUCT_NAME"],
        "display_name": server.get("title", "Memory for AI"),
        "repository": repository,
        "repository_url": repository_url,
        "version": version,
        "mcp_tool_count": len(names),
        "mcp_tools": names,
        "language_count": 162,
        "agent_surface_count": 45,
        "cache_env": macros["CBM_PRODUCT_CACHE_ENV"],
        "runtime_env": macros["CBM_PRODUCT_RUNTIME_ENV"],
        "index_log_env": macros["CBM_PRODUCT_INDEX_LOG_ENV"],
    }


def update_text(path: Path, replacements: list[tuple[str, str]]) -> bool:
    text = read_text(path)
    original = text
    for pattern, replacement in replacements:
        text = re.sub(pattern, replacement, text)
    if text == original:
        return False
    path.write_text(text, encoding="utf-8", newline="")
    return True


def write_generated_metadata(metadata: dict) -> None:
    METADATA_JSON.write_text(
        json.dumps(metadata, indent=2) + "\n",
        encoding="utf-8",
        newline="",
    )


def write_active_docs(metadata: dict) -> list[str]:
    count = str(metadata["mcp_tool_count"])
    languages = str(metadata["language_count"])
    version = metadata["version"]
    changed: list[str] = []

    replacements = {
        "README.md": [
            (r"\b\d+ MCP tools\b", f"{count} MCP tools"),
            (r"\b\d+ vendored tree-sitter grammars\b", f"{languages} vendored tree-sitter grammars"),
        ],
        "pkg/npm/README.md": [
            (r"\b\d+ MCP tools\b", f"{count} MCP tools"),
            (r"\b\d+ vendored tree-sitter grammars\b", f"{languages} vendored tree-sitter grammars"),
        ],
        "docs/llms.txt": [
            (r"Languages: \d+ \(\d+ vendored", f"Languages: {languages} ({languages} vendored"),
            (r"MCP tools: \d+\b", f"MCP tools: {count}"),
        ],
        "docs/index.html": [
            (r'"softwareVersion": "[^"]+"', f'"softwareVersion": "{version}"'),
            (r"\b\d+ languages\b", f"{languages} languages"),
            (r"Indexes \d+ programming languages", f"Indexes {languages} programming languages"),
            (r"\b\d+ MCP tools\b", f"{count} MCP tools"),
        ],
    }
    for relative, patterns in replacements.items():
        path = ROOT / relative
        if update_text(path, patterns):
            changed.append(relative)
    return changed


def check_package_versions(metadata: dict) -> list[str]:
    """Check manifests whose version is the current source-of-truth version.

    Package archives with embedded checksums are release snapshots and can
    legitimately lag until the next archive is published; their release
    workflows validate them separately.
    """

    version = metadata["version"]
    mismatches: list[str] = []
    json_path = ROOT / "pkg" / "npm" / "package.json"
    if json.loads(read_text(json_path)).get("version") != version:
        mismatches.append("pkg/npm/package.json")
    if not re.search(
        r'^version\s*=\s*"' + re.escape(version) + r'"',
        read_text(ROOT / "pkg" / "pypi" / "pyproject.toml"),
        re.MULTILINE,
    ):
        mismatches.append("pkg/pypi/pyproject.toml")
    if not re.search(
        r'version\s*=\s*"' + re.escape(version) + r'"',
        read_text(ROOT / "pkg" / "go" / "cmd" / "memory-for-ai" / "main.go"),
    ):
        mismatches.append("pkg/go/cmd/memory-for-ai/main.go")
    return mismatches


def check_docs(metadata: dict) -> list[str]:
    errors: list[str] = []
    expected_count = str(metadata["mcp_tool_count"])
    for relative in ("README.md", "pkg/npm/README.md", "docs/llms.txt", "docs/index.html"):
        text = read_text(ROOT / relative)
        values = re.findall(r"\b(\d+) MCP tools\b", text)
        if any(value != expected_count for value in values):
            errors.append(f"{relative}: MCP tool count is not {expected_count}")
        if re.search(
            r"\b158 languages\b|Languages: 158|158 vendored|Indexes 158 programming",
            text,
        ):
            errors.append(f"{relative}: stale language count 158")
    return errors


def check_smoke_expectations(metadata: dict) -> list[str]:
    text = read_text(ROOT / "scripts" / "smoke-invariants.sh")
    errors: list[str] = []
    count_match = re.search(r"^EXPECTED_TOOL_COUNT=(\d+)\s*$", text, re.MULTILINE)
    if not count_match or int(count_match.group(1)) != metadata["mcp_tool_count"]:
        errors.append("scripts/smoke-invariants.sh: EXPECTED_TOOL_COUNT is stale")
    names_match = re.search(r'^EXPECTED_TOOLS="([^"]*)"\s*$', text, re.MULTILINE)
    expected = " ".join(metadata["mcp_tools"])
    if not names_match or names_match.group(1) != expected:
        errors.append("scripts/smoke-invariants.sh: EXPECTED_TOOLS does not match src/mcp/mcp.c")
    return errors


def run(write: bool) -> int:
    try:
        metadata = canonical_metadata()
        if write:
            write_generated_metadata(metadata)
            changed = write_active_docs(metadata)
            if changed:
                print("updated: " + ", ".join(changed))

        errors: list[str] = []
        if not METADATA_JSON.exists():
            errors.append("docs/product-metadata.json is missing; run with --write")
        else:
            try:
                generated = json.loads(read_text(METADATA_JSON))
            except json.JSONDecodeError as exc:
                errors.append(f"docs/product-metadata.json is invalid JSON: {exc}")
            else:
                if generated != metadata:
                    errors.append("docs/product-metadata.json is stale; run with --write")
        errors.extend(check_docs(metadata))
        errors.extend(check_smoke_expectations(metadata))
        errors.extend(
            f"{path}: version does not match server.json"
            for path in check_package_versions(metadata)
        )
        if errors:
            for error in errors:
                print(f"ERROR: {error}", file=sys.stderr)
            return 1
        print(
            f"product metadata OK: {metadata['product_name']} {metadata['version']} | "
            f"{metadata['mcp_tool_count']} MCP tools | {metadata['language_count']} languages"
        )
        return 0
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="refresh generated metadata and active docs")
    return run(parser.parse_args().write)


if __name__ == "__main__":
    raise SystemExit(main())
