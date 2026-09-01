/*
 * repro_issue480.c — Reproduce-first case for OPEN bug #480.
 *
 * Issue: #480 — "trace_path returns empty for all functions despite
 *               traversable CALLS edges (v0.8.1, macOS arm64)"
 *
 * Root cause (identified by maintainer DeusData + reporter halindrome):
 *   handle_trace_call_path() calls cbm_store_find_nodes_by_name() to locate
 *   the start node for BFS.  On the affected build, the name-to-node lookup
 *   returns node_count == 0 for EVERY function name — even names that the
 *   graph clearly contains (confirmed by query_graph Cypher returning the same
 *   function with 5–8 inbound CALLS edges).  The fallback to
 *   cbm_store_find_node_by_qn() also returns nothing, so the handler exits
 *   with a "function not found" error OR (when the node IS found by name)
 *   the BFS start-node id does not match any edge endpoint stored in the
 *   graph, so cbm_store_bfs() returns visited_count == 0 and the "callers"
 *   / "callees" JSON arrays are serialised empty.
 *
 *   The split: query_graph Cypher (direct SQL) traverses the same edges
 *   correctly, while trace_path (BFS via start-node id) yields nothing.
 *   This isolates the bug to trace_path's own start-node lookup or to how
 *   the resolved node id is passed to cbm_store_bfs(), NOT to edge creation.
 *
 * Expected (correct) behaviour:
 *   After indexing a two-function Python file where caller() calls callee(),
 *   trace_path for "callee" with direction="inbound" must return a non-empty
 *   "callers" array that contains a node named "caller".
 *
 * Actual (buggy) behaviour:
 *   trace_path returns {"function":"callee","direction":"inbound","callers":[]}
 *   — an empty "callers" array — even though CALLS edges exist in the graph
 *   and are walkable via query_graph.
 *
 * Why RED on current code:
 *   The precondition assertion (CALLS edges > 0) passes because edge creation
 *   is correct.  The subsequent assertion that resp contains the string
 *   "\"caller\"" (the caller function's name embedded in the callers array)
 *   FAILS because cbm_store_bfs() finds no hops from the resolved start node.
 *
 * How this isolates the traversal bug from an extraction bug:
 *   If CALLS edges were the problem, rh_count_edges(store, …, "CALLS") would
 *   return 0 and the ASSERT_GT precondition would fire RED — visibly flagging
 *   an extraction failure instead.  By asserting the precondition GREEN and
 *   the trace_path result RED, we prove the edges exist and the fault lies
 *   exclusively in trace_path's traversal layer.
 *
 * Fixed path:
 *   trace_path now resolves the start node against the same project-scoped
 *   graph identifiers used by cbm_store_bfs(), so the name lookup and edge
 *   endpoints remain compatible.
 */

#include <foundation/compat.h>
#include "test_framework.h"
#include "repro_harness.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Fixture ────────────────────────────────────────────────────────────────
 *
 * Two Python functions in one file:
 *
 *   def callee():
 *       return 42
 *
 *   def caller():
 *       return callee()
 *
 * Python has proven reliable CALLS extraction (test_extraction.c:python_calls
 * asserts calls.count > 0 for a simpler fixture; the integration suite's
 * main.py fixture yields CALLS edges that are visible via query_graph).
 * caller() → callee() is a simple, unambiguous intra-file call: the extractor
 * sees exactly one callee() call expression inside caller(), so the graph
 * must have ≥ 1 CALLS edge after indexing.
 */
static const RFile k_files[] = {
    {
        "main.py",
        "def callee():\n"
        "    return 42\n"
        "\n"
        "def caller():\n"
        "    return callee()\n"
    }
};

/* ─────────────────────────────────────────────────────────────────────────
 * repro_issue480_trace_path_nonempty_with_calls
 *
 * Precondition (must be GREEN to prove this is a traversal bug):
 *   rh_count_edges(store, project, "CALLS") > 0
 *
 * The failing assertion (RED on buggy code):
 *   The "callers" array in the trace_path response is non-empty and contains
 *   the string "caller" (the name of the caller function).
 * ───────────────────────────────────────────────────────────────────────── */
TEST(repro_issue480_trace_path_nonempty_with_calls) {
    RProj lp;
    cbm_store_t *store = rh_index_files(&lp, k_files,
                                        (int)(sizeof(k_files) / sizeof(k_files[0])));
    ASSERT_NOT_NULL(store);

    /* ── Precondition: extraction must have produced ≥ 1 CALLS edge ──────
     * If this fires RED, the fixture or language has an extraction bug —
     * that is a different problem from #480.  Switch to a different
     * language fixture (e.g. Go utils.go with Multiply→Add) in that case. */
    int calls_count = rh_count_edges(store, lp.project, "CALLS");
    ASSERT_GT(calls_count, 0);

    /* ── Invoke trace_path for "callee" with direction="inbound" ─────────
     *
     * Args match the trace_path schema (required: function_name, project):
     *   function_name  — bare name "callee"; also tested by the reporter with
     *                    the fully-qualified name, both yield empty on buggy code
     *   project        — lp.project (derived from tmpdir by cbm_project_name_from_path)
     *   direction      — "inbound": ask for callers of callee()
     *   depth          — 2: enough to reach one hop (caller → callee)
     *
     * Expected response shape (correct):
     *   {"function":"callee","direction":"inbound","callers":[{"name":"caller",...},...]}
     *
     * Buggy response shape:
     *   {"function":"callee","direction":"inbound","callers":[]}
     *   (or: {"error":"function not found",...} if the name lookup fails entirely)
     */
    char args[512];
    snprintf(args, sizeof(args),
             "{\"function_name\":\"callee\","
             "\"project\":\"%s\","
             "\"direction\":\"inbound\","
             "\"depth\":2}",
             lp.project);

    char *resp = cbm_mcp_handle_tool(lp.srv, "trace_path", args);
    ASSERT_NOT_NULL(resp);

    /* The response must NOT be a "function not found" error.
     * If the name lookup itself fails, this fires first and pinpoints the
     * start-node lookup as the breakage site. */
    ASSERT_NULL(strstr(resp, "function not found"));

    /* The default trace_path response is the compact tree table. The
     * structured JSON envelope is deliberately not the default because MCP
     * clients consume the tree representation directly. */
    ASSERT_NOT_NULL(strstr(resp, "callers:"));

    /* The "callers" table must be NON-EMPTY. A populated, correctly named
     * caller hop is required. The tree format factors the module prefix into
     * its own header, so the row itself contains the terminal name. */
    ASSERT_NULL(strstr(resp, "callers: 0")); /* empty result = traversal bug */
    ASSERT_NOT_NULL(strstr(resp, "  caller ")); /* caller row in results */

    free(resp);
    rh_cleanup(&lp, store);
    PASS();
}

/* ── Suite ─────────────────────────────────────────────────────────────── */
SUITE(repro_issue480) {
    RUN_TEST(repro_issue480_trace_path_nonempty_with_calls);
}
