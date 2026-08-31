/*
 * test_store_pragmas.c — Tests for SQLite pragma resolution.
 *
 * Validates that the CBM_SQLITE_MMAP_SIZE env var controls the mmap_size
 * pragma applied to on-disk stores. Default behavior (env unset) must
 * remain 64 MB. Setting the env to 0 disables memory-mapped I/O so
 * concurrent processes that truncate the DB file under a sibling's live
 * mapping return SQLITE_IOERR instead of crashing the process with SIGBUS.
 */
#include "../src/foundation/compat.h"
#include "../src/foundation/compat_thread.h"
#include "test_framework.h"
#include "test_helpers.h"
#include <store/store.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void clear_mmap_env(void) {
    cbm_unsetenv("CBM_SQLITE_MMAP_SIZE");
}

TEST(mmap_size_default_when_unset) {
    clear_mmap_env();
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 67108864LL);
    PASS();
}

TEST(mmap_size_zero_disables_mmap) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "0", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 0LL);
    clear_mmap_env();
    PASS();
}

TEST(mmap_size_explicit_value) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "1048576", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 1048576LL);
    clear_mmap_env();
    PASS();
}

TEST(mmap_size_negative_clamped_to_zero) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "-1", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 0LL);
    clear_mmap_env();
    PASS();
}

TEST(mmap_size_garbage_falls_back_to_default) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "not-a-number", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 67108864LL);
    clear_mmap_env();
    PASS();
}

TEST(mmap_size_partial_garbage_falls_back_to_default) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "123abc", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 67108864LL);
    clear_mmap_env();
    PASS();
}

/* Integration smoke: opening a file-backed store with mmap_size=0 must
 * succeed. Proves the resolver is wired through configure_pragmas(). */
TEST(store_open_with_mmap_disabled) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "0", 1);
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/cbm_test_pragmas_%d.db", cbm_tmpdir(), (int)getpid());
    unlink(tmp_path);

    cbm_store_t *s = cbm_store_open_path(tmp_path);
    ASSERT(s != NULL);
    cbm_store_close(s);

    unlink(tmp_path);
    /* WAL/SHM siblings created by the open */
    char tmp_wal[300];
    char tmp_shm[300];
    snprintf(tmp_wal, sizeof(tmp_wal), "%s-wal", tmp_path);
    snprintf(tmp_shm, sizeof(tmp_shm), "%s-shm", tmp_path);
    unlink(tmp_wal);
    unlink(tmp_shm);

    clear_mmap_env();
    PASS();
}

TEST(runtime_operational_hardening_quota_metrics_rebuild) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_upsert_project(s, "runtime-ops", "/tmp/runtime-ops"), CBM_STORE_OK);

    cbm_runtime_quota_t quota = {1, 1, 1};
    ASSERT_EQ(cbm_store_set_runtime_quota(s, &quota), CBM_STORE_OK);
    cbm_runtime_quota_t read_quota = {0};
    ASSERT_EQ(cbm_store_get_runtime_quota(s, &read_quota), CBM_STORE_OK);
    ASSERT_EQ(read_quota.max_legacy_batches, 1);
    ASSERT_EQ(read_quota.max_canonical_contributions, 1);
    ASSERT_EQ(read_quota.max_canonical_spans, 1);

    cbm_runtime_trace_edge_t edge = {.caller = "a",
                                     .callee = "b",
                                     .call_count = 2,
                                     .duration_ns_total = 10,
                                     .duration_ns_max = 5,
                                     .observations = 1};
    bool idempotent = false;
    int64_t observations = 0;
    ASSERT_EQ(cbm_store_ingest_runtime_traces(s, "runtime-ops", "batch-1", "hash-1", &edge, 1,
                                              &idempotent, &observations),
              CBM_STORE_OK);
    ASSERT_FALSE(idempotent);
    ASSERT_EQ(observations, 1);

    cbm_runtime_metrics_t metrics = {0};
    ASSERT_EQ(cbm_store_get_runtime_metrics(s, "runtime-ops", &metrics), CBM_STORE_OK);
    ASSERT_EQ(metrics.legacy_batches, 1);
    ASSERT_EQ(metrics.canonical_contributions, 0);
    ASSERT_EQ(metrics.runtime_edges, 1);
    ASSERT_EQ(metrics.runtime_publications, 1);
    ASSERT_EQ(metrics.runtime_endpoints, 2);
    ASSERT_EQ(metrics.quota.max_canonical_spans, 1);

    ASSERT_EQ(cbm_store_ingest_runtime_traces(s, "runtime-ops", "batch-2", "hash-2", &edge, 1,
                                              &idempotent, &observations),
              CBM_STORE_QUOTA_EXCEEDED);
    ASSERT_EQ(cbm_store_get_runtime_metrics(s, "runtime-ops", &metrics), CBM_STORE_OK);
    ASSERT_EQ(metrics.legacy_batches, 1);

    /* The live aggregate is mutable derived state. Recovery copies the latest
     * immutable publication and never rewrites its historical snapshot. */
    ASSERT_EQ(sqlite3_exec(cbm_store_get_db(s),
                           "DELETE FROM runtime_trace_edges WHERE project='runtime-ops';", NULL,
                           NULL, NULL),
              SQLITE_OK);
    ASSERT_EQ(cbm_store_rebuild_runtime_aggregates(s, "runtime-ops"), CBM_STORE_OK);
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(cbm_store_get_db(s),
                                 "SELECT call_count, duration_ns_total FROM runtime_trace_edges "
                                 "WHERE project='runtime-ops' AND caller='a' AND callee='b';",
                                 -1, &stmt, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int64(stmt, 0), 2);
    ASSERT_EQ(sqlite3_column_int64(stmt, 1), 10);
    sqlite3_finalize(stmt);
    cbm_store_close(s);
    PASS();
}

TEST(runtime_canonical_span_histograms_are_per_span) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_upsert_project(s, "runtime-hist", "/tmp/runtime-hist"), CBM_STORE_OK);
    cbm_runtime_trace_span_t spans[2] = {
        {.producer_id = "producer",
         .producer_epoch = "epoch",
         .trace_id = "trace-1",
         .span_id = "span-1",
         .normalized_hash = "payload-1",
         .caller = "a",
         .callee = "b",
         .call_count = 1,
         .duration_ns_total = 1,
         .duration_ns_max = 1},
        {.producer_id = "producer",
         .producer_epoch = "epoch",
         .trace_id = "trace-2",
         .span_id = "span-2",
         .normalized_hash = "payload-2",
         .caller = "a",
         .callee = "b",
         .call_count = 1,
         .duration_ns_total = 1000,
         .duration_ns_max = 1000},
    };
    bool idempotent = false;
    int64_t observations = 0;
    int new_spans = 0;
    ASSERT_EQ(cbm_store_ingest_runtime_trace_contribution(
                  s, "runtime-hist", "producer", "epoch", "contribution", "hash", spans, 2,
                  &idempotent, &observations, &new_spans),
              CBM_STORE_OK);
    ASSERT_EQ(new_spans, 2);

    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(
                  cbm_store_get_db(s),
                  "SELECT duration_histogram FROM runtime_trace_spans "
                  "WHERE project='runtime-hist' ORDER BY span_id;",
                  -1, &stmt, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    const char *first = (const char *)sqlite3_column_text(stmt, 0);
    ASSERT_NOT_NULL(first);
    ASSERT_TRUE(strcmp(first, "1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0") == 0);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    const char *second = (const char *)sqlite3_column_text(stmt, 0);
    ASSERT_NOT_NULL(second);
    ASSERT_TRUE(strcmp(second, "0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0") == 0);
    sqlite3_finalize(stmt);
    cbm_store_close(s);
    PASS();
}

typedef struct {
    cbm_store_t *store;
    int index;
    atomic_int *ready;
    atomic_int *go;
    int result;
} runtime_producer_worker_t;

static void *runtime_producer_worker(void *arg) {
    runtime_producer_worker_t *worker = (runtime_producer_worker_t *)arg;
    atomic_fetch_add(worker->ready, 1);
    while (atomic_load(worker->go) == 0) {
        cbm_usleep(1000);
    }
    char producer[32];
    char contribution[32];
    char trace[32];
    char span_id[32];
    char payload[32];
    snprintf(producer, sizeof(producer), "producer-%d", worker->index);
    snprintf(contribution, sizeof(contribution), "contribution-%d", worker->index);
    snprintf(trace, sizeof(trace), "trace-%d", worker->index);
    snprintf(span_id, sizeof(span_id), "span-%d", worker->index);
    snprintf(payload, sizeof(payload), "payload-%d", worker->index);
    cbm_runtime_trace_span_t span = {.producer_id = producer,
                                     .producer_epoch = "epoch-1",
                                     .trace_id = trace,
                                     .span_id = span_id,
                                     .normalized_hash = payload,
                                     .caller = "shared-caller",
                                     .callee = "shared-callee",
                                     .call_count = 1,
                                     .duration_ns_total = 10,
                                     .duration_ns_max = 10};
    bool idempotent = false;
    int64_t observations = 0;
    int new_spans = 0;
    worker->result = cbm_store_ingest_runtime_trace_contribution(
        worker->store, "runtime-concurrent", producer, "epoch-1", contribution, payload, &span, 1,
        &idempotent, &observations, &new_spans);
    return NULL;
}

TEST(runtime_multi_producer_handles_are_serializable) {
    enum { WORKERS = 8 };
    char *td = th_mktempdir("cbm_runtime_concurrent");
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/runtime.db", td);
    cbm_store_t *seed = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(seed);
    ASSERT_EQ(cbm_store_upsert_project(seed, "runtime-concurrent", "/tmp/runtime-concurrent"),
              CBM_STORE_OK);
    cbm_store_close(seed);

    cbm_store_t *stores[WORKERS] = {0};
    runtime_producer_worker_t workers[WORKERS] = {0};
    cbm_thread_t threads[WORKERS];
    atomic_int ready;
    atomic_int go;
    atomic_init(&ready, 0);
    atomic_init(&go, 0);
    bool opened = true;
    for (int i = 0; i < WORKERS; i++) {
        stores[i] = cbm_store_open_path_existing(db_path);
        opened = opened && stores[i] != NULL;
        workers[i].store = stores[i];
        workers[i].index = i;
        workers[i].ready = &ready;
        workers[i].go = &go;
        workers[i].result = CBM_STORE_ERR;
    }
    ASSERT_TRUE(opened);
    bool started[WORKERS] = {false};
    for (int i = 0; i < WORKERS; i++) {
        started[i] = cbm_thread_create(&threads[i], 0, runtime_producer_worker, &workers[i]) == 0;
    }
    atomic_store(&go, 1);
    for (int i = 0; i < WORKERS; i++) {
        if (started[i]) {
            ASSERT_EQ(cbm_thread_join(&threads[i]), 0);
        }
    }
    for (int i = 0; i < WORKERS; i++) {
        ASSERT_EQ(workers[i].result, CBM_STORE_OK);
        cbm_store_close(stores[i]);
    }

    cbm_store_t *check = cbm_store_open_path_query(db_path);
    ASSERT_NOT_NULL(check);
    cbm_runtime_metrics_t metrics = {0};
    ASSERT_EQ(cbm_store_get_runtime_metrics(check, "runtime-concurrent", &metrics), CBM_STORE_OK);
    ASSERT_EQ(metrics.canonical_contributions, WORKERS);
    ASSERT_EQ(metrics.canonical_spans, WORKERS);
    ASSERT_EQ(metrics.runtime_edges, 1);
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(
                  cbm_store_get_db(check),
                  "SELECT call_count, observations FROM runtime_trace_edges "
                  "WHERE project='runtime-concurrent';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), WORKERS);
    ASSERT_EQ(sqlite3_column_int(stmt, 1), WORKERS);
    sqlite3_finalize(stmt);
    cbm_store_close(check);
    th_rmtree(td);
    PASS();
}

/* #1083: on-disk write connections must bound the WAL via journal_size_limit
 * so a checkpoint-starved log is physically reclaimed once a checkpoint can
 * reset it. On main this is UNSET (-1 = unlimited), so the -wal file only ever
 * grows (all our checkpoints are PASSIVE and never ftruncate). Read the pragma
 * back on the SAME connection — it's per-connection and not persisted. */
TEST(journal_size_limit_bounds_wal_issue1083) {
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/cbm_test_jsl_%d.db", cbm_tmpdir(), (int)getpid());
    unlink(tmp_path);

    cbm_store_t *s = cbm_store_open_path(tmp_path);
    ASSERT(s != NULL);
    /* 256 MiB — far above the healthy WAL (~4 MiB), so no truncate/regrow churn
     * in normal operation; it only fires after abnormal (starved) growth. */
    ASSERT(cbm_store_journal_size_limit(s) == (int64_t)268435456);
    cbm_store_close(s);

    unlink(tmp_path);
    char tmp_wal[300];
    char tmp_shm[300];
    snprintf(tmp_wal, sizeof(tmp_wal), "%s-wal", tmp_path);
    snprintf(tmp_shm, sizeof(tmp_shm), "%s-shm", tmp_path);
    unlink(tmp_wal);
    unlink(tmp_shm);
    PASS();
}

/* Pagination-cursor generation: minted per DB file, bumped per index run.
 * Same store + reads only -> stable; upsert_project (every index run's choke
 * point) -> changes; two distinct DB files can never share a generation even
 * at the same counter value (random db_uid). */
TEST(store_generation_tracks_mutations) {
    char g1[128];
    char g2[128];
    char g3[128];
    cbm_store_t *a = cbm_store_open_memory();
    ASSERT(a != NULL);
    ASSERT_EQ(cbm_store_upsert_project(a, "p", "/tmp/p"), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_generation(a, g1, sizeof(g1)), CBM_STORE_OK);
    ASSERT(strncmp(g1, "u", 1) == 0); /* seeded, not legacy */
    ASSERT_EQ(cbm_store_generation(a, g2, sizeof(g2)), CBM_STORE_OK);
    ASSERT(strcmp(g1, g2) == 0); /* reads are stable */
    ASSERT_EQ(cbm_store_upsert_project(a, "p", "/tmp/p"), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_generation(a, g3, sizeof(g3)), CBM_STORE_OK);
    ASSERT(strcmp(g1, g3) != 0); /* index run bumps */

    cbm_store_t *b = cbm_store_open_memory();
    ASSERT(b != NULL);
    ASSERT_EQ(cbm_store_upsert_project(b, "p", "/tmp/p"), CBM_STORE_OK);
    char gb[128];
    ASSERT_EQ(cbm_store_generation(b, gb, sizeof(gb)), CBM_STORE_OK);
    ASSERT(strcmp(g1, gb) != 0); /* distinct DBs never alias (random uid) */
    cbm_store_close(a);
    cbm_store_close(b);
    PASS();
}

/* #896: a row-scan that dies mid-stream (SQLITE_CORRUPT) must surface a
 * loud store error, not masquerade as a clean end of results. Counts are
 * answered from covering indexes (still correct) while row fetches die at
 * the first corrupt table page — the old loops discarded the terminal
 * sqlite3_step code, so every query surface returned plausible
 * truncated/empty answers with no error. */
TEST(corrupt_page_scan_returns_error_not_truncation) {
    enum { CORRUPT_NODES = 2000, ZERO_PAGES = 40 };
    char *td = th_mktempdir("cbm_corrupt");
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/c.db", td);

    cbm_store_t *s = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(s);
    cbm_store_upsert_project(s, "corr", "/tmp/corr");
    for (int i = 0; i < CORRUPT_NODES; i++) {
        char name[64];
        char qn[256];
        snprintf(name, sizeof(name), "corrupt_probe_fn_%04d", i);
        snprintf(qn, sizeof(qn),
                 "corr.some.rather.long.module.path.to.fill.table.pages.%s_padding_padding", name);
        cbm_node_t n = {.project = "corr",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "src/corrupt_probe.py",
                        .start_line = i + 1,
                        .end_line = i + 2};
        ASSERT_TRUE(cbm_store_upsert_node(s, &n) > 0);
    }
    /* Precondition: a full scan works on the healthy file. */
    cbm_search_params_t params = {.project = "corr", .label = "Function", .limit = 50};
    cbm_search_output_t out = {0};
    ASSERT_EQ(cbm_store_search(s, &params, &out), CBM_STORE_OK);
    ASSERT_EQ(out.total, CORRUPT_NODES);
    cbm_store_search_free(&out);
    cbm_store_close(s);

    /* Zero a band of mid-file pages (the report's dd repro): page 25%..
     * covers nodes-table leaves on a file this shape. */
    FILE *f = fopen(db_path, "rb+");
    ASSERT_NOT_NULL(f);
    (void)fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    enum { PAGE = 4096 };
    long page_count = fsize / PAGE;
    ASSERT_TRUE(page_count > ZERO_PAGES + 8);
    char zero[PAGE];
    memset(zero, 0, sizeof(zero));
    (void)fseek(f, (page_count / 4) * (long)PAGE, SEEK_SET);
    for (int i = 0; i < ZERO_PAGES; i++) {
        ASSERT_EQ(fwrite(zero, 1, PAGE, f), (size_t)PAGE);
    }
    (void)fclose(f);

    /* The scans must now fail LOUDLY (CBM_STORE_ERR), not truncate. */
    cbm_store_t *s2 = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(s2);
    /* The scan must CROSS the corrupt band: request every row. */
    cbm_search_params_t all_params = {
        .project = "corr", .label = "Function", .limit = CORRUPT_NODES};
    cbm_search_output_t out2 = {0};
    int rc_search = cbm_store_search(s2, &all_params, &out2);
    if (rc_search == CBM_STORE_OK && out2.count == CORRUPT_NODES) {
        /* Vacuous-guard: a complete, healthy scan means corruption missed
         * the table pages — rebuild the fixture, don't relax the assert. */
        FAIL("fixture failed to hit table pages (full scan healthy)");
    }
    /* THE BUG (#896): OK + silently truncated rows. Fixed = loud ERR. */
    ASSERT_EQ(rc_search, CBM_STORE_ERR);
    cbm_store_search_free(&out2);

    /* Point lookups may legitimately succeed when their row's page
     * escaped the corrupt band — the class contract is about SCANS. A
     * second scan surface (qn-suffix, different SQL path) must also err. */
    cbm_node_t *hits = NULL;
    int hit_count = 0;
    int rc_suffix =
        cbm_store_find_nodes_by_qn_suffix(s2, "corr", "padding_padding", &hits, &hit_count);
    if (rc_suffix == CBM_STORE_OK && hit_count == CORRUPT_NODES) {
        FAIL("suffix scan healthy — fixture failed to hit table pages");
    }
    ASSERT_EQ(rc_suffix, CBM_STORE_ERR);
    cbm_store_free_nodes(hits, hit_count);
    cbm_store_close(s2);

    unlink(db_path);
    PASS();
}

SUITE(store_pragmas) {
    RUN_TEST(journal_size_limit_bounds_wal_issue1083);
    RUN_TEST(store_generation_tracks_mutations);
    RUN_TEST(corrupt_page_scan_returns_error_not_truncation);
    RUN_TEST(mmap_size_default_when_unset);
    RUN_TEST(mmap_size_zero_disables_mmap);
    RUN_TEST(mmap_size_explicit_value);
    RUN_TEST(mmap_size_negative_clamped_to_zero);
    RUN_TEST(mmap_size_garbage_falls_back_to_default);
    RUN_TEST(mmap_size_partial_garbage_falls_back_to_default);
    RUN_TEST(store_open_with_mmap_disabled);
    RUN_TEST(runtime_operational_hardening_quota_metrics_rebuild);
    RUN_TEST(runtime_canonical_span_histograms_are_per_span);
    RUN_TEST(runtime_multi_producer_handles_are_serializable);
}
