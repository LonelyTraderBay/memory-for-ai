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
LANGUAGE_HEADER = ROOT / "internal" / "cbm" / "cbm.h"
LANGUAGE_SPECS = ROOT / "internal" / "cbm" / "lang_specs.c"
SEMANTIC_HEADER = ROOT / "src" / "semantic" / "semantic.h"
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


def language_metadata() -> dict[str, int]:
    """Derive language counts from the native registry instead of prose.

    The enum contains a few dialect/synthetic entries that do not have a
    Tree-sitter factory (for example ObjectScript export XML).  Keep those
    counts explicit so marketing documentation can describe the actual
    parser surface without pretending every enum value is a grammar.
    """

    enum_text = read_text(LANGUAGE_HEADER)
    enum_names = re.findall(r"^\s*CBM_LANG_[A-Z0-9_]+\s*(?:,|$)", enum_text, re.MULTILINE)
    enum_names = [name.strip().rstrip(",") for name in enum_names if name.strip() != "CBM_LANG_COUNT"]
    if not enum_names:
        fail("could not locate CBM_LANG_* registry in internal/cbm/cbm.h")

    specs_text = read_text(LANGUAGE_SPECS)
    spec_matches = list(
        re.finditer(r"^\s*\[CBM_LANG_([A-Z0-9_]+)\]\s*=", specs_text, re.MULTILINE)
    )
    spec_names = [match.group(1) for match in spec_matches]
    grammar_names: list[str] = []
    for index, match in enumerate(spec_matches):
        end = spec_matches[index + 1].start() if index + 1 < len(spec_matches) else len(specs_text)
        block = specs_text[match.end() : end]
        if re.search(r"tree_sitter_[A-Za-z0-9_]+\s*,", block):
            grammar_names.append(match.group(1))
    if not grammar_names:
        fail("could not locate Tree-sitter language factories in internal/cbm/lang_specs.c")

    return {
        "language_enum_count": len(enum_names),
        "language_spec_count": len(spec_names),
        "language_count": len(grammar_names),
    }


def semantic_metadata() -> dict[str, float]:
    """Read semantic edge policy from the native implementation header."""

    text = read_text(SEMANTIC_HEADER)
    match = re.search(
        r"^#define\s+CBM_SEM_EDGE_THRESHOLD\s+([0-9]+(?:\.[0-9]+)?)\s*$",
        text,
        re.MULTILINE,
    )
    if not match:
        fail("could not locate CBM_SEM_EDGE_THRESHOLD in src/semantic/semantic.h")
    return {"semantic_edge_threshold": float(match.group(1))}


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
    languages = language_metadata()
    semantic = semantic_metadata()

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
        **languages,
        **semantic,
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
    semantic_threshold = f"{metadata['semantic_edge_threshold']:.2f}"
    version = metadata["version"]
    changed: list[str] = []

    replacements = {
        "README.md": [
            (r"\b\d+ MCP tools\b", f"{count} MCP tools"),
            (r"languages-\d+", f"languages-{languages}"),
            (r"\b\d+ languages\b", f"{languages} languages"),
            (r"\b\d+ vendored tree-sitter grammars\b", f"{languages} vendored tree-sitter grammars"),
            (r"score ≥ \d+\.\d+", f"score ≥ {semantic_threshold}"),
            (r"(Channel detection.*?across )\d+( languages)", r"\g<1>8\g<2>"),
        ],
        "pkg/npm/README.md": [
            (r"\b\d+ MCP tools\b", f"{count} MCP tools"),
            (r"languages-\d+", f"languages-{languages}"),
            (r"\b\d+ languages\b", f"{languages} languages"),
            (r"\b\d+ vendored tree-sitter grammars\b", f"{languages} vendored tree-sitter grammars"),
            (r"score ≥ \d+\.\d+", f"score ≥ {semantic_threshold}"),
        ],
        "docs/llms.txt": [
            (r"\b\d+ languages\b", f"{languages} languages"),
            (r"Languages: \d+ \(\d+ vendored", f"Languages: {languages} ({languages} vendored"),
            (r"MCP tools: \d+\b", f"MCP tools: {count}"),
            (r"score ≥ \d+\.\d+", f"score ≥ {semantic_threshold}"),
        ],
        "docs/index.html": [
            (r'"softwareVersion": "[^"]+"', f'"softwareVersion": "{version}"'),
            (r"\b\d+ languages\b", f"{languages} languages"),
            (r"Indexes \d+ programming languages", f"Indexes {languages} programming languages"),
            (r"\b\d+ MCP tools\b", f"{count} MCP tools"),
            (r"scored ≥ \d+\.\d+", f"scored ≥ {semantic_threshold}"),
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
    expected_languages = str(metadata["language_count"])
    expected_threshold = f"{metadata['semantic_edge_threshold']:.2f}"
    for relative in ("README.md", "pkg/npm/README.md", "docs/llms.txt", "docs/index.html"):
        text = read_text(ROOT / relative)
        values = re.findall(r"\b(\d+) MCP tools\b", text)
        if any(value != expected_count for value in values):
            errors.append(f"{relative}: MCP tool count is not {expected_count}")
        # The channel detector intentionally covers a smaller, documented
        # language subset than the complete parser registry.
        check_text = re.sub(r"(?im)^.*Channel detection.*$", "", text)
        language_values = re.findall(r"\b(\d+) languages\b", check_text)
        if any(value != expected_languages for value in language_values):
            errors.append(f"{relative}: language count is not {expected_languages}")
        if re.search(r"Languages:\s*\d+\s*\(\d+ vendored", text):
            match = re.search(r"Languages:\s*(\d+)\s*\((\d+) vendored", text)
            if not match or match.group(1) != expected_languages or match.group(2) != expected_languages:
                errors.append(f"{relative}: vendored language count is not {expected_languages}")
        if re.search(r"Indexes\s+\d+ programming languages", text):
            match = re.search(r"Indexes\s+(\d+) programming languages", text)
            if not match or match.group(1) != expected_languages:
                errors.append(f"{relative}: indexed language count is not {expected_languages}")
        thresholds = re.findall(r"(?:scored|score) ≥ (\d+\.\d+)", text)
        if any(value != expected_threshold for value in thresholds):
            errors.append(f"{relative}: semantic threshold is not {expected_threshold}")
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
