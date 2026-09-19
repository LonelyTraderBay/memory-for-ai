/*
 * edit.h — Symbol-level source editing on top of the knowledge graph (Phase 1).
 *
 * The graph knows where every symbol lives (cbm_node_t.file_path + start/end
 * line). This module turns that knowledge into safe, evidence-carrying edits:
 *
 *   resolve  (edit_resolve.c) — qualified_name → graph node
 *   surgery  (edit_surgery.c) — pure in-memory line-range text surgery
 *   plan     (edit_plan.c)    — bounded unified-style diff preview (dry-run)
 *   write    (edit_write.c)   — mtime guard, backup, atomic temp+rename write
 *
 * Safety contract (see docs/THIET-KE-EDIT-TOOLS.md):
 *   - dry-run first: the MCP layer plans by default and writes only on
 *     dry_run=false;
 *   - verify-before-write: the node's line range must still match the on-disk
 *     source (stale index is rejected, not edited around);
 *   - optimistic concurrency: the file's mtime/size observed at read time must
 *     still hold at write time, else the write is refused;
 *   - atomic write: temp file in the same directory + rename-replace, with a
 *     backup copy of the original kept for undo.
 *
 * Phase-1 scope note: replace_body replaces the node's FULL line range
 * (signature + body); `content` is the complete new definition. Signature-
 * preserving body-only replacement needs per-language body-start detection
 * and is intentionally left to a later phase.
 */
#ifndef CBM_EDIT_H
#define CBM_EDIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "store/store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Actions ────────────────────────────────────────────────────── */

typedef enum {
    CBM_EDIT_REPLACE_BODY = 0, /* replace the node's full line range */
    CBM_EDIT_INSERT_BEFORE,    /* insert content directly above the node */
    CBM_EDIT_INSERT_AFTER,     /* insert content directly below the node */
} cbm_edit_action_t;

const char *cbm_edit_action_name(cbm_edit_action_t action);
bool cbm_edit_action_parse(const char *s, cbm_edit_action_t *out);

/* ── Result codes ───────────────────────────────────────────────── */

enum {
    CBM_EDIT_OK = 0,
    CBM_EDIT_ERR_ARGS = -1,   /* invalid arguments */
    CBM_EDIT_ERR_OOM = -2,    /* allocation failure */
    CBM_EDIT_ERR_IO = -3,     /* filesystem read/write failure */
    CBM_EDIT_ERR_RANGE = -4,  /* line range invalid for the file (stale index) */
    CBM_EDIT_ERR_VERIFY = -5, /* range no longer matches source (stale index) */
    CBM_EDIT_ERR_MTIME = -6,  /* file changed externally between read and write */
};

/* ── Resolve (edit_resolve.c) ───────────────────────────────────── */

typedef enum {
    CBM_EDIT_RESOLVE_OK = 0,
    CBM_EDIT_RESOLVE_NOT_FOUND,
    CBM_EDIT_RESOLVE_AMBIGUOUS, /* candidates_out holds the matches */
    CBM_EDIT_RESOLVE_ERROR,
} cbm_edit_resolve_status_t;

/* Resolve a qualified_name to exactly one node. Tier 1: exact qn. Tier 2:
 * unique qn-suffix match (same convention as get_code_snippet). On AMBIGUOUS,
 * *candidates_out receives the suffix matches (free with cbm_store_free_nodes)
 * and *candidate_count their number. On OK, *out_node owns its strings — free
 * with cbm_edit_free_node. */
cbm_edit_resolve_status_t cbm_edit_resolve_symbol(cbm_store_t *store, const char *project,
                                                  const char *qn, cbm_node_t *out_node,
                                                  cbm_node_t **candidates_out,
                                                  int *candidate_count);

/* Free the heap strings inside a node produced by cbm_edit_resolve_symbol
 * (does NOT free the node struct itself). */
void cbm_edit_free_node(cbm_node_t *node);

/* Node labels Phase 1 is allowed to edit. Containers of namespace scope
 * (Module/File/Folder/Package) and non-source nodes (TestSuite, BuildTarget,
 * Resource, ...) are rejected. */
bool cbm_edit_label_editable(const char *label);

/* ── Surgery (edit_surgery.c) ───────────────────────────────────── */

typedef struct {
    int start_line;    /* 1-based inclusive start of the affected region */
    int end_line;      /* 1-based inclusive end of the affected region */
    int lines_removed; /* lines taken out (replace only) */
    int lines_added;   /* lines put in */
} cbm_edit_surgery_stats_t;

/* Apply an action to a file's contents in memory. Lines are 1-based and
 * inclusive, taken from the graph node. The file's dominant line ending is
 * detected and `content` is normalized to it; a missing trailing newline on
 * `content` is added (whole-line splicing never leaves a half line).
 *
 * On CBM_EDIT_OK, *out_data is a malloc'd buffer (caller frees) holding the
 * new file contents. All other returns leave outputs untouched. */
int cbm_edit_surgery_apply(const char *old_data, size_t old_len, int start_line, int end_line,
                           cbm_edit_action_t action, const char *content, size_t content_len,
                           char **out_data, size_t *out_len, cbm_edit_surgery_stats_t *out_stats);

/* Number of logical lines in a buffer (a final line without a trailing
 * newline still counts; an empty buffer has 0 lines). */
int cbm_edit_count_lines(const char *data, size_t len);

/* Byte offset of the start of 1-based `line` in `data`. Returns CBM_EDIT_OK,
 * or CBM_EDIT_ERR_RANGE when the file has fewer than `line` lines. */
int cbm_edit_line_offset(const char *data, size_t len, int line, size_t *out_offset);

/* Delete the 1-based inclusive line range [start_line, end_line]. Also
 * absorbs ONE adjacent blank line so deletion never leaves double blank
 * lines: the blank line right after the region when there is one, else the
 * blank line right before it when the region ends at EOF.
 * On CBM_EDIT_OK, *out_data is a malloc'd buffer (caller frees). */
int cbm_edit_surgery_delete(const char *old_data, size_t old_len, int start_line, int end_line,
                            char **out_data, size_t *out_len, cbm_edit_surgery_stats_t *out_stats);

/* ── Plan preview (edit_plan.c) ─────────────────────────────────── */

/* Bounded unified-style diff hunk for a planned edit: up to `context_lines`
 * of surrounding context, '-' for removed region lines, '+' for the new
 * content. Output is capped at `max_bytes` and ends with a truncation marker
 * when cut. Returns a malloc'd string (caller frees), NULL on OOM/bad args. */
char *cbm_edit_plan_preview(const char *old_data, size_t old_len, int start_line, int end_line,
                            cbm_edit_action_t action, const char *content, size_t content_len,
                            int context_lines, size_t max_bytes);

/* ── Rename support (edit_rename.c) ───────────────────────────── */

typedef struct {
    int line;      /* 1-based line of the match */
    size_t offset; /* byte offset of the match start */
} cbm_edit_occurrence_t;

/* Identifier rules for rename targets: [A-Za-z_][A-Za-z0-9_]* — matches the
 * intersection of every indexed language's identifier grammar (Unicode
 * identifiers are deliberately out of scope for Phase 3). */
bool cbm_edit_is_valid_identifier(const char *s);

/* Find whole-identifier occurrences of `name` in `data` — a match whose
 * neighbours are NOT identifier characters (so "order" never matches inside
 * "reorder"). Results come sorted by offset; at most `cap` are collected.
 * On CBM_EDIT_OK, *out is a malloc'd array (caller frees). */
int cbm_edit_scan_identifier(const char *data, size_t len, const char *name,
                             cbm_edit_occurrence_t **out, int *count, int cap);

/* Cheap comment-line heuristic for the SKIP tier: the line's first
 * non-whitespace characters start a comment (double slash, #, --, slash-star,
 * star, <!--, triple quotes). Language-agnostic by design — it only needs to
 * be conservative. */
bool cbm_edit_line_looks_like_comment(const char *data, size_t len, size_t line_start);

/* Build a new buffer with `new_name` spliced over every occurrence whose
 * apply[] entry is true (occs sorted by offset, as produced by
 * cbm_edit_scan_identifier; each must still point at `old_name` in `data`).
 * On CBM_EDIT_OK, *out_data is malloc'd. */
int cbm_edit_rename_in_buffer(const char *data, size_t len, const char *old_name,
                              const char *new_name, const cbm_edit_occurrence_t *occs,
                              const bool *apply, int count, char **out_data, size_t *out_len);

/* ── Move support (edit_move.c) — Phase 5 ─────────────────────── */

/* Copy the 1-based inclusive line range [start_line, end_line] out of `data`
 * (the symbol's definition text, verbatim). On CBM_EDIT_OK, *out_data is a
 * malloc'd buffer (caller frees). */
int cbm_edit_move_extract_lines(const char *data, size_t len, int start_line, int end_line,
                                char **out_data, size_t *out_len);

/* Append `def` at the end of a destination file: the file's dominant line
 * ending is detected and `def` is normalized to it; an unterminated final
 * line is terminated first; exactly one blank separator line is inserted
 * unless the file already ends with a blank line (or is empty).
 * On CBM_EDIT_OK, *out_data is a malloc'd buffer (caller frees). */
int cbm_edit_move_append_definition(const char *data, size_t len, const char *def, size_t def_len,
                                    char **out_data, size_t *out_len);

/* Rewrite tallies for cbm_edit_move_rewrite_python_imports. The *_refs /
 * *_skipped counters are occurrences LEFT UNTOUCHED — the MCP layer lists
 * them in the REVIEW tier of the move plan. */
typedef struct {
    int import_lines_rewritten; /* from-imports repointed at the new module */
    int import_lines_split;     /* multi-name lines split old/new */
    int aliases_kept;           /* rewritten lines keeping an `as` alias */
    int plain_import_refs;      /* `import mod_a` lines (untouched) */
    int star_import_refs;       /* `from mod_a import *` lines (untouched) */
    int parenthesized_skipped;  /* parenthesized/unparsable imports (untouched) */
    int all_refs;               /* `__all__` lines mentioning the symbol (untouched) */
} cbm_edit_move_py_stats_t;

/* Rewrite Python from-import lines that import `symbol` from `old_module` so
 * they import it from `new_module` instead. Single-name lines are repointed
 * in place (alias and trailing comment preserved); multi-name lines are
 * split: the remaining names stay on the old-module line and a new
 * `from new_module import symbol` line is inserted directly after it with
 * the same indentation. Module matching is an exact dotted-name comparison.
 * Output is a malloc'd full-file buffer (caller frees); counters are
 * reported via `stats` when non-NULL. */
int cbm_edit_move_rewrite_python_imports(const char *data, size_t len, const char *old_module,
                                         const char *new_module, const char *symbol,
                                         char **out_data, size_t *out_len,
                                         cbm_edit_move_py_stats_t *stats);

/* Rewrite tallies for cbm_edit_move_rewrite_ts_imports. The *_refs /
 * *_skipped counters are occurrences LEFT UNTOUCHED (REVIEW tier). */
typedef struct {
    int import_lines_rewritten; /* named imports repointed at the new spec */
    int import_lines_split;     /* multi-name lines split old/new */
    int aliases_kept;           /* rewritten lines keeping an `as` alias */
    int default_import_refs;    /* default / default+named imports (untouched) */
    int namespace_import_refs;  /* `import * as m` (untouched) */
    int side_effect_refs;       /* `import "spec"` (untouched) */
    int barrel_refs;            /* `export ... from "spec"` (untouched) */
    int dynamic_import_refs;    /* `import("spec")` / `require("spec")` (untouched) */
    int multiline_skipped;      /* named imports spanning lines (untouched) */
} cbm_edit_move_ts_stats_t;

/* Rewrite TypeScript/JavaScript named imports of `symbol` whose module
 * specifier is exactly `old_spec` so they import from `new_spec` instead.
 * `old_spec`/`new_spec` are the literal specifier strings as written in the
 * importing file (the MCP layer computes them per importer with
 * cbm_edit_move_ts_relative_spec). Single-name lines are repointed in place
 * (quote style, semicolon, alias, `import type` all preserved); multi-name
 * lines are split: remaining names stay on the old-spec line and a new
 * import line for the moved symbol is inserted directly after it with the
 * same indentation, quote style, padding, and semicolon. Output is a
 * malloc'd full-file buffer (caller frees); counters via `stats`. */
int cbm_edit_move_rewrite_ts_imports(const char *data, size_t len, const char *old_spec,
                                     const char *new_spec, const char *symbol, char **out_data,
                                     size_t *out_len, cbm_edit_move_ts_stats_t *stats);

/* Compute the TS module specifier for `module_rel` as seen from
 * `importer_rel` (both project-relative POSIX paths): the relative path from
 * the importer's directory to the module, without the module's extension,
 * prefixed "./" when the module is in the same directory or below. Writes a
 * NUL-terminated string to `out` (CBM_EDIT_SZ-spec sized by the caller).
 * Examples: ("src/app/main.ts","src/utils.ts") -> "../utils";
 * ("main.ts","src/mod.ts") -> "./src/mod". */
int cbm_edit_move_ts_relative_spec(const char *importer_rel, const char *module_rel, char *out,
                                   size_t out_sz);

/* ── File state / atomic write (edit_write.c) ───────────────────── */

typedef struct {
    int64_t mtime_ns;
    int64_t size;
} cbm_edit_file_state_t;

/* stat() a file into a comparable state token. CBM_EDIT_OK or CBM_EDIT_ERR_IO. */
int cbm_edit_file_stat(const char *abs_path, cbm_edit_file_state_t *out);

/* Read an entire file. *out_data is malloc'd (caller frees), NUL-terminated
 * for convenience (binary content is still length-delimited). */
int cbm_edit_read_file(const char *abs_path, char **out_data, size_t *out_len);

/* Atomically replace a file's contents:
 *   1. when `expected` is non-NULL, the file's current mtime/size must match
 *      it exactly, else CBM_EDIT_ERR_MTIME (nothing is written);
 *   2. the original file is copied to `backup_dir` (created when missing)
 *      before anything is replaced; the backup path is written to
 *      backup_path_out when non-NULL;
 *   3. the new contents go to a temp file in the same directory and are
 *      rename-replaced over the target (atomic on POSIX and Windows).
 * `backup_dir` may be NULL to skip the backup (not recommended). */
int cbm_edit_write_atomic(const char *abs_path, const char *data, size_t len,
                          const cbm_edit_file_state_t *expected, const char *backup_dir,
                          char *backup_path_out, size_t backup_path_sz);

/* Find the most recent backup of `basename` in `backup_dir` (backup names
 * follow bk_<epoch>_<pid>_<basename>, written by cbm_edit_write_atomic).
 * On CBM_EDIT_OK the full path is written to `out`; CBM_EDIT_ERR_RANGE when
 * no backup matches; CBM_EDIT_ERR_IO when the directory cannot be read. */
int cbm_edit_latest_backup(const char *backup_dir, const char *basename, char *out, size_t out_sz);

#if defined(CBM_EDIT_TEST_API) && CBM_EDIT_TEST_API
/* One-shot fault injection for the write path (test builds only — the
 * production build has neither this hook nor a branch at the write entry,
 * mirroring CBM_INCREMENTAL_TEST_API). cbm_edit_write_test_fail_once arms
 * the NEXT cbm_edit_write_atomic call to return CBM_EDIT_ERR_IO before
 * touching anything, so tests can exercise partial multi-file failure and
 * verify no file is left corrupt. */
void cbm_edit_write_test_fail_once(void);
void cbm_edit_write_test_reset_faults(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* CBM_EDIT_H */
