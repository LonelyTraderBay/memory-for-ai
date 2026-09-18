/*
 * test_edit_integration.c — End-to-end integration tests for the edit tools.
 *
 * Complements tests/test_edit.c (unit-level surgery on buffers) with the
 * full production flow promised by docs/THIET-KE-EDIT-TOOLS.md §8:
 *
 *   fixture project (Python + TypeScript + Go + C)
 *     → index through MCP (production path)
 *     → edit_symbol / delete_symbol / rename_symbol / undo_edit via MCP
 *     → synchronous incremental re-index inside the call
 *     → assert BOTH the on-disk source and the fresh graph state
 *
 * No mocking — real files, real parsing, real re-index.
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include <mcp/mcp.h>
#include <store/store.h>
#include <pipeline/pipeline.h>
#include <edit/edit.h>
#include <foundation/log.h>
#include <foundation/platform.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

/* ── Fixture: one temp project, five files, four languages ──────── */

static char g_tmpdir[256];
static char g_dbpath[512];
static cbm_mcp_server_t *g_srv = NULL;
static char *g_project = NULL;

static void write_fixture_file(const char *rel, const char *content) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_tmpdir, rel);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

/* calc.py — add/multiply/main call chain; multiply is the replace target. */
static const char *CALC_PY = "def add(a, b):\n"
                             "    return a + b\n"
                             "\n"
                             "def multiply(a, b):\n"
                             "    result = 0\n"
                             "    for _ in range(b):\n"
                             "        result = add(result, a)\n"
                             "    return result\n"
                             "\n"
                             "def main():\n"
                             "    print(multiply(2, 3))\n";

/* math.ts — square is called by doubleSquare (same-file rename target). */
static const char *MATH_TS = "export function square(x: number): number {\n"
                             "    return x * x;\n"
                             "}\n"
                             "\n"
                             "export function doubleSquare(x: number): number {\n"
                             "    return 2 * square(x);\n"
                             "}\n";

/* util.go — Compute calls Add; insert target. */
static const char *UTIL_GO = "package util\n"
                             "\n"
                             "func Add(a int, b int) int {\n"
                             "\treturn a + b\n"
                             "}\n"
                             "\n"
                             "func Compute(x int) int {\n"
                             "\treturn Add(x, 1)\n"
                             "}\n";

/* ops.c — unused_helper has no callers (safe delete), entry_point stands. */
static const char *OPS_C = "int unused_helper(int x) {\n"
                           "    return x * 2;\n"
                           "}\n"
                           "\n"
                           "int entry_point(int x) {\n"
                           "    return x + 1;\n"
                           "}\n";

/* drift.py — rewritten externally mid-suite to exercise the staleness gate. */
static const char *DRIFT_PY = "def stable_fn():\n"
                              "    return 42\n";

/* runner.py — imports and calls calc.add, giving rename a second file so the
 * partial-failure test can exercise a multi-file write. */
static const char *RUNNER_PY = "from calc import add\n"
                               "\n"
                               "def run():\n"
                               "    return add(1, 2)\n";

/* Expected contents after renaming every `add` occurrence to `plus`. */
static const char *CALC_PY_PLUS = "def plus(a, b):\n"
                                  "    return a + b\n"
                                  "\n"
                                  "def multiply(a, b):\n"
                                  "    result = 0\n"
                                  "    for _ in range(b):\n"
                                  "        result = plus(result, a)\n"
                                  "    return result\n"
                                  "\n"
                                  "def main():\n"
                                  "    print(multiply(2, 3))\n";

static const char *RUNNER_PY_PLUS = "from calc import plus\n"
                                    "\n"
                                    "def run():\n"
                                    "    return plus(1, 2)\n";

static int einteg_setup(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/cbm_edit_integ_XXXXXX");
    if (!cbm_mkdtemp(g_tmpdir))
        return -1;

    write_fixture_file("calc.py", CALC_PY);
    write_fixture_file("math.ts", MATH_TS);
    write_fixture_file("util.go", UTIL_GO);
    write_fixture_file("ops.c", OPS_C);
    write_fixture_file("drift.py", DRIFT_PY);
    write_fixture_file("runner.py", RUNNER_PY);

    g_project = cbm_project_name_from_path(g_tmpdir);
    if (!g_project)
        return -1;

    const char *cache_dir = cbm_resolve_cache_dir();
    int dbpath_length =
        cache_dir ? snprintf(g_dbpath, sizeof(g_dbpath), "%s/%s.db", cache_dir, g_project) : -1;
    if (dbpath_length <= 0 || (size_t)dbpath_length >= sizeof(g_dbpath) ||
        !cbm_mkdir_p(cache_dir, 0700)) {
        return -1;
    }
    unlink(g_dbpath);

    g_srv = cbm_mcp_server_new(NULL);
    if (!g_srv)
        return -1;

    char args[512];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", g_tmpdir);
    char *resp = cbm_mcp_handle_tool(g_srv, "index_repository", args);
    if (!resp)
        return -1;
    bool ok = strstr(resp, "indexed") != NULL;
    free(resp);
    return ok ? 0 : -1;
}

static void einteg_teardown(void) {
    if (g_srv) {
        cbm_mcp_server_free(g_srv);
        g_srv = NULL;
    }
    free(g_project);
    g_project = NULL;
    th_rmtree(g_tmpdir);
    unlink(g_dbpath);
    char wal[520], shm[520];
    snprintf(wal, sizeof(wal), "%s-wal", g_dbpath);
    snprintf(shm, sizeof(shm), "%s-shm", g_dbpath);
    unlink(wal);
    unlink(shm);
}

/* ── Helpers ────────────────────────────────────────────────────── */

/* Minimal JSON string escaping for embedding source into tool args. */
static char *json_escape(const char *s) {
    size_t cap = strlen(s) * 2 + 1;
    char *out = malloc(cap);
    if (!out)
        return NULL;
    size_t j = 0;
    for (const char *p = s; *p; p++) {
        char buf[8];
        const char *rep = NULL;
        switch (*p) {
        case '"':
            rep = "\\\"";
            break;
        case '\\':
            rep = "\\\\";
            break;
        case '\n':
            rep = "\\n";
            break;
        case '\t':
            rep = "\\t";
            break;
        case '\r':
            rep = "\\r";
            break;
        default:
            if ((unsigned char)*p < 0x20) {
                snprintf(buf, sizeof(buf), "\\u%04x", *p);
                rep = buf;
            }
            break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (j + rl + 1 > cap) {
                cap = (cap + rl) * 2;
                char *bigger = realloc(out, cap);
                if (!bigger) {
                    free(out);
                    return NULL;
                }
                out = bigger;
            }
            memcpy(out + j, rep, rl);
            j += rl;
        } else {
            if (j + 2 > cap) {
                cap *= 2;
                char *bigger = realloc(out, cap);
                if (!bigger) {
                    free(out);
                    return NULL;
                }
                out = bigger;
            }
            out[j++] = *p;
        }
    }
    out[j] = '\0';
    return out;
}

/* Call an edit tool with qn + extra JSON properties (already escaped). */
static char *call_edit_tool(const char *tool, const char *qn, const char *extra_json) {
    char *eqn = json_escape(qn);
    if (!eqn)
        return NULL;
    size_t cap = strlen(eqn) + strlen(extra_json) + strlen(g_project) + 128;
    char *args = malloc(cap);
    if (!args) {
        free(eqn);
        return NULL;
    }
    snprintf(args, cap, "{\"project\":\"%s\",\"qualified_name\":\"%s\"%s}", g_project, eqn,
             extra_json);
    free(eqn);
    char *resp = cbm_mcp_handle_tool(g_srv, tool, args);
    free(args);
    return resp;
}

/* Resolve a function's qualified_name + line range from the on-disk db.
 * Returns true when exactly the node was found; qn_out (malloc'd, caller
 * frees), start/end lines filled. */
static bool find_function(const char *name, char **qn_out, int *start_out, int *end_out) {
    cbm_store_t *store = cbm_store_open_path_existing(g_dbpath);
    if (!store)
        return false;
    cbm_node_t *funcs = NULL;
    int count = 0;
    bool found = false;
    if (cbm_store_find_nodes_by_label(store, g_project, "Function", &funcs, &count) ==
        CBM_STORE_OK) {
        for (int i = 0; i < count; i++) {
            if (funcs[i].name && strcmp(funcs[i].name, name) == 0 && funcs[i].qualified_name) {
                *qn_out = cbm_strdup(funcs[i].qualified_name);
                *start_out = funcs[i].start_line;
                *end_out = funcs[i].end_line;
                found = *qn_out != NULL;
                break;
            }
        }
        cbm_store_free_nodes(funcs, count);
    }
    cbm_store_close(store);
    return found;
}

static bool file_contains(const char *rel, const char *needle) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_tmpdir, rel);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

/* Read a fixture file into a malloc'd NUL-terminated string with CR bytes
 * stripped (caller frees; NULL on IO error). Fixtures are written in text
 * mode, so on Windows the disk bytes are CRLF while the C string constants
 * are LF — normalizing here keeps byte-exact assertions portable. */
static char *read_fixture_file(const char *rel) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_tmpdir, rel);
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    char *out = malloc(n + 1);
    if (!out)
        return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] != '\r') {
            out[j++] = buf[i];
        }
    }
    out[j] = '\0';
    return out;
}

static bool file_equals(const char *rel, const char *expected) {
    char *actual = read_fixture_file(rel);
    bool eq = actual && strcmp(actual, expected) == 0;
    free(actual);
    return eq;
}

/* ══════════════════════════════════════════════════════════════════
 *  TESTS — order matters: the suite shares one indexed fixture
 * ══════════════════════════════════════════════════════════════════ */

TEST(einteg_dry_run_writes_nothing) {
    /* Default (dry_run=true) returns a plan and touches neither disk nor graph. */
    char *qn = NULL;
    int start = 0, end = 0;
    ASSERT_TRUE(find_function("multiply", &qn, &start, &end));

    char *resp = call_edit_tool("edit_symbol", qn,
                                ",\"action\":\"replace_body\",\"content\":\"def multiply(a, b):\\n"
                                "    return a * b\\n\"");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "DRY-RUN") != NULL);
    free(resp);

    ASSERT_TRUE(file_contains("calc.py", "for _ in range(b):")); /* original intact */
    ASSERT_TRUE(!file_contains("calc.py", "return a * b"));      /* nothing written */
    free(qn);
    PASS();
}

TEST(einteg_replace_body_updates_graph) {
    /* Apply replace_body on multiply (Python): +1 line. After the call the
     * synchronous re-index must make the graph reflect the new ranges. */
    char *qn = NULL;
    int start = 0, end = 0;
    ASSERT_TRUE(find_function("multiply", &qn, &start, &end));

    const char *new_def = "def multiply(a, b):\n"
                          "    # repeat-add implementation\n"
                          "    result = 0\n"
                          "    for _ in range(b):\n"
                          "        result = add(result, a)\n"
                          "    return result\n";
    char *esc = json_escape(new_def);
    ASSERT_NOT_NULL(esc);
    size_t cap = strlen(esc) + 64;
    char *extra = malloc(cap);
    ASSERT_NOT_NULL(extra);
    snprintf(extra, cap, ",\"action\":\"replace_body\",\"content\":\"%s\",\"dry_run\":false", esc);
    free(esc);

    char *resp = call_edit_tool("edit_symbol", qn, extra);
    free(extra);
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "edit_symbol: APPLIED") != NULL);
    ASSERT_TRUE(strstr(resp, "reindex: incremental OK") != NULL);
    free(resp);

    /* Disk: new body present, old marker gone. */
    ASSERT_TRUE(file_contains("calc.py", "# repeat-add implementation"));

    /* Graph: multiply grew by exactly one line; main shifted accordingly. */
    char *qn2 = NULL;
    int start2 = 0, end2 = 0;
    ASSERT_TRUE(find_function("multiply", &qn2, &start2, &end2));
    ASSERT_EQ(start2, start);
    ASSERT_EQ(end2, end + 1);
    free(qn2);

    char *qn3 = NULL;
    int mstart = 0, mend = 0;
    ASSERT_TRUE(find_function("main", &qn3, &mstart, &mend));
    ASSERT_EQ(mstart, end2 + 2); /* blank line between defs preserved */
    free(qn3);
    free(qn);
    PASS();
}

TEST(einteg_undo_restores_file_and_graph) {
    /* Undo the replace from the previous test: backup restore must return
     * calc.py to its original bytes and re-index back to the old ranges. */
    char args[512];
    snprintf(args, sizeof(args), "{\"project\":\"%s\",\"path\":\"calc.py\",\"dry_run\":false}",
             g_project);
    char *resp = cbm_mcp_handle_tool(g_srv, "undo_edit", args);
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "undo_edit: APPLIED") != NULL);
    free(resp);

    ASSERT_TRUE(file_contains("calc.py", "for _ in range(b):"));
    ASSERT_TRUE(!file_contains("calc.py", "# repeat-add implementation"));

    char *qn = NULL;
    int start = 0, end = 0;
    ASSERT_TRUE(find_function("multiply", &qn, &start, &end));
    ASSERT_EQ(start, 4); /* original fixture layout */
    ASSERT_EQ(end, 8);
    free(qn);
    PASS();
}

TEST(einteg_insert_after_creates_node_go) {
    /* insert_after Compute (Go): a brand-new function must appear in the
     * graph after the in-call re-index. */
    char *qn = NULL;
    int start = 0, end = 0;
    ASSERT_TRUE(find_function("Compute", &qn, &start, &end));

    const char *block = "func Extra(x int) int {\n"
                        "\treturn x * 10\n"
                        "}\n";
    char *esc = json_escape(block);
    ASSERT_NOT_NULL(esc);
    size_t cap = strlen(esc) + 64;
    char *extra = malloc(cap);
    ASSERT_NOT_NULL(extra);
    snprintf(extra, cap, ",\"action\":\"insert_after\",\"content\":\"%s\",\"dry_run\":false", esc);
    free(esc);

    char *resp = call_edit_tool("edit_symbol", qn, extra);
    free(extra);
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "edit_symbol: APPLIED") != NULL);
    ASSERT_TRUE(strstr(resp, "reindex: incremental OK") != NULL);
    free(resp);

    ASSERT_TRUE(file_contains("util.go", "func Extra(x int) int {"));

    char *qn2 = NULL;
    int start2 = 0, end2 = 0;
    ASSERT_TRUE(find_function("Extra", &qn2, &start2, &end2));
    ASSERT_TRUE(start2 > end); /* inserted below Compute */
    free(qn2);
    free(qn);
    PASS();
}

TEST(einteg_delete_removes_node_c) {
    /* delete_symbol on a caller-less C function: allowed without force;
     * node must disappear from the graph and from disk. */
    char *qn = NULL;
    int start = 0, end = 0;
    ASSERT_TRUE(find_function("unused_helper", &qn, &start, &end));

    char *resp = call_edit_tool("delete_symbol", qn, ",\"dry_run\":false");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "delete_symbol: APPLIED") != NULL);
    ASSERT_TRUE(strstr(resp, "reindex: incremental OK") != NULL);
    free(resp);

    ASSERT_TRUE(!file_contains("ops.c", "unused_helper"));
    ASSERT_TRUE(file_contains("ops.c", "entry_point")); /* neighbour intact */

    char *qn2 = NULL;
    int start2 = 0, end2 = 0;
    ASSERT_TRUE(!find_function("unused_helper", &qn2, &start2, &end2));
    /* entry_point survived re-index */
    ASSERT_TRUE(find_function("entry_point", &qn2, &start2, &end2));
    free(qn2);
    free(qn);
    PASS();
}

TEST(einteg_rename_propagates_ts) {
    /* rename_symbol square → squared (TypeScript): definition + the call
     * inside doubleSquare must both be rewritten, and the fresh graph must
     * resolve the new name and forget the old one. force=true also applies
     * REVIEW-tier occurrences; tiering mechanics are covered by unit tests. */
    char *qn = NULL;
    int start = 0, end = 0;
    ASSERT_TRUE(find_function("square", &qn, &start, &end));

    size_t cap = strlen(qn) + 96;
    char *extra = malloc(cap);
    ASSERT_NOT_NULL(extra);
    snprintf(extra, cap, ",\"new_name\":\"squared\",\"dry_run\":false,\"force\":true");

    char *resp = call_edit_tool("rename_symbol", qn, extra);
    free(extra);
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "rename_symbol: APPLIED") != NULL);
    ASSERT_TRUE(strstr(resp, "reindex: incremental OK") != NULL);
    free(resp);

    /* Disk: both the definition and the caller updated. */
    ASSERT_TRUE(file_contains("math.ts", "export function squared(x: number): number {"));
    ASSERT_TRUE(file_contains("math.ts", "return 2 * squared(x);"));

    /* Graph: new name resolves, old name is gone. */
    char *qn2 = NULL;
    int start2 = 0, end2 = 0;
    ASSERT_TRUE(find_function("squared", &qn2, &start2, &end2));
    free(qn2);
    ASSERT_TRUE(!find_function("square", &qn2, &start2, &end2));
    free(qn);
    PASS();
}

TEST(einteg_drift_rejected_without_write) {
    /* External rewrite of drift.py (no re-index) → the index still resolves
     * stable_fn but the on-disk signature line changed: the drift gate must
     * reject the edit and write nothing. */
    char *qn = NULL;
    int start = 0, end = 0;
    ASSERT_TRUE(find_function("stable_fn", &qn, &start, &end));

    write_fixture_file("drift.py", "# externally rewritten\n"
                                   "def stable_fn_v2():\n"
                                   "    return 43\n");

    char *resp = call_edit_tool("edit_symbol", qn,
                                ",\"action\":\"replace_body\",\"content\":\"def stable_fn():\\n"
                                "    return 99\\n\",\"dry_run\":false");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "drifted") != NULL || strstr(resp, "stale") != NULL);
    free(resp);

    /* Nothing written: the external content is still there. */
    ASSERT_TRUE(file_contains("drift.py", "externally rewritten"));
    ASSERT_TRUE(!file_contains("drift.py", "return 99"));
    free(qn);
    PASS();
}

#if defined(CBM_EDIT_TEST_API) && CBM_EDIT_TEST_API
/* Fault-injection tests (THIET-KE-EDIT-TOOLS.md §8 "Concurrency"): the write
 * path must fail cleanly — no partial bytes, no lost originals — when a
 * write dies mid-operation, whether the edit touches one file or many. */

TEST(einteg_edit_write_failure_keeps_file) {
    /* Armed one-shot fault → edit_symbol's apply reports the write failure
     * and calc.py stays byte-identical to the original fixture. */
    char *qn = NULL;
    int start = 0, end = 0;
    ASSERT_TRUE(find_function("main", &qn, &start, &end));

    cbm_edit_write_test_reset_faults();
    cbm_edit_write_test_fail_once();
    char *resp = call_edit_tool("edit_symbol", qn,
                                ",\"action\":\"replace_body\",\"content\":\"def main():\\n"
                                "    print('changed')\\n\",\"dry_run\":false");
    cbm_edit_write_test_reset_faults();
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "nothing was written") != NULL);
    free(resp);

    ASSERT_TRUE(file_equals("calc.py", CALC_PY));
    free(qn);
    PASS();
}

TEST(einteg_rename_partial_failure_no_corruption) {
    /* Multi-file rename with the FIRST write killed by the one-shot fault:
     * exactly one of calc.py / runner.py is fully renamed, the other stays
     * byte-identical to its original — no file may be left in a mixed or
     * truncated state. Cleanup restores the written file from its backup so
     * the suite ends with a pristine fixture. */
    char *qn = NULL;
    int start = 0, end = 0;
    ASSERT_TRUE(find_function("add", &qn, &start, &end));

    cbm_edit_write_test_reset_faults();
    cbm_edit_write_test_fail_once();
    char *resp = call_edit_tool(
        "rename_symbol", qn, ",\"new_name\":\"plus\",\"dry_run\":false,\"force\":true");
    cbm_edit_write_test_reset_faults();
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "rename_symbol: APPLIED") != NULL);
    ASSERT_TRUE(strstr(resp, "WARNING: 1 file(s) failed") != NULL);
    free(resp);

    bool calc_original = file_equals("calc.py", CALC_PY);
    bool calc_renamed = file_equals("calc.py", CALC_PY_PLUS);
    bool runner_original = file_equals("runner.py", RUNNER_PY);
    bool runner_renamed = file_equals("runner.py", RUNNER_PY_PLUS);

    /* Each file is in EXACTLY one of the two whole states — never a mix. */
    ASSERT_TRUE(calc_original != calc_renamed);
    ASSERT_TRUE(runner_original != runner_renamed);
    /* And exactly one file moved (the failed write kept its target intact). */
    ASSERT_TRUE(calc_renamed != runner_renamed);

    /* Cleanup: undo the file that was written, restoring the fixture. */
    const char *written_rel = calc_renamed ? "calc.py" : "runner.py";
    char args[512];
    snprintf(args, sizeof(args), "{\"project\":\"%s\",\"path\":\"%s\",\"dry_run\":false}",
             g_project, written_rel);
    char *undo = cbm_mcp_handle_tool(g_srv, "undo_edit", args);
    ASSERT_NOT_NULL(undo);
    ASSERT_TRUE(strstr(undo, "undo_edit: APPLIED") != NULL);
    free(undo);

    ASSERT_TRUE(file_equals("calc.py", CALC_PY));
    ASSERT_TRUE(file_equals("runner.py", RUNNER_PY));
    free(qn);
    PASS();
}
#endif /* CBM_EDIT_TEST_API */

/* ══════════════════════════════════════════════════════════════════
 *  SUITE
 * ══════════════════════════════════════════════════════════════════ */

SUITE(edit_integration) {
    if (einteg_setup() != 0) {
        printf("  %sFAIL%s %s:%d: %s\n", tf_red(), tf_reset(), __FILE__, __LINE__,
               "edit_integration setup failed");
        tf_fail_count++;
        einteg_teardown();
        return;
    }

    RUN_TEST(einteg_dry_run_writes_nothing);
    RUN_TEST(einteg_replace_body_updates_graph);
    RUN_TEST(einteg_undo_restores_file_and_graph);
    RUN_TEST(einteg_insert_after_creates_node_go);
    RUN_TEST(einteg_delete_removes_node_c);
    RUN_TEST(einteg_rename_propagates_ts);
    RUN_TEST(einteg_drift_rejected_without_write);
#if defined(CBM_EDIT_TEST_API) && CBM_EDIT_TEST_API
    RUN_TEST(einteg_edit_write_failure_keeps_file);
    RUN_TEST(einteg_rename_partial_failure_no_corruption);
#endif

    einteg_teardown();
}
