#!/usr/bin/env bash
# update-pkg-manifests.sh — sync every in-repo package manifest to a release.
#
# Usage: scripts/update-pkg-manifests.sh <version> <checksums.txt>
#   <version>        release tag, with or without the leading v (e.g. v0.11.0)
#   <checksums.txt>  the release's checksum file ("<sha256>  <filename>" lines)
#
# Covers the manifests the release pipeline does NOT inject at publish time
# (npm/PyPI versions are already synced inside release.yml): scoop, chocolatey, winget. Every substitution is verified — the script fails
# closed rather than writing a half-synced manifest.
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <version> <checksums.txt>" >&2
    exit 2
fi
V="${1#v}"
CHECKSUMS="$2"
[ -f "$CHECKSUMS" ] || { echo "error: checksums file not found: $CHECKSUMS" >&2; exit 2; }

hash_of() {
    local name="$1" h
    h=$(awk -v f="$name" '$2 == f {print $1}' "$CHECKSUMS")
    if [ -z "$h" ]; then
        echo "error: no sha256 for $name in $CHECKSUMS" >&2
        exit 1
    fi
    printf '%s' "$h"
}

H_WIN_AMD64=$(hash_of memory-for-ai-windows-amd64.zip)

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO_ROOT"

# ── scoop ────────────────────────────────────────────────────────
SCOOP=pkg/scoop/memory-for-ai.json
python3 - "$SCOOP" "$V" "$H_WIN_AMD64" <<'PY'
import json, sys
path, v, h = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path, encoding="utf-8") as f:
    d = json.load(f)
d["version"] = v
a = d["architecture"]["64bit"]
a["url"] = f"https://github.com/LonelyTraderBay/memory-for-ai/releases/download/v{v}/memory-for-ai-windows-amd64.zip"
a["hash"] = h
with open(path, "w", encoding="utf-8") as f:
    json.dump(d, f, indent=2, ensure_ascii=False)
    f.write("\n")
PY
grep -q "\"version\": \"$V\"" "$SCOOP"
grep -q "$H_WIN_AMD64" "$SCOOP"

# ── chocolatey ───────────────────────────────────────────────────
NUSPEC=pkg/chocolatey/memory-for-ai.nuspec
sed -i \
    -e "s|<version>[^<]*</version>|<version>$V</version>|" \
    -e "s|/releases/tag/v[^<]*</releaseNotes>|/releases/tag/v$V</releaseNotes>|" \
    "$NUSPEC"
grep -q "<version>$V</version>" "$NUSPEC"

CHOCO=pkg/chocolatey/tools/chocolateyInstall.ps1
sed -i \
    -e "s|^\\\$version     = '.*'|\\\$version     = '$V'|" \
    -e "s|^\\\$checksum64  = '.*'|\\\$checksum64  = '$H_WIN_AMD64'|" \
    "$CHOCO"
grep -q "version     = '$V'" "$CHOCO"
grep -q "$H_WIN_AMD64" "$CHOCO"

# ── winget ───────────────────────────────────────────────────────
# Manifests are one directory per version; clone the latest as the template.
WINGET_ROOT=pkg/winget/manifests/d/LonelyTraderBay/MemoryForAi
LATEST=$(ls "$WINGET_ROOT" | sort -V | tail -1)
if [ "$LATEST" != "$V" ]; then
    if [ -d "$WINGET_ROOT/$V" ]; then
        echo "error: winget manifest dir already exists: $WINGET_ROOT/$V" >&2
        exit 1
    fi
    cp -r "$WINGET_ROOT/$LATEST" "$WINGET_ROOT/$V"
    for f in "$WINGET_ROOT/$V"/*.yaml; do
        sed -i "s/^PackageVersion: .*/PackageVersion: $V/" "$f"
    done
    INST="$WINGET_ROOT/$V/LonelyTraderBay.MemoryForAi.installer.yaml"
    sed -i \
        -e "s|/releases/download/v[^/]*/|/releases/download/v$V/|" \
        -e "s/^    InstallerSha256: .*/    InstallerSha256: $H_WIN_AMD64/" \
        "$INST"
    grep -q "PackageVersion: $V" "$INST"
    grep -q "$H_WIN_AMD64" "$INST"
fi

echo "pkg manifests synced to v$V"
