/*
 * test_mcp_project_scope.c — Tests for --scope session pinning.
 *
 * A scoped MCP session (--scope=<repo>, written by `install --project`)
 * serves exactly one repository: every project-qualifying tool argument must
 * name the derived session project (or be omitted), and list_projects must
 * not even reveal that other projects exist. These tests pin an in-process
 * server and drive the real tool dispatch path.
 */
#include "test_framework.h"
#include "foundation/compat.h" /* cbm_setenv / cbm_unsetenv / cbm_mkdtemp */
#include "mcp/mcp.h"
#include "store/store.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Scoped fixture: an isolated MFA_CACHE_DIR plus a server whose session
 * context is pinned to a fake repository root. Restores the environment on
 * teardown. */
typedef struct {
    char dir[512];
    char *old_cache; /* NULL when MFA_CACHE_DIR was unset */
    cbm_mcp_server_t *srv;
    char root[512];
} scope_fixture_t;

static bool scope_fixture_init(scope_fixture_t *fx, const char *repo_name) {
    memset(fx, 0, sizeof(*fx));
    snprintf(fx->dir, sizeof(fx->dir), "%s/cbm-scope-test-XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(fx->dir)) {
        return false;
    }
    snprintf(fx->root, sizeof(fx->root), "%s/%s", fx->dir, repo_name);
    if (cbm_mkdir(fx->root) != 0) {
        return false;
    }
    fx->old_cache = getenv("MFA_CACHE_DIR") ? strdup(getenv("MFA_CACHE_DIR")) : NULL;
    if (cbm_setenv("MFA_CACHE_DIR", fx->dir, 1) != 0) {
        return false;
    }
    fx->srv = cbm_mcp_server_new(NULL);
    return fx->srv && cbm_mcp_server_set_session_context(fx->srv, fx->root, fx->root);
}

static void scope_fixture_free(scope_fixture_t *fx) {
    if (fx->srv) {
        cbm_mcp_server_free(fx->srv);
    }
    if (fx->old_cache) {
        (void)cbm_setenv("MFA_CACHE_DIR", fx->old_cache, 1);
        free(fx->old_cache);
    } else {
        (void)cbm_unsetenv("MFA_CACHE_DIR");
    }
}

/* list_projects resolves a project by its INTERNAL project row (#704 skips
 * ghost/empty DBs), so seed a real store the way indexing does: open
 * <cache>/<project>.db and upsert the projects row. */
static bool scope_touch_db(scope_fixture_t *fx, const char *project) {
    cbm_store_t *st = cbm_store_open(project);
    if (!st) {
        return false;
    }
    int rc = cbm_store_upsert_project(st, project, fx->root);
    cbm_store_close(st);
    return rc == CBM_STORE_OK;
}

/* ── Pin API ──────────────────────────────────────────────────── */

TEST(scope_pin_requires_session_context) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    ASSERT_FALSE(cbm_mcp_server_pin_session_scope(srv, true));
    cbm_mcp_server_free(srv);
    PASS();
}

/* ── Guard semantics through the real dispatch path ───────────── */

TEST(scope_guard_refuses_foreign_project) {
    scope_fixture_t fx;
    ASSERT_TRUE(scope_fixture_init(&fx, "scoped-repo"));
    ASSERT_TRUE(cbm_mcp_server_pin_session_scope(fx.srv, true));

    char *result = cbm_mcp_handle_tool(fx.srv, "search_graph",
                                       "{\"project\":\"some-other-project\",\"name_pattern\":\"x\"}");
    ASSERT_NOT_NULL(result);
    ASSERT_NOT_NULL(strstr(result, "session is scoped"));
    ASSERT_NOT_NULL(strstr(result, "some-other-project"));
    free(result);

    /* compare_graphs names two projects; both sides are checked. */
    result = cbm_mcp_handle_tool(
        fx.srv, "compare_graphs",
        "{\"base_project\":\"scoped-repo\",\"target_project\":\"some-other-project\"}");
    ASSERT_NOT_NULL(result);
    ASSERT_NOT_NULL(strstr(result, "session is scoped"));
    free(result);

    scope_fixture_free(&fx);
    PASS();
}

TEST(scope_guard_accepts_session_project_name) {
    scope_fixture_t fx;
    ASSERT_TRUE(scope_fixture_init(&fx, "scoped-repo"));
    const char *derived = cbm_mcp_server_session_project(fx.srv);
    ASSERT_NOT_NULL(derived);
    ASSERT_TRUE(cbm_mcp_server_pin_session_scope(fx.srv, true));

    /* The exact derived name passes the guard (the tool then reports its own
     * not-indexed state, which proves the guard let it through). */
    char args[768];
    snprintf(args, sizeof(args), "{\"project\":\"%s\",\"name_pattern\":\"x\"}", derived);
    char *result = cbm_mcp_handle_tool(fx.srv, "search_graph", args);
    ASSERT_NOT_NULL(result);
    ASSERT_NULL(strstr(result, "session is scoped"));
    free(result);

    scope_fixture_free(&fx);
    PASS();
}

TEST(scope_guard_accepts_folder_alias_when_unambiguous) {
    scope_fixture_t fx;
    ASSERT_TRUE(scope_fixture_init(&fx, "scoped-repo"));
    const char *derived = cbm_mcp_server_session_project(fx.srv);
    ASSERT_NOT_NULL(derived);
    ASSERT_TRUE(scope_touch_db(&fx, derived)); /* the #1025 tail-match target */
    ASSERT_TRUE(cbm_mcp_server_pin_session_scope(fx.srv, true));

    /* The natural repo FOLDER name adopts the full derived name (#1025) and
     * stays inside the scope. */
    char *result = cbm_mcp_handle_tool(fx.srv, "search_graph",
                                       "{\"project\":\"scoped-repo\",\"name_pattern\":\"x\"}");
    ASSERT_NOT_NULL(result);
    ASSERT_NULL(strstr(result, "session is scoped"));
    free(result);

    scope_fixture_free(&fx);
    PASS();
}

TEST(scope_guard_allows_omitted_project) {
    scope_fixture_t fx;
    ASSERT_TRUE(scope_fixture_init(&fx, "scoped-repo"));
    ASSERT_TRUE(cbm_mcp_server_pin_session_scope(fx.srv, true));

    /* No project key at all: the guard must not fire; the tool's own
     * missing-argument validation answers instead. */
    char *result = cbm_mcp_handle_tool(fx.srv, "search_graph", "{\"name_pattern\":\"x\"}");
    ASSERT_NOT_NULL(result);
    ASSERT_NULL(strstr(result, "session is scoped"));
    free(result);

    scope_fixture_free(&fx);
    PASS();
}

TEST(scope_guard_inactive_without_pin) {
    scope_fixture_t fx;
    ASSERT_TRUE(scope_fixture_init(&fx, "scoped-repo"));

    /* Session context alone (no pin) keeps the historical behaviour: another
     * project's name reaches the tool and fails with the ordinary
     * not-found/not-indexed error, never a scope refusal. */
    char *result = cbm_mcp_handle_tool(fx.srv, "search_graph",
                                       "{\"project\":\"some-other-project\",\"name_pattern\":\"x\"}");
    ASSERT_NOT_NULL(result);
    ASSERT_NULL(strstr(result, "session is scoped"));
    free(result);

    scope_fixture_free(&fx);
    PASS();
}

/* ── list_projects hides other projects while pinned ──────────── */

TEST(scope_list_projects_reports_only_the_pinned_project) {
    scope_fixture_t fx;
    ASSERT_TRUE(scope_fixture_init(&fx, "scoped-repo"));
    const char *derived = cbm_mcp_server_session_project(fx.srv);
    ASSERT_NOT_NULL(derived);
    ASSERT_TRUE(scope_touch_db(&fx, "some-other-project"));
    ASSERT_TRUE(scope_touch_db(&fx, derived));
    ASSERT_TRUE(cbm_mcp_server_pin_session_scope(fx.srv, true));

    char *result = cbm_mcp_handle_tool(fx.srv, "list_projects", "{}");
    ASSERT_NOT_NULL(result);
    ASSERT_NULL(strstr(result, "some-other-project"));
    ASSERT_NOT_NULL(strstr(result, derived));
    ASSERT_NOT_NULL(strstr(result, "\"total\":1"));
    free(result);

    scope_fixture_free(&fx);
    PASS();
}

TEST(scope_list_projects_unpinned_reports_everything) {
    scope_fixture_t fx;
    ASSERT_TRUE(scope_fixture_init(&fx, "scoped-repo"));
    const char *derived = cbm_mcp_server_session_project(fx.srv);
    ASSERT_NOT_NULL(derived);
    ASSERT_TRUE(scope_touch_db(&fx, "some-other-project"));
    ASSERT_TRUE(scope_touch_db(&fx, derived));

    char *result = cbm_mcp_handle_tool(fx.srv, "list_projects", "{}");
    ASSERT_NOT_NULL(result);
    ASSERT_NOT_NULL(strstr(result, "some-other-project"));
    ASSERT_NOT_NULL(strstr(result, "\"total\":2"));
    free(result);

    scope_fixture_free(&fx);
    PASS();
}

SUITE(mcp_project_scope) {
    RUN_TEST(scope_pin_requires_session_context);
    RUN_TEST(scope_guard_refuses_foreign_project);
    RUN_TEST(scope_guard_accepts_session_project_name);
    RUN_TEST(scope_guard_accepts_folder_alias_when_unambiguous);
    RUN_TEST(scope_guard_allows_omitted_project);
    RUN_TEST(scope_guard_inactive_without_pin);
    RUN_TEST(scope_list_projects_reports_only_the_pinned_project);
    RUN_TEST(scope_list_projects_unpinned_reports_everything);
}
