/*
 * test_edit.c — Unit tests for src/edit (edit_symbol / delete_symbol /
 * rename_symbol primitives).
 *
 * The buffer-level cases mirror build/edit_surgery_logic_test.py 1:1 (same
 * fixtures, same expected bytes) so the C implementation is checked against
 * the exact logic that the Python port validated. File-level cases exercise
 * cbm_edit_file_stat / cbm_edit_read_file / cbm_edit_write_atomic against a
 * real temp directory.
 */
#include "test_framework.h"
#include "test_helpers.h"
#include "../src/edit/edit.h"

#include <stdlib.h>
#include <string.h>

/* ── helpers ────────────────────────────────────────────────────── */

/* Apply surgery and assert the exact resulting bytes. Frees the buffer.
 * Returns 0 on success; on failure the detailed file:line was already printed
 * and the caller's wrapping assertion aborts the test. */
static int expect_surgery(const char *old_data, int start, int end, cbm_edit_action_t action,
                          const char *content, const char *expected) {
    char *out = NULL;
    size_t out_len = 0;
    cbm_edit_surgery_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    int rc = cbm_edit_surgery_apply(old_data, strlen(old_data), start, end, action, content,
                                    strlen(content), &out, &out_len, &stats);
    ASSERT_EQ(rc, CBM_EDIT_OK);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ((long long)out_len, (long long)strlen(expected));
    ASSERT_MEM_EQ(out, expected, out_len);
    free(out);
    return 0;
}

static int expect_delete(const char *old_data, int start, int end, const char *expected) {
    char *out = NULL;
    size_t out_len = 0;
    cbm_edit_surgery_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    int rc =
        cbm_edit_surgery_delete(old_data, strlen(old_data), start, end, &out, &out_len, &stats);
    ASSERT_EQ(rc, CBM_EDIT_OK);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ((long long)out_len, (long long)strlen(expected));
    ASSERT_MEM_EQ(out, expected, out_len);
    free(out);
    return 0;
}

/* Scan `name` in `data` with a generous cap. NULL on error; caller asserts. */
static cbm_edit_occurrence_t *scan_all(const char *data, const char *name, int *count) {
    cbm_edit_occurrence_t *occs = NULL;
    *count = 0;
    if (cbm_edit_scan_identifier(data, strlen(data), name, &occs, count, 4096) != CBM_EDIT_OK) {
        return NULL;
    }
    return occs;
}

/* Call-site wrappers: abort the test when the helper reported a failure. */
#define EXPECT_SURGERY(...) ASSERT_EQ(expect_surgery(__VA_ARGS__), 0)
#define EXPECT_DELETE(...) ASSERT_EQ(expect_delete(__VA_ARGS__), 0)
#define EXPECT_RENAME(...) ASSERT_EQ(expect_rename(__VA_ARGS__), 0)

/* ── actions & labels ───────────────────────────────────────────── */

TEST(action_parse_roundtrip) {
    cbm_edit_action_t a;
    ASSERT_TRUE(cbm_edit_action_parse("replace_body", &a));
    ASSERT_EQ(a, CBM_EDIT_REPLACE_BODY);
    ASSERT_TRUE(cbm_edit_action_parse("insert_before", &a));
    ASSERT_EQ(a, CBM_EDIT_INSERT_BEFORE);
    ASSERT_TRUE(cbm_edit_action_parse("insert_after", &a));
    ASSERT_EQ(a, CBM_EDIT_INSERT_AFTER);
    ASSERT_FALSE(cbm_edit_action_parse("delete", &a));
    ASSERT_FALSE(cbm_edit_action_parse("", &a));
    ASSERT_STR_EQ(cbm_edit_action_name(CBM_EDIT_REPLACE_BODY), "replace_body");
    ASSERT_STR_EQ(cbm_edit_action_name(CBM_EDIT_INSERT_BEFORE), "insert_before");
    ASSERT_STR_EQ(cbm_edit_action_name(CBM_EDIT_INSERT_AFTER), "insert_after");
    PASS();
}

TEST(label_editable) {
    ASSERT_TRUE(cbm_edit_label_editable("Function"));
    ASSERT_TRUE(cbm_edit_label_editable("Method"));
    ASSERT_TRUE(cbm_edit_label_editable("Class"));
    ASSERT_TRUE(cbm_edit_label_editable("Struct"));
    ASSERT_TRUE(cbm_edit_label_editable("Variable"));
    ASSERT_FALSE(cbm_edit_label_editable("Module"));
    ASSERT_FALSE(cbm_edit_label_editable("File"));
    ASSERT_FALSE(cbm_edit_label_editable("Folder"));
    ASSERT_FALSE(cbm_edit_label_editable("TestSuite"));
    ASSERT_FALSE(cbm_edit_label_editable(NULL));
    ASSERT_FALSE(cbm_edit_label_editable("function")); /* case-sensitive */
    PASS();
}

/* ── line utilities ─────────────────────────────────────────────── */

TEST(count_lines_basic) {
    ASSERT_EQ(cbm_edit_count_lines("", 0), 0);
    ASSERT_EQ(cbm_edit_count_lines("a\nb\nc\n", 6), 3);
    ASSERT_EQ(cbm_edit_count_lines("a\nb", 3), 2); /* unterminated last line counts */
    ASSERT_EQ(cbm_edit_count_lines("only", 4), 1);
    PASS();
}

TEST(line_offset_basic) {
    const char *d = "ab\ncde\nf\n";
    size_t off = 999;
    ASSERT_EQ(cbm_edit_line_offset(d, strlen(d), 1, &off), CBM_EDIT_OK);
    ASSERT_EQ((long long)off, 0);
    ASSERT_EQ(cbm_edit_line_offset(d, strlen(d), 2, &off), CBM_EDIT_OK);
    ASSERT_EQ((long long)off, 3);
    ASSERT_EQ(cbm_edit_line_offset(d, strlen(d), 3, &off), CBM_EDIT_OK);
    ASSERT_EQ((long long)off, 7);
    /* line total+1 resolves to EOF when the file ends with a newline */
    ASSERT_EQ(cbm_edit_line_offset(d, strlen(d), 4, &off), CBM_EDIT_OK);
    ASSERT_EQ((long long)off, 9);
    ASSERT_EQ(cbm_edit_line_offset(d, strlen(d), 5, &off), CBM_EDIT_ERR_RANGE);
    PASS();
}

/* ── surgery: replace / insert (mirrors Python cases 1-7) ───────── */

TEST(surgery_replace_middle) {
    EXPECT_SURGERY("int a() { return 1; }\nint b() {\n  return 2;\n}\nint c() { return 3; }\n", 2,
                   4, CBM_EDIT_REPLACE_BODY, "int b() {\n  return 42;\n}",
                   "int a() { return 1; }\nint b() {\n  return 42;\n}\nint c() { return 3; }\n");
    PASS();
}

TEST(surgery_insert_before_first) {
    EXPECT_SURGERY("line1\nline2\n", 1, 1, CBM_EDIT_INSERT_BEFORE, "header",
                   "header\nline1\nline2\n");
    PASS();
}

TEST(surgery_insert_after_eof_no_trailing_newline) {
    EXPECT_SURGERY("line1\nline2", 2, 2, CBM_EDIT_INSERT_AFTER, "line3", "line1\nline2\nline3\n");
    PASS();
}

TEST(surgery_crlf_normalize_on_replace) {
    EXPECT_SURGERY("fn a() {\r\n  x();\r\n}\r\nfn b() {}\r\n", 1, 3, CBM_EDIT_REPLACE_BODY,
                   "fn a() {\n  y();\n}", "fn a() {\r\n  y();\r\n}\r\nfn b() {}\r\n");
    PASS();
}

TEST(surgery_strip_crlf_in_lf_file) {
    EXPECT_SURGERY("a\nb\n", 1, 1, CBM_EDIT_REPLACE_BODY, "a1\r\na2\r\n", "a1\na2\nb\n");
    PASS();
}

TEST(surgery_replace_whole_unterminated_file) {
    EXPECT_SURGERY("only", 1, 1, CBM_EDIT_REPLACE_BODY, "new only", "new only\n");
    PASS();
}

TEST(surgery_insert_after_middle) {
    EXPECT_SURGERY("l1\nl2\nl3\n", 2, 2, CBM_EDIT_INSERT_AFTER, "inserted",
                   "l1\nl2\ninserted\nl3\n");
    PASS();
}

TEST(surgery_range_out_of_bounds_rejected) {
    const char *d = "one\ntwo\n";
    char *out = NULL;
    size_t out_len = 0;
    cbm_edit_surgery_stats_t stats;
    /* end past EOF — the stale-index case */
    ASSERT_EQ(cbm_edit_surgery_apply(d, strlen(d), 1, 3, CBM_EDIT_REPLACE_BODY, "x", 1, &out,
                                     &out_len, &stats),
              CBM_EDIT_ERR_RANGE);
    /* start < 1 — invalid argument */
    ASSERT_EQ(cbm_edit_surgery_apply(d, strlen(d), 0, 1, CBM_EDIT_REPLACE_BODY, "x", 1, &out,
                                     &out_len, &stats),
              CBM_EDIT_ERR_ARGS);
    /* start > end — invalid argument */
    ASSERT_EQ(cbm_edit_surgery_apply(d, strlen(d), 2, 1, CBM_EDIT_REPLACE_BODY, "x", 1, &out,
                                     &out_len, &stats),
              CBM_EDIT_ERR_ARGS);
    PASS();
}

/* ── surgery: delete (mirrors Python cases 8-13) ────────────────── */

TEST(delete_middle_absorbs_following_blank) {
    EXPECT_DELETE("int a() { return 1; }\nint b() {\n  return 2;\n}\n\nint c() { return 3; }\n", 2,
                  4, "int a() { return 1; }\nint c() { return 3; }\n");
    PASS();
}

TEST(delete_at_eof_absorbs_preceding_blank) {
    EXPECT_DELETE("int a() { return 1; }\n\nint b() { return 2; }\n", 3, 3,
                  "int a() { return 1; }\n");
    PASS();
}

TEST(delete_first_line) {
    EXPECT_DELETE("x\ny\nz\n", 1, 1, "y\nz\n");
    PASS();
}

TEST(delete_everything) {
    EXPECT_DELETE("x\ny\nz\n", 1, 3, "");
    PASS();
}

TEST(delete_crlf_absorbs_following_blank) {
    EXPECT_DELETE("a\r\nb\r\n\r\nc\r\n", 2, 2, "a\r\nc\r\n");
    PASS();
}

TEST(delete_unterminated_last_line_absorbs_blank) {
    EXPECT_DELETE("a\n\nb", 3, 3, "a\n");
    PASS();
}

/* ── rename: identifier scan (mirrors Python cases 14, 19) ──────── */

TEST(is_valid_identifier) {
    ASSERT_TRUE(cbm_edit_is_valid_identifier("foo"));
    ASSERT_TRUE(cbm_edit_is_valid_identifier("_private"));
    ASSERT_TRUE(cbm_edit_is_valid_identifier("CamelCase9"));
    ASSERT_FALSE(cbm_edit_is_valid_identifier("9lives"));
    ASSERT_FALSE(cbm_edit_is_valid_identifier(""));
    ASSERT_FALSE(cbm_edit_is_valid_identifier("has-dash"));
    ASSERT_FALSE(cbm_edit_is_valid_identifier("has space"));
    ASSERT_FALSE(cbm_edit_is_valid_identifier("ptr->field"));
    ASSERT_FALSE(cbm_edit_is_valid_identifier(NULL));
    PASS();
}

TEST(scan_whole_identifier_boundaries) {
    const char *data = "order reorder order_id _order order(x) x.order\n";
    int count = 0;
    cbm_edit_occurrence_t *occs = scan_all(data, "order", &count);
    ASSERT_NOT_NULL(occs);
    /* matches: standalone 'order' at offsets 0, 30 and 41 (inside x.order);
     * reorder / order_id / _order must NOT match. */
    ASSERT_EQ(count, 3);
    ASSERT_EQ((long long)occs[0].offset, 0);
    ASSERT_EQ((long long)occs[1].offset, 30);
    ASSERT_EQ((long long)occs[2].offset, 41);
    ASSERT_EQ(occs[0].line, 1);
    free(occs);
    PASS();
}

TEST(scan_results_sorted_and_line_tracked) {
    const char *data = "x foo\nfoo y\nz foo\n";
    int count = 0;
    cbm_edit_occurrence_t *occs = scan_all(data, "foo", &count);
    ASSERT_NOT_NULL(occs);
    ASSERT_EQ(count, 3);
    ASSERT_EQ((long long)occs[0].offset, 2);
    ASSERT_EQ(occs[0].line, 1);
    ASSERT_EQ(occs[1].line, 2);
    ASSERT_EQ(occs[2].line, 3);
    ASSERT_TRUE(occs[0].offset < occs[1].offset && occs[1].offset < occs[2].offset);
    free(occs);
    PASS();
}

/* ── rename: buffer splice (mirrors Python cases 15-17, 19) ─────── */

static int expect_rename(const char *data, const char *old_name, const char *new_name,
                         const bool *apply, int count, const cbm_edit_occurrence_t *occs,
                         const char *expected) {
    char *out = NULL;
    size_t out_len = 0;
    int rc = cbm_edit_rename_in_buffer(data, strlen(data), old_name, new_name, occs, apply, count,
                                       &out, &out_len);
    ASSERT_EQ(rc, CBM_EDIT_OK);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ((long long)out_len, (long long)strlen(expected));
    ASSERT_MEM_EQ(out, expected, out_len);
    free(out);
    return 0;
}

TEST(rename_with_apply_mask) {
    const char *data = "foo();\n// foo comment\nfoo();\n";
    int count = 0;
    cbm_edit_occurrence_t *occs = scan_all(data, "foo", &count);
    ASSERT_NOT_NULL(occs);
    ASSERT_EQ(count, 3);
    bool apply[3] = {true, false, true};
    EXPECT_RENAME(data, "foo", "bar", apply, count, occs, "bar();\n// foo comment\nbar();\n");
    free(occs);
    PASS();
}

TEST(rename_growth) {
    const char *data = "a b a\n";
    int count = 0;
    cbm_edit_occurrence_t *occs = scan_all(data, "a", &count);
    ASSERT_NOT_NULL(occs);
    ASSERT_EQ(count, 2);
    bool apply[2] = {true, true};
    EXPECT_RENAME(data, "a", "longer", apply, count, occs, "longer b longer\n");
    free(occs);
    PASS();
}

TEST(rename_shrink) {
    const char *data = "longer b longer\n";
    int count = 0;
    cbm_edit_occurrence_t *occs = scan_all(data, "longer", &count);
    ASSERT_NOT_NULL(occs);
    ASSERT_EQ(count, 2);
    bool apply[2] = {true, true};
    EXPECT_RENAME(data, "longer", "a", apply, count, occs, "a b a\n");
    free(occs);
    PASS();
}

TEST(rename_occurrences_at_buffer_edges) {
    const char *data = "foo x foo";
    int count = 0;
    cbm_edit_occurrence_t *occs = scan_all(data, "foo", &count);
    ASSERT_NOT_NULL(occs);
    ASSERT_EQ(count, 2);
    bool apply[2] = {true, true};
    EXPECT_RENAME(data, "foo", "baz", apply, count, occs, "baz x baz");
    free(occs);
    PASS();
}

TEST(rename_stale_occurrence_rejected) {
    /* The occurrence no longer points at old_name — the buffer changed after
     * the scan. rename must refuse instead of corrupting the file. */
    const char *data = "bar x bar";
    cbm_edit_occurrence_t occs[1] = {{1, 0}};
    bool apply[1] = {true};
    char *out = NULL;
    size_t out_len = 0;
    int rc =
        cbm_edit_rename_in_buffer(data, strlen(data), "foo", "baz", occs, apply, 1, &out, &out_len);
    ASSERT_NEQ(rc, CBM_EDIT_OK);
    PASS();
}

/* ── rename: comment heuristic (mirrors Python case 18) ─────────── */

TEST(line_looks_like_comment) {
    const char *c1 = "   // note foo";
    ASSERT_TRUE(cbm_edit_line_looks_like_comment(c1, strlen(c1), 0));
    const char *c2 = "\t# py note";
    ASSERT_TRUE(cbm_edit_line_looks_like_comment(c2, strlen(c2), 0));
    const char *c3 = " * block body";
    ASSERT_TRUE(cbm_edit_line_looks_like_comment(c3, strlen(c3), 0));
    const char *c4 = "-- sql note";
    ASSERT_TRUE(cbm_edit_line_looks_like_comment(c4, strlen(c4), 0));
    const char *code1 = "  foo();";
    ASSERT_FALSE(cbm_edit_line_looks_like_comment(code1, strlen(code1), 0));
    const char *code2 = "  x * y;";
    ASSERT_FALSE(cbm_edit_line_looks_like_comment(code2, strlen(code2), 0));
    /* heuristic applies at a mid-buffer line start, not just offset 0 */
    const char *multi = "int x;\n// gone\nint y;\n";
    ASSERT_TRUE(cbm_edit_line_looks_like_comment(multi, strlen(multi), 7));
    ASSERT_FALSE(cbm_edit_line_looks_like_comment(multi, strlen(multi), 0));
    PASS();
}

/* ── plan preview ───────────────────────────────────────────────── */

TEST(plan_preview_replace_contains_markers) {
    const char *old_data = "int a() { return 1; }\nint b() { return 2; }\n";
    const char *content = "int b() { return 42; }";
    char *preview = cbm_edit_plan_preview(old_data, strlen(old_data), 2, 2, CBM_EDIT_REPLACE_BODY,
                                          content, strlen(content), 3, 1 << 16);
    ASSERT_NOT_NULL(preview);
    /* removed lines are prefixed "- ", added lines "+ ", context "  " */
    ASSERT_NOT_NULL(strstr(preview, "- int b() { return 2; }"));
    ASSERT_NOT_NULL(strstr(preview, "+ int b() { return 42; }"));
    ASSERT_NOT_NULL(strstr(preview, "  int a() { return 1; }"));
    ASSERT_NOT_NULL(strstr(preview, "@@ -2,1 +2,1 @@"));
    free(preview);
    PASS();
}

TEST(plan_preview_respects_max_bytes) {
    /* Long content with a small cap: the preview must stay within max_bytes
     * (the truncation marker is appended only when it still fits). Note that
     * max_bytes < 256 is rejected with NULL by contract. */
    const char *old_data = "line one\nline two\n";
    const char *content = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
                          "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
                          "cccccccccccccccccccccccccccccc\n"
                          "dddddddddddddddddddddddddddddd\n"
                          "eeeeeeeeeeeeeeeeeeeeeeeeeeeeee\n"
                          "ffffffffffffffffffffffffffffff\n"
                          "gggggggggggggggggggggggggggggg\n"
                          "hhhhhhhhhhhhhhhhhhhhhhhhhhhhhh\n"
                          "iiiiiiiiiiiiiiiiiiiiiiiiiiiiii\n"
                          "jjjjjjjjjjjjjjjjjjjjjjjjjjjjjj\n";
    char *preview = cbm_edit_plan_preview(old_data, strlen(old_data), 1, 1, CBM_EDIT_REPLACE_BODY,
                                          content, strlen(content), 3, 256);
    ASSERT_NOT_NULL(preview);
    ASSERT_LTE((long long)strlen(preview), 256);
    free(preview);
    /* below the minimum cap → NULL */
    preview = cbm_edit_plan_preview(old_data, strlen(old_data), 1, 1, CBM_EDIT_REPLACE_BODY,
                                    content, strlen(content), 3, 16);
    ASSERT_NULL(preview);
    PASS();
}

/* ── file state / atomic write (real temp dir) ──────────────────── */

TEST(file_stat_and_read_roundtrip) {
    char *dir = th_mktempdir("cbm_edit_stat");
    ASSERT_NOT_NULL(dir);
    const char *path = TH_PATH(dir, "sample.txt");
    ASSERT_EQ(th_write_file(path, "hello\nworld\n"), 0);

    cbm_edit_file_state_t st;
    ASSERT_EQ(cbm_edit_file_stat(path, &st), CBM_EDIT_OK);
    ASSERT_EQ((long long)st.size, 12);

    char *data = NULL;
    size_t len = 0;
    ASSERT_EQ(cbm_edit_read_file(path, &data, &len), CBM_EDIT_OK);
    ASSERT_EQ((long long)len, 12);
    ASSERT_MEM_EQ(data, "hello\nworld\n", 12);
    free(data);

    ASSERT_EQ(cbm_edit_file_stat(TH_PATH(dir, "missing.txt"), &st), CBM_EDIT_ERR_IO);
    th_cleanup(dir);
    PASS();
}

TEST(write_atomic_replaces_and_backups_up) {
    char *dir = th_mktempdir("cbm_edit_write");
    ASSERT_NOT_NULL(dir);
    const char *path = TH_PATH(dir, "target.c");
    ASSERT_EQ(th_write_file(path, "int old_fn() { return 1; }\n"), 0);

    cbm_edit_file_state_t st;
    ASSERT_EQ(cbm_edit_file_stat(path, &st), CBM_EDIT_OK);

    char backup_path[1024];
    backup_path[0] = '\0';
    const char *new_content = "int new_fn() { return 2; }\n";
    ASSERT_EQ(cbm_edit_write_atomic(path, new_content, strlen(new_content), &st,
                                    TH_PATH(dir, "backups"), backup_path, sizeof(backup_path)),
              CBM_EDIT_OK);

    /* target holds the new content */
    char *data = NULL;
    size_t len = 0;
    ASSERT_EQ(cbm_edit_read_file(path, &data, &len), CBM_EDIT_OK);
    ASSERT_STR_EQ(data, new_content);
    free(data);

    /* backup holds the original content */
    ASSERT_TRUE(backup_path[0] != '\0');
    ASSERT_EQ(cbm_edit_read_file(backup_path, &data, &len), CBM_EDIT_OK);
    ASSERT_STR_EQ(data, "int old_fn() { return 1; }\n");
    free(data);

    th_cleanup(dir);
    PASS();
}

TEST(write_atomic_mtime_mismatch_refuses) {
    char *dir = th_mktempdir("cbm_edit_mtime");
    ASSERT_NOT_NULL(dir);
    const char *path = TH_PATH(dir, "guarded.c");
    ASSERT_EQ(th_write_file(path, "v1\n"), 0);

    cbm_edit_file_state_t st;
    ASSERT_EQ(cbm_edit_file_stat(path, &st), CBM_EDIT_OK);

    /* External modification after the stat — size changes, mtime may too. */
    ASSERT_EQ(th_write_file(path, "v1 changed externally\n"), 0);

    ASSERT_EQ(cbm_edit_write_atomic(path, "agent write\n", strlen("agent write\n"), &st,
                                    TH_PATH(dir, "backups"), NULL, 0),
              CBM_EDIT_ERR_MTIME);

    /* The external content must be untouched by the refused write. */
    char *data = NULL;
    size_t len = 0;
    ASSERT_EQ(cbm_edit_read_file(path, &data, &len), CBM_EDIT_OK);
    ASSERT_STR_EQ(data, "v1 changed externally\n");
    free(data);

    th_cleanup(dir);
    PASS();
}

TEST(write_atomic_null_expected_writes_anyway) {
    char *dir = th_mktempdir("cbm_edit_noexp");
    ASSERT_NOT_NULL(dir);
    const char *path = TH_PATH(dir, "plain.txt");
    ASSERT_EQ(th_write_file(path, "before\n"), 0);
    ASSERT_EQ(cbm_edit_write_atomic(path, "after\n", 6, NULL, TH_PATH(dir, "bk"), NULL, 0),
              CBM_EDIT_OK);
    char *data = NULL;
    size_t len = 0;
    ASSERT_EQ(cbm_edit_read_file(path, &data, &len), CBM_EDIT_OK);
    ASSERT_STR_EQ(data, "after\n");
    free(data);
    th_cleanup(dir);
    PASS();
}

/* ── backup discovery / undo (cbm_edit_latest_backup) ───────────── */

TEST(latest_backup_picks_newest_epoch_then_pid) {
    char *dir = th_mktempdir("cbm_edit_bkfind");
    ASSERT_NOT_NULL(dir);
    /* TH_PATH rotates a 4-slot ring — pin long-lived paths into locals. */
    char bk[1024];
    snprintf(bk, sizeof(bk), "%s", TH_PATH(dir, "backups"));
    ASSERT_EQ(th_write_file(TH_PATH(bk, "bk_100_1_target.c"), "v1"), 0);
    ASSERT_EQ(th_write_file(TH_PATH(bk, "bk_300_1_target.c"), "v3"), 0);
    ASSERT_EQ(th_write_file(TH_PATH(bk, "bk_200_2_target.c"), "v2"), 0);
    ASSERT_EQ(th_write_file(TH_PATH(bk, "bk_999_9_other.c"), "other"), 0);
    /* junk names must be ignored */
    ASSERT_EQ(th_write_file(TH_PATH(bk, "notes.txt"), "x"), 0);
    ASSERT_EQ(th_write_file(TH_PATH(bk, "bk_abc_1_target.c"), "x"), 0);
    ASSERT_EQ(th_write_file(TH_PATH(bk, "bk_400_1_target.c.bak"), "x"), 0);

    char out[1024];
    ASSERT_EQ(cbm_edit_latest_backup(bk, "target.c", out, sizeof(out)), CBM_EDIT_OK);
    ASSERT_NOT_NULL(strstr(out, "bk_300_1_target.c"));
    ASSERT_EQ(cbm_edit_latest_backup(bk, "other.c", out, sizeof(out)), CBM_EDIT_OK);
    ASSERT_NOT_NULL(strstr(out, "bk_999_9_other.c"));
    th_cleanup(dir);
    PASS();
}

TEST(latest_backup_no_match) {
    char *dir = th_mktempdir("cbm_edit_bknone");
    ASSERT_NOT_NULL(dir);
    char bk[1024];
    snprintf(bk, sizeof(bk), "%s", TH_PATH(dir, "backups"));
    ASSERT_EQ(th_write_file(TH_PATH(bk, "bk_100_1_other.c"), "x"), 0);
    char out[1024];
    ASSERT_EQ(cbm_edit_latest_backup(bk, "target.c", out, sizeof(out)), CBM_EDIT_ERR_RANGE);
    ASSERT_EQ(cbm_edit_latest_backup(TH_PATH(dir, "no-such-dir"), "target.c", out, sizeof(out)),
              CBM_EDIT_ERR_IO);
    th_cleanup(dir);
    PASS();
}

TEST(undo_roundtrip_via_latest_backup) {
    char *dir = th_mktempdir("cbm_edit_undo");
    ASSERT_NOT_NULL(dir);
    char path[1024];
    snprintf(path, sizeof(path), "%s", TH_PATH(dir, "target.c"));
    char bk[1024];
    snprintf(bk, sizeof(bk), "%s", TH_PATH(dir, "backups"));
    ASSERT_EQ(th_write_file(path, "int v1() { return 1; }\n"), 0);
    /* one guarded write → exactly one backup holding v1 */
    const char *v2 = "int v2() { return 2; }\n";
    ASSERT_EQ(cbm_edit_write_atomic(path, v2, strlen(v2), NULL, bk, NULL, 0), CBM_EDIT_OK);

    char backup_path[1024];
    ASSERT_EQ(cbm_edit_latest_backup(bk, "target.c", backup_path, sizeof(backup_path)),
              CBM_EDIT_OK);
    char *data = NULL;
    size_t len = 0;
    ASSERT_EQ(cbm_edit_read_file(backup_path, &data, &len), CBM_EDIT_OK);
    ASSERT_STR_EQ(data, "int v1() { return 1; }\n");

    /* restore: write the backup content back through the same atomic path */
    ASSERT_EQ(cbm_edit_write_atomic(path, data, len, NULL, bk, NULL, 0), CBM_EDIT_OK);
    free(data);
    ASSERT_EQ(cbm_edit_read_file(path, &data, &len), CBM_EDIT_OK);
    ASSERT_STR_EQ(data, "int v1() { return 1; }\n");
    free(data);
    th_cleanup(dir);
    PASS();
}

/* ── move: extract / append / Python import rewrite ─────────────── */

static int expect_extract(const char *data, int start, int end, const char *expected) {
    char *out = NULL;
    size_t out_len = 0;
    int rc = cbm_edit_move_extract_lines(data, strlen(data), start, end, &out, &out_len);
    ASSERT_EQ(rc, CBM_EDIT_OK);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ((long long)out_len, (long long)strlen(expected));
    ASSERT_MEM_EQ(out, expected, out_len);
    free(out);
    return 0;
}

static int expect_append(const char *data, const char *def, const char *expected) {
    char *out = NULL;
    size_t out_len = 0;
    int rc = cbm_edit_move_append_definition(data, strlen(data), def, strlen(def), &out, &out_len);
    ASSERT_EQ(rc, CBM_EDIT_OK);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ((long long)out_len, (long long)strlen(expected));
    ASSERT_MEM_EQ(out, expected, out_len);
    free(out);
    return 0;
}

static int expect_py_rewrite(const char *data, const char *old_mod, const char *new_mod,
                             const char *symbol, const char *expected) {
    char *out = NULL;
    size_t out_len = 0;
    int rc = cbm_edit_move_rewrite_python_imports(data, strlen(data), old_mod, new_mod, symbol,
                                                  &out, &out_len, NULL);
    ASSERT_EQ(rc, CBM_EDIT_OK);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ((long long)out_len, (long long)strlen(expected));
    ASSERT_MEM_EQ(out, expected, out_len);
    free(out);
    return 0;
}

#define EXPECT_EXTRACT(...) ASSERT_EQ(expect_extract(__VA_ARGS__), 0)
#define EXPECT_APPEND(...) ASSERT_EQ(expect_append(__VA_ARGS__), 0)
#define EXPECT_PY_REWRITE(...) ASSERT_EQ(expect_py_rewrite(__VA_ARGS__), 0)

TEST(move_extract_middle) {
    EXPECT_EXTRACT("line1\nline2\nline3\nline4\n", 2, 3, "line2\nline3\n");
    PASS();
}

TEST(move_extract_unterminated_last_line) {
    EXPECT_EXTRACT("line1\nline2", 2, 2, "line2");
    PASS();
}

TEST(move_extract_out_of_range_rejected) {
    char *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ(cbm_edit_move_extract_lines("line1\n", 6, 2, 2, &out, &out_len), CBM_EDIT_ERR_RANGE);
    PASS();
}

TEST(move_append_to_empty_file) {
    EXPECT_APPEND("", "def f():\n    pass\n", "def f():\n    pass\n");
    PASS();
}

TEST(move_append_inserts_one_blank_separator) {
    EXPECT_APPEND("import os\n", "def f():\n    pass\n", "import os\n\ndef f():\n    pass\n");
    PASS();
}

TEST(move_append_no_double_blank_when_already_blank) {
    EXPECT_APPEND("import os\n\n", "def f():\n    pass\n", "import os\n\ndef f():\n    pass\n");
    PASS();
}

TEST(move_append_terminates_unterminated_last_line) {
    EXPECT_APPEND("import os", "def f():\n    pass\n", "import os\n\ndef f():\n    pass\n");
    PASS();
}

TEST(move_append_crlf_file_normalizes_def) {
    EXPECT_APPEND("import os\r\n", "def f():\n    pass\n",
                  "import os\r\n\r\ndef f():\r\n    pass\r\n");
    PASS();
}

TEST(move_py_rewrite_single_name) {
    EXPECT_PY_REWRITE("from mod_a import f\n", "mod_a", "mod_b", "f", "from mod_b import f\n");
    PASS();
}

TEST(move_py_rewrite_multi_name_split) {
    EXPECT_PY_REWRITE("from mod_a import f, g\n", "mod_a", "mod_b", "f",
                      "from mod_a import g\nfrom mod_b import f\n");
    PASS();
}

TEST(move_py_rewrite_multi_name_split_keeps_order) {
    EXPECT_PY_REWRITE("from mod_a import g, f, h\n", "mod_a", "mod_b", "f",
                      "from mod_a import g, h\nfrom mod_b import f\n");
    PASS();
}

TEST(move_py_rewrite_alias_kept) {
    cbm_edit_move_py_stats_t st;
    char *out = NULL;
    size_t out_len = 0;
    const char *data = "from mod_a import f as f1\n";
    ASSERT_EQ(cbm_edit_move_rewrite_python_imports(data, strlen(data), "mod_a", "mod_b", "f", &out,
                                                   &out_len, &st),
              CBM_EDIT_OK);
    ASSERT_STR_EQ(out, "from mod_b import f as f1\n");
    ASSERT_EQ(st.import_lines_rewritten, 1);
    ASSERT_EQ(st.aliases_kept, 1);
    free(out);
    PASS();
}

TEST(move_py_rewrite_trailing_comment_preserved) {
    EXPECT_PY_REWRITE("from mod_a import f  # legacy\n", "mod_a", "mod_b", "f",
                      "from mod_b import f  # legacy\n");
    PASS();
}

TEST(move_py_rewrite_split_keeps_comment_on_old_line) {
    EXPECT_PY_REWRITE("from mod_a import f, g  # both\n", "mod_a", "mod_b", "f",
                      "from mod_a import g  # both\nfrom mod_b import f\n");
    PASS();
}

TEST(move_py_rewrite_indented_split_keeps_indent) {
    EXPECT_PY_REWRITE("def use():\n    from mod_a import f, g\n", "mod_a", "mod_b", "f",
                      "def use():\n    from mod_a import g\n    from mod_b import f\n");
    PASS();
}

TEST(move_py_rewrite_ignores_other_modules) {
    const char *data = "from other import f\nfrom mod_a import g\n";
    EXPECT_PY_REWRITE(data, "mod_a", "mod_b", "f", data);
    PASS();
}

TEST(move_py_rewrite_exact_module_match_only) {
    /* `mod_a` must not match `mod_a.sub` or `pkg.mod_a` */
    const char *data = "from mod_a.sub import f\nfrom pkg.mod_a import f\n";
    EXPECT_PY_REWRITE(data, "mod_a", "mod_b", "f", data);
    PASS();
}

TEST(move_py_rewrite_plain_import_counted_untouched) {
    cbm_edit_move_py_stats_t st;
    char *out = NULL;
    size_t out_len = 0;
    const char *data = "import mod_a\nimport os, mod_a as ma\n";
    ASSERT_EQ(cbm_edit_move_rewrite_python_imports(data, strlen(data), "mod_a", "mod_b", "f", &out,
                                                   &out_len, &st),
              CBM_EDIT_OK);
    ASSERT_STR_EQ(out, data);
    ASSERT_EQ(st.plain_import_refs, 2);
    ASSERT_EQ(st.import_lines_rewritten, 0);
    free(out);
    PASS();
}

TEST(move_py_rewrite_star_import_counted_untouched) {
    cbm_edit_move_py_stats_t st;
    char *out = NULL;
    size_t out_len = 0;
    const char *data = "from mod_a import *\n";
    ASSERT_EQ(cbm_edit_move_rewrite_python_imports(data, strlen(data), "mod_a", "mod_b", "f", &out,
                                                   &out_len, &st),
              CBM_EDIT_OK);
    ASSERT_STR_EQ(out, data);
    ASSERT_EQ(st.star_import_refs, 1);
    free(out);
    PASS();
}

TEST(move_py_rewrite_parenthesized_counted_untouched) {
    cbm_edit_move_py_stats_t st;
    char *out = NULL;
    size_t out_len = 0;
    const char *data = "from mod_a import (f,\n                   g)\n";
    ASSERT_EQ(cbm_edit_move_rewrite_python_imports(data, strlen(data), "mod_a", "mod_b", "f", &out,
                                                   &out_len, &st),
              CBM_EDIT_OK);
    ASSERT_STR_EQ(out, data);
    ASSERT_EQ(st.parenthesized_skipped, 1);
    free(out);
    PASS();
}

TEST(move_py_rewrite_all_ref_counted_untouched) {
    cbm_edit_move_py_stats_t st;
    char *out = NULL;
    size_t out_len = 0;
    const char *data = "__all__ = [\"f\", \"g\"]\n";
    ASSERT_EQ(cbm_edit_move_rewrite_python_imports(data, strlen(data), "mod_a", "mod_b", "f", &out,
                                                   &out_len, &st),
              CBM_EDIT_OK);
    ASSERT_STR_EQ(out, data);
    ASSERT_EQ(st.all_refs, 1);
    free(out);
    PASS();
}

TEST(move_py_rewrite_crlf_preserved) {
    EXPECT_PY_REWRITE("from mod_a import f, g\r\n", "mod_a", "mod_b", "f",
                      "from mod_a import g\r\nfrom mod_b import f\r\n");
    PASS();
}

TEST(move_py_rewrite_symbol_not_in_list_untouched) {
    const char *data = "from mod_a import g, h\n";
    EXPECT_PY_REWRITE(data, "mod_a", "mod_b", "f", data);
    PASS();
}

/* ── move: TypeScript import rewrite + relative spec ────────────── */

static int expect_ts_rewrite(const char *data, const char *old_spec, const char *new_spec,
                             const char *symbol, const char *expected) {
    char *out = NULL;
    size_t out_len = 0;
    int rc = cbm_edit_move_rewrite_ts_imports(data, strlen(data), old_spec, new_spec, symbol, &out,
                                              &out_len, NULL);
    ASSERT_EQ(rc, CBM_EDIT_OK);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ((long long)out_len, (long long)strlen(expected));
    ASSERT_MEM_EQ(out, expected, out_len);
    free(out);
    return 0;
}

#define EXPECT_TS_REWRITE(...) ASSERT_EQ(expect_ts_rewrite(__VA_ARGS__), 0)

static int expect_rel_spec(const char *importer, const char *module, const char *expected) {
    char out[1024];
    int rc = cbm_edit_move_ts_relative_spec(importer, module, out, sizeof(out));
    ASSERT_EQ(rc, CBM_EDIT_OK);
    ASSERT_STR_EQ(out, expected);
    return 0;
}

#define EXPECT_REL_SPEC(...) ASSERT_EQ(expect_rel_spec(__VA_ARGS__), 0)

TEST(move_ts_rewrite_single_name) {
    EXPECT_TS_REWRITE("import {f} from \"./mod_a\";\n", "./mod_a", "./mod_b", "f",
                      "import {f} from \"./mod_b\";\n");
    PASS();
}

TEST(move_ts_rewrite_preserves_quote_and_no_semicolon) {
    EXPECT_TS_REWRITE("import {f} from './mod_a'\n", "./mod_a", "./mod_b", "f",
                      "import {f} from './mod_b'\n");
    PASS();
}

TEST(move_ts_rewrite_multi_name_split) {
    EXPECT_TS_REWRITE("import {f, g} from \"./mod_a\";\n", "./mod_a", "./mod_b", "f",
                      "import {g} from \"./mod_a\";\nimport {f} from \"./mod_b\";\n");
    PASS();
}

TEST(move_ts_rewrite_split_keeps_padding_and_order) {
    EXPECT_TS_REWRITE("import { g, f, h } from \"./mod_a\";\n", "./mod_a", "./mod_b", "f",
                      "import { g, h } from \"./mod_a\";\nimport { f } from \"./mod_b\";\n");
    PASS();
}

TEST(move_ts_rewrite_alias_kept) {
    cbm_edit_move_ts_stats_t st;
    char *out = NULL;
    size_t out_len = 0;
    const char *data = "import {f as f1} from \"./mod_a\";\n";
    ASSERT_EQ(cbm_edit_move_rewrite_ts_imports(data, strlen(data), "./mod_a", "./mod_b", "f", &out,
                                               &out_len, &st),
              CBM_EDIT_OK);
    ASSERT_STR_EQ(out, "import {f as f1} from \"./mod_b\";\n");
    ASSERT_EQ(st.import_lines_rewritten, 1);
    ASSERT_EQ(st.aliases_kept, 1);
    free(out);
    PASS();
}

TEST(move_ts_rewrite_import_type_preserved) {
    EXPECT_TS_REWRITE("import type {f, g} from \"./mod_a\";\n", "./mod_a", "./mod_b", "f",
                      "import type {g} from \"./mod_a\";\nimport type {f} from \"./mod_b\";\n");
    PASS();
}

TEST(move_ts_rewrite_inline_type_entry_preserved) {
    EXPECT_TS_REWRITE("import {type f, g} from \"./mod_a\";\n", "./mod_a", "./mod_b", "f",
                      "import {g} from \"./mod_a\";\nimport {type f} from \"./mod_b\";\n");
    PASS();
}

TEST(move_ts_rewrite_ignores_other_specs) {
    const char *data = "import {f} from \"./other\";\nimport {g} from \"./mod_a\";\n";
    EXPECT_TS_REWRITE(data, "./mod_a", "./mod_b", "f", data);
    PASS();
}

TEST(move_ts_rewrite_default_import_counted_untouched) {
    cbm_edit_move_ts_stats_t st;
    char *out = NULL;
    size_t out_len = 0;
    const char *data = "import modA from \"./mod_a\";\nimport def, {f} from \"./mod_a\";\n";
    ASSERT_EQ(cbm_edit_move_rewrite_ts_imports(data, strlen(data), "./mod_a", "./mod_b", "f", &out,
                                               &out_len, &st),
              CBM_EDIT_OK);
    ASSERT_STR_EQ(out, data);
    ASSERT_EQ(st.default_import_refs, 2);
    free(out);
    PASS();
}

TEST(move_ts_rewrite_namespace_counted_untouched) {
    cbm_edit_move_ts_stats_t st;
    char *out = NULL;
    size_t out_len = 0;
    const char *data = "import * as modA from \"./mod_a\";\n";
    ASSERT_EQ(cbm_edit_move_rewrite_ts_imports(data, strlen(data), "./mod_a", "./mod_b", "f", &out,
                                               &out_len, &st),
              CBM_EDIT_OK);
    ASSERT_STR_EQ(out, data);
    ASSERT_EQ(st.namespace_import_refs, 1);
    free(out);
    PASS();
}

TEST(move_ts_rewrite_side_effect_counted_untouched) {
    cbm_edit_move_ts_stats_t st;
    char *out = NULL;
    size_t out_len = 0;
    const char *data = "import \"./mod_a\";\n";
    ASSERT_EQ(cbm_edit_move_rewrite_ts_imports(data, strlen(data), "./mod_a", "./mod_b", "f", &out,
                                               &out_len, &st),
              CBM_EDIT_OK);
    ASSERT_STR_EQ(out, data);
    ASSERT_EQ(st.side_effect_refs, 1);
    free(out);
    PASS();
}

TEST(move_ts_rewrite_barrel_counted_untouched) {
    cbm_edit_move_ts_stats_t st;
    char *out = NULL;
    size_t out_len = 0;
    const char *data = "export {f} from \"./mod_a\";\nexport * from \"./mod_a\";\n";
    ASSERT_EQ(cbm_edit_move_rewrite_ts_imports(data, strlen(data), "./mod_a", "./mod_b", "f", &out,
                                               &out_len, &st),
              CBM_EDIT_OK);
    ASSERT_STR_EQ(out, data);
    ASSERT_EQ(st.barrel_refs, 2);
    free(out);
    PASS();
}

TEST(move_ts_rewrite_dynamic_and_require_counted_untouched) {
    cbm_edit_move_ts_stats_t st;
    char *out = NULL;
    size_t out_len = 0;
    const char *data = "const m = await import(\"./mod_a\");\nconst n = require(\"./mod_a\");\n";
    ASSERT_EQ(cbm_edit_move_rewrite_ts_imports(data, strlen(data), "./mod_a", "./mod_b", "f", &out,
                                               &out_len, &st),
              CBM_EDIT_OK);
    ASSERT_STR_EQ(out, data);
    ASSERT_EQ(st.dynamic_import_refs, 2);
    free(out);
    PASS();
}

TEST(move_ts_rewrite_multiline_counted_untouched) {
    cbm_edit_move_ts_stats_t st;
    char *out = NULL;
    size_t out_len = 0;
    const char *data = "import {\n    f,\n    g\n} from \"./mod_a\";\n";
    ASSERT_EQ(cbm_edit_move_rewrite_ts_imports(data, strlen(data), "./mod_a", "./mod_b", "f", &out,
                                               &out_len, &st),
              CBM_EDIT_OK);
    ASSERT_STR_EQ(out, data);
    ASSERT_EQ(st.multiline_skipped, 1);
    free(out);
    PASS();
}

TEST(move_ts_rewrite_crlf_preserved) {
    EXPECT_TS_REWRITE("import {f, g} from \"./mod_a\";\r\n", "./mod_a", "./mod_b", "f",
                      "import {g} from \"./mod_a\";\r\nimport {f} from \"./mod_b\";\r\n");
    PASS();
}

TEST(move_ts_rel_spec_same_dir) {
    EXPECT_REL_SPEC("src/main.ts", "src/utils.ts", "./utils");
    PASS();
}

TEST(move_ts_rel_spec_child_dir) {
    EXPECT_REL_SPEC("src/main.ts", "src/lib/utils.ts", "./lib/utils");
    PASS();
}

TEST(move_ts_rel_spec_parent_dir) {
    EXPECT_REL_SPEC("src/app/main.ts", "src/utils.ts", "../utils");
    PASS();
}

TEST(move_ts_rel_spec_sibling_deep) {
    EXPECT_REL_SPEC("src/app/main.ts", "src/shared/lib/utils.ts", "../shared/lib/utils");
    PASS();
}

TEST(move_ts_rel_spec_root_importer) {
    EXPECT_REL_SPEC("main.ts", "src/utils.ts", "./src/utils");
    PASS();
}

TEST(move_ts_rel_spec_module_at_root) {
    EXPECT_REL_SPEC("src/app/main.ts", "utils.ts", "../../utils");
    PASS();
}

TEST(move_ts_rel_spec_strips_extensions) {
    EXPECT_REL_SPEC("src/main.ts", "src/a.tsx", "./a");
    EXPECT_REL_SPEC("src/main.ts", "src/b.test.ts", "./b.test");
    PASS();
}

TEST(move_ts_rel_spec_index_kept_literal) {
    EXPECT_REL_SPEC("main.ts", "mod/index.ts", "./mod/index");
    PASS();
}

/* ── suite ──────────────────────────────────────────────────────── */

SUITE(edit) {
    RUN_TEST(action_parse_roundtrip);
    RUN_TEST(label_editable);
    RUN_TEST(count_lines_basic);
    RUN_TEST(line_offset_basic);
    RUN_TEST(surgery_replace_middle);
    RUN_TEST(surgery_insert_before_first);
    RUN_TEST(surgery_insert_after_eof_no_trailing_newline);
    RUN_TEST(surgery_crlf_normalize_on_replace);
    RUN_TEST(surgery_strip_crlf_in_lf_file);
    RUN_TEST(surgery_replace_whole_unterminated_file);
    RUN_TEST(surgery_insert_after_middle);
    RUN_TEST(surgery_range_out_of_bounds_rejected);
    RUN_TEST(delete_middle_absorbs_following_blank);
    RUN_TEST(delete_at_eof_absorbs_preceding_blank);
    RUN_TEST(delete_first_line);
    RUN_TEST(delete_everything);
    RUN_TEST(delete_crlf_absorbs_following_blank);
    RUN_TEST(delete_unterminated_last_line_absorbs_blank);
    RUN_TEST(is_valid_identifier);
    RUN_TEST(scan_whole_identifier_boundaries);
    RUN_TEST(scan_results_sorted_and_line_tracked);
    RUN_TEST(rename_with_apply_mask);
    RUN_TEST(rename_growth);
    RUN_TEST(rename_shrink);
    RUN_TEST(rename_occurrences_at_buffer_edges);
    RUN_TEST(rename_stale_occurrence_rejected);
    RUN_TEST(line_looks_like_comment);
    RUN_TEST(plan_preview_replace_contains_markers);
    RUN_TEST(plan_preview_respects_max_bytes);
    RUN_TEST(file_stat_and_read_roundtrip);
    RUN_TEST(write_atomic_replaces_and_backups_up);
    RUN_TEST(write_atomic_mtime_mismatch_refuses);
    RUN_TEST(write_atomic_null_expected_writes_anyway);
    RUN_TEST(latest_backup_picks_newest_epoch_then_pid);
    RUN_TEST(latest_backup_no_match);
    RUN_TEST(undo_roundtrip_via_latest_backup);
    RUN_TEST(move_extract_middle);
    RUN_TEST(move_extract_unterminated_last_line);
    RUN_TEST(move_extract_out_of_range_rejected);
    RUN_TEST(move_append_to_empty_file);
    RUN_TEST(move_append_inserts_one_blank_separator);
    RUN_TEST(move_append_no_double_blank_when_already_blank);
    RUN_TEST(move_append_terminates_unterminated_last_line);
    RUN_TEST(move_append_crlf_file_normalizes_def);
    RUN_TEST(move_py_rewrite_single_name);
    RUN_TEST(move_py_rewrite_multi_name_split);
    RUN_TEST(move_py_rewrite_multi_name_split_keeps_order);
    RUN_TEST(move_py_rewrite_alias_kept);
    RUN_TEST(move_py_rewrite_trailing_comment_preserved);
    RUN_TEST(move_py_rewrite_split_keeps_comment_on_old_line);
    RUN_TEST(move_py_rewrite_indented_split_keeps_indent);
    RUN_TEST(move_py_rewrite_ignores_other_modules);
    RUN_TEST(move_py_rewrite_exact_module_match_only);
    RUN_TEST(move_py_rewrite_plain_import_counted_untouched);
    RUN_TEST(move_py_rewrite_star_import_counted_untouched);
    RUN_TEST(move_py_rewrite_parenthesized_counted_untouched);
    RUN_TEST(move_py_rewrite_all_ref_counted_untouched);
    RUN_TEST(move_py_rewrite_crlf_preserved);
    RUN_TEST(move_py_rewrite_symbol_not_in_list_untouched);
    RUN_TEST(move_ts_rewrite_single_name);
    RUN_TEST(move_ts_rewrite_preserves_quote_and_no_semicolon);
    RUN_TEST(move_ts_rewrite_multi_name_split);
    RUN_TEST(move_ts_rewrite_split_keeps_padding_and_order);
    RUN_TEST(move_ts_rewrite_alias_kept);
    RUN_TEST(move_ts_rewrite_import_type_preserved);
    RUN_TEST(move_ts_rewrite_inline_type_entry_preserved);
    RUN_TEST(move_ts_rewrite_ignores_other_specs);
    RUN_TEST(move_ts_rewrite_default_import_counted_untouched);
    RUN_TEST(move_ts_rewrite_namespace_counted_untouched);
    RUN_TEST(move_ts_rewrite_side_effect_counted_untouched);
    RUN_TEST(move_ts_rewrite_barrel_counted_untouched);
    RUN_TEST(move_ts_rewrite_dynamic_and_require_counted_untouched);
    RUN_TEST(move_ts_rewrite_multiline_counted_untouched);
    RUN_TEST(move_ts_rewrite_crlf_preserved);
    RUN_TEST(move_ts_rel_spec_same_dir);
    RUN_TEST(move_ts_rel_spec_child_dir);
    RUN_TEST(move_ts_rel_spec_parent_dir);
    RUN_TEST(move_ts_rel_spec_sibling_deep);
    RUN_TEST(move_ts_rel_spec_root_importer);
    RUN_TEST(move_ts_rel_spec_module_at_root);
    RUN_TEST(move_ts_rel_spec_strips_extensions);
    RUN_TEST(move_ts_rel_spec_index_kept_literal);
}
