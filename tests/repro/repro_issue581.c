// repro_issue581.c -- Regression coverage for the #581 query-memory fix.
//
// Issue: #581 -- "Memory leak: process grows to 50+ GB virtual memory over
//               hours/days, crashes Windows"
//   https://github.com/LonelyTraderBay/memory-for-ai/issues/581
//
// OBSERVED BEHAVIOUR:
//   memory-for-ai in stdio MCP server mode grows from ~12 MB working
//   set to 50-107 GB virtual memory over 12-48 hours while the agent issues
//   repeated queries (search_graph, query_graph, get_architecture, etc.).
//   The reporter confirmed auto_index=false, so indexing is NOT the growth
//   path -- the leak occurs purely from query/read operations.
//
// ROOT-CAUSE HYPOTHESIS (two-part):
//
//   1. SQLite WAL file: every query-only store open uses WAL journal mode
//      (configure_pragmas, store.c:343) and mmap_size=64 MB
//      (store.c:355-358).  The WAL file accumulates un-checkpointed frames
//      on every write-side flush (which happens from other operations even
//      on a "read-only" query session because SQLite WAL readers also
//      participate in the WAL protocol).  The only checkpoint in the MCP
//      event loop is SQLITE_CHECKPOINT_PASSIVE, which never ftruncates
//      (mcp.c:869).  Over thousands of operations the WAL grows without
//      bound, with each page mapped via mmap into virtual address space.
//
//   2. mimalloc page retention: cbm_mem_collect() is called after
//      index_repository (mcp.c:2866, 4616) and after delete_project
//      (mcp.c:1860), but NEVER after query operations.  mimalloc retains
//      freed arena pages in its internal free-lists so they show up as
//      committed virtual memory (visible on Windows as "commit charge")
//      even after the query result is freed.
//
//   The combination -- SQLite WAL mapped pages + mimalloc retained pages
//   not returned to OS -- accumulates monotonically across thousands of
//   query iterations without any compaction trigger.
//
// BOUNDED REPRODUCTION STRATEGY:
//   Repeat a single MCP query tool call (search_graph) N=150 times against
//   a small indexed project.  Measure current RSS (not peak) at warmup
//   (iteration 10) and at the end (iteration 150).  Assert that end RSS is
//   not more than LEAK_FACTOR x warmup RSS.
//
//   The real-world leak is 50 GB over hours (~thousands of operations).
//   Per-query accumulation is therefore large but the signal over 150
//   iterations is proportionally small.  We choose a generous threshold
//   (3.0x) so a truly bounded implementation passes easily, while a
//   genuinely leaking implementation that retains ~10-100 kB per query
//   accumulates enough to exceed 3x warmup after 150 iterations (at
//   10 kB/call on a 30 MB baseline: 30 MB + 1.5 MB = 1.05x -- borderline).
//
// IMPORTANT CAVEATS / FLAKINESS NOTES:
//
//   (a) RSS MEASUREMENT: we use cbm_mem_rss() (src/foundation/mem.c) which
//       calls mi_process_info() for current RSS, or falls back to
//       /proc/self/statm (Linux), mach_task_basic_info.resident_size (macOS),
//       or GetProcessMemoryInfo.WorkingSetSize (Windows).  This is CURRENT
//       RSS, not peak -- suitable for detecting steady-state growth.
//
//   (b) ASan BUILD PITFALL: the repro runner uses ASAN_OPTIONS=detect_leaks=0,
//       so LSan won't catch this class of leak here (mimalloc/WAL accumulated
//       pages are not classically leaked -- they are reachable but never freed).
//       This test is an RSS-growth test, not a LSan test.  ASan instrumentation
//       inflates per-allocation overhead ~3x; iteration count (150) is chosen
//       conservatively to stay well within CI time budgets even with ASan.
//
//   (c) THRESHOLD 3.0x: the warmup RSS includes the full SQLite page cache
//       and mimalloc initial arenas.  On an 8-core machine warmup may be
//       50-100 MB; 3x would be 150-300 MB, achievable with a bad leak rate of
//       ~1 MB/query over 150 queries.  On a FIXED implementation the end RSS
//       should be close to 1.0-1.2x warmup (GC cycle, small jitter).
//       If this test produces a false FAIL on a correct implementation (warmup
//       RSS is very small, e.g. 5 MB, and allocator variance causes spike), the
//       threshold can be increased to 4x or the warmup moved later; this is
//       documented as a known-fragile point.
//
//   (d) LINUX-ONLY ALTERNATIVE: if cbm_mem_rss() returns 0 (e.g. MI_OVERRIDE=0
//       without the OS fallback compiled), the test falls back to reading
//       /proc/self/statm directly below.  On macOS and Windows cbm_mem_rss()
//       is expected to return non-zero.  If all RSS readings are zero the test
//       is declared inconclusive and PASSES to avoid false failures (the
//       growth assertion requires reliable RSS readings).
//
// FIX STATUS:
//   Request-scoped file stores are closed after every MCP tool call, cached
//   statements are finalized before the close-path checkpoint, and allocator
//   pages are collected after request/idle/shutdown store eviction. Shared
//   databases still use PASSIVE checkpoints deliberately: TRUNCATE is unsafe
//   while a sibling process can hold a live mmap'd WAL view.

#include "test_framework.h"
#include "repro_harness.h"
#include <foundation/mem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Number of search_graph calls per trial.
// 10 warmup + 140 measurement = 150 total.
// Deliberately modest to stay within CI time budgets even with ASan.
#define ITER_WARMUP   10
#define ITER_TOTAL   150

// Generous RSS growth multiplier: end RSS must not exceed LEAK_FACTOR x
// warmup RSS.  A correct implementation stays near 1.0-1.2x; a leaking
// implementation grows linearly.
// Set to 3.0 to tolerate allocator variance while still catching a real leak
// of >1 MB per query over 140 post-warmup iterations.
#define LEAK_FACTOR  3.0

// Fallback current-RSS reader for Linux, used if cbm_mem_rss() returns 0
// (MI_OVERRIDE=0 with no OS fallback compiled in).  Returns 0 if unavailable.
static size_t rss_bytes(void) {
    size_t v = cbm_mem_rss();
    if (v > 0) {
        return v;
    }
#if defined(__linux__)
    // /proc/self/statm: fields are "VmSize VmRSS ..." in pages
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) {
        return 0;
    }
    unsigned long vm_pages = 0;
    unsigned long rss_pages = 0;
    if (fscanf(f, "%lu %lu", &vm_pages, &rss_pages) != 2) {
        rss_pages = 0;
    }
    fclose(f);
    long ps = sysconf(_SC_PAGESIZE);
    return rss_pages * (size_t)(ps > 0 ? (unsigned long)ps : 4096UL);
#else
    return 0;
#endif
}

// Small fixture: a tiny Python module with a few functions.
// Chosen to produce a small but real graph (~5 nodes/edges) so that
// search_graph hits the actual SQLite code path including FTS5 lookup,
// node scan, and JSON serialisation -- replicating the real query workload.
static const char FIXTURE_PY[] =
    "def add(a, b):\n"
    "    return a + b\n"
    "\n"
    "def multiply(a, b):\n"
    "    result = a * b\n"
    "    return result\n"
    "\n"
    "def greet(name):\n"
    "    msg = 'hello ' + name\n"
    "    print(msg)\n"
    "    return msg\n";

// search_graph args JSON for repeated queries.
// Uses a broad name_pattern so results are always non-empty (exercises both
// the FTS5 and regex code paths and forces JSON result allocation + free).
static const char SEARCH_ARGS[] =
    "{\"project\":\"__PROJ__\","
    "\"name_pattern\":\".*\","
    "\"limit\":10}";

// Build the args string with the real project name substituted.
// Caller must free the returned string.
static char *build_search_args(const char *project) {
    const char *tmpl = SEARCH_ARGS;
    const char *marker = "__PROJ__";
    const char *pos = strstr(tmpl, marker);
    if (!pos || !project) {
        return NULL;
    }
    size_t prefix_len = (size_t)(pos - tmpl);
    size_t proj_len = strlen(project);
    size_t suffix_len = strlen(pos + strlen(marker));
    size_t total = prefix_len + proj_len + suffix_len + 1;
    char *out = malloc(total);
    if (!out) {
        return NULL;
    }
    memcpy(out, tmpl, prefix_len);
    memcpy(out + prefix_len, project, proj_len);
    memcpy(out + prefix_len + proj_len, pos + strlen(marker), suffix_len + 1);
    return out;
}

// repro_issue581_query_rss_stable
//
// Asserts that RSS does not grow monotonically when search_graph is called
// repeatedly against a single indexed project.
//
// The test also checks the direct request-store lifecycle invariant and uses
// RSS as a coarse signal. It is intentionally small; it is not a substitute
// for the manual large-repository soak that originally exposed #581.
//
// NOTE on ITER_WARMUP/ITER_TOTAL calibration:
//   The real leak is ~10 GB/day with an active agent (rough rate:
//   10 GB / 86400 s * avg call interval).  We cannot reproduce that scale
//   in CI, so we rely on the leak being MONOTONIC -- any growth per iteration
//   shows up as a slope over 150 iterations.  If the leak rate is so slow
//   that even 150x does not visibly move RSS beyond allocator jitter, this
//   test may not fire RED on every CI run (documented flakiness risk above).
TEST(repro_issue581_query_rss_stable) {
    RFile files[] = {{"module.py", FIXTURE_PY}};
    RProj lp;
    cbm_store_t *store = rh_index_files(&lp, files, 1);
    ASSERT_NOT_NULL(store);

    // Project name from the harness.
    const char *project = lp.project;
    ASSERT_NOT_NULL(project);

    char *args = build_search_args(project);
    ASSERT_NOT_NULL(args);

    size_t rss_warmup = 0;
    size_t rss_end = 0;

    for (int i = 0; i < ITER_TOTAL; i++) {
        char *resp = cbm_mcp_handle_tool(lp.srv, "search_graph", args);
        // The response must be freed on every call -- verifying the MCP layer
        // does not itself accumulate the result (it doesn't; the leak is lower).
        if (resp) {
            free(resp);
        }
        /* File-backed query stores must not survive the request boundary. This
         * is the direct lifecycle invariant behind the #581 hardening. */
        ASSERT_FALSE(cbm_mcp_server_has_cached_store(lp.srv));

        if (i + 1 == ITER_WARMUP) {
            rss_warmup = rss_bytes();
        }
    }

    rss_end = rss_bytes();

    free(args);
    rh_cleanup(&lp, store);

    if (rss_warmup > 0 && rss_end > 0) {
        printf("  rss_warmup_kb=%zu rss_end_kb=%zu factor=%.2f threshold=%.1f\n", rss_warmup / 1024,
               rss_end / 1024, (double)rss_end / (double)rss_warmup, LEAK_FACTOR);
    } else {
        printf("  NOTE: RSS not measurable on this platform/build\n");
    }

    /* This small fixture is a bounded smoke guard, not a claim that 150 calls
     * reproduce the historical multi-day large-repository leak. The production
     * fix is covered directly by request-store eviction and allocator cleanup;
     * RSS remains a coarse regression signal. */
    if (rss_warmup > 0 && rss_end > 0) {
        ASSERT_TRUE((double)rss_end <= (double)rss_warmup * LEAK_FACTOR);
    }
    PASS();
}

// -- Suite ------------------------------------------------------------------

SUITE(repro_issue581) {
    RUN_TEST(repro_issue581_query_rss_stable);
}
