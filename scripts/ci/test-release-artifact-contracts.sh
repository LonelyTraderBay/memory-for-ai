#!/usr/bin/env bash
# Run release-selection and archive contracts that do not require a product build.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

usage() {
    cat <<'EOF'
Usage: scripts/ci/test-release-artifact-contracts.sh

Run release-candidate selection, archive extraction, and gate-chain contracts.
These source/artifact checks do not certify a product binary on this host.
EOF
}

case "${1:-}" in
-h | --help)
    usage
    exit 0
    ;;
"") ;;
*)
    echo "test-release-artifact-contracts.sh: unexpected argument '$1'. Please consult --help." >&2
    exit 2
    ;;
esac

cd "$ROOT"
bash tests/test_vt_candidate_selection_contract.sh
bash tests/test_release_archive_extractor_contract.sh
bash tests/test_release_gate_chain_contract.sh
