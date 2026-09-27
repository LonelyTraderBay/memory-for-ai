#!/usr/bin/env bash
# benchmark-search-graph.sh — Time search_graph name_pattern= queries against a
# memory-for-ai binary to measure the regex / LIKE pre-filter performance.
#
# Usage:
#   scripts/benchmark-search-graph.sh <binary-path> <project-name>
#
# Example:
#   scripts/benchmark-search-graph.sh ./build/c/memory-for-ai my-project

set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

BINARY="${1:?Usage: $0 <binary-path> <project-name>}"
PROJECT="${2:?Usage: $0 <binary-path> <project-name>}"

echo "Binary:  $BINARY"
echo "Project: $PROJECT"
echo ""

run_case() {
    local label="$1"
    local request="$2"
    local start end elapsed_ms result

    start=$(python3 -c "import time; print(time.perf_counter_ns() // 1000000)")
    result=$("$BINARY" cli search_graph "$request")
    end=$(python3 -c "import time; print(time.perf_counter_ns() // 1000000)")
    elapsed_ms=$(( end - start ))

    local count
    count=$(printf '%s\n' "$result" | python3 "$SCRIPT_DIR/benchmark-response.py" search)

    printf "  %-55s %5dms  (total=%s)\n" "$label" "$elapsed_ms" "$count"
}

sg() {
    local project="$1"
    local args="$2"
    python3 -c 'import json,sys
args=json.loads("{"+sys.argv[2]+"}")
args.update(project=sys.argv[1],format="json")
print(json.dumps(args))' "$project" "$args"
}

echo "=== search_graph name_pattern= benchmarks ==="
run_case "name_pattern=.*Controller.*"         "$(sg "$PROJECT" '"name_pattern":".*Controller.*","limit":20')"
run_case "name_pattern=.*Service.*"            "$(sg "$PROJECT" '"name_pattern":".*Service.*","limit":20')"
run_case "name_pattern=.*Repository.*"         "$(sg "$PROJECT" '"name_pattern":".*Repository.*","limit":20')"
run_case "name_pattern=specificFunctionName"   "$(sg "$PROJECT" '"name_pattern":"specificFunctionName","limit":20')"
run_case "label=Method + name_pattern=.*get.*" "$(sg "$PROJECT" '"label":"Method","name_pattern":".*get.*","limit":20')"

echo ""
echo "=== search_graph query= benchmarks (BM25 path) ==="
run_case "query=controller service handler"                   "$(sg "$PROJECT" '"query":"controller service handler","limit":20')"
run_case "query=user authentication permission role"          "$(sg "$PROJECT" '"query":"user authentication permission role","limit":20')"
run_case "query=create update delete manage list view admin"  "$(sg "$PROJECT" '"query":"create update delete manage list view admin","limit":20')"
