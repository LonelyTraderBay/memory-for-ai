/*
 * edit_plan.c — Bounded unified-style diff preview for dry-run plans.
 *
 * This is not a general LCS diff: the edit region is known exactly (the graph
 * node's line range), so the hunk is "context + removed region + added
 * content + context". That is all an agent needs to review a planned edit,
 * and it keeps the preview deterministic and cheap.
 */

#include "edit/edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal growing buffer (sticky OOM flag; finish returns NULL on OOM). */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    size_t max; /* hard byte cap; further appends set truncated */
    bool oom;
    bool truncated;
} plan_sb_t;

static void plan_sb_init(plan_sb_t *sb, size_t max) {
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
    sb->max = max;
    sb->oom = false;
    sb->truncated = false;
}

static void plan_sb_append_n(plan_sb_t *sb, const char *s, size_t n) {
    if (sb->oom || sb->truncated) {
        return;
    }
    if (sb->len + n > sb->max) {
        sb->truncated = true;
        return;
    }
    if (sb->len + n + 1 > sb->cap) {
        size_t new_cap = sb->cap ? sb->cap * 2 : 1024;
        while (new_cap < sb->len + n + 1) {
            new_cap *= 2;
        }
        char *nb = realloc(sb->buf, new_cap);
        if (!nb) {
            sb->oom = true;
            return;
        }
        sb->buf = nb;
        sb->cap = new_cap;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void plan_sb_append(plan_sb_t *sb, const char *s) {
    plan_sb_append_n(sb, s, strlen(s));
}

static char *plan_sb_finish(plan_sb_t *sb) {
    if (sb->oom) {
        free(sb->buf);
        return NULL;
    }
    if (sb->truncated) {
        /* The cap was hit mid-hunk: say so instead of looking complete. */
        sb->truncated = false;
        plan_sb_append(sb, "... (diff truncated)\n");
        if (sb->oom) {
            free(sb->buf);
            return NULL;
        }
    }
    if (!sb->buf) {
        sb->buf = malloc(1);
        if (sb->buf) {
            sb->buf[0] = '\0';
        }
    }
    char *out = sb->buf;
    sb->buf = NULL;
    return out;
}

/* Emit one line of `data` starting at byte `offset` (up to and including its
 * newline), prefixed by `prefix`. Returns the offset of the next line. */
static size_t emit_line(plan_sb_t *sb, const char *data, size_t len, size_t offset,
                        const char *prefix) {
    const char *nl = memchr(data + offset, '\n', len - offset);
    size_t end = nl ? (size_t)(nl - data) + 1 : len;
    plan_sb_append(sb, prefix);
    plan_sb_append_n(sb, data + offset, end - offset);
    if (!nl) {
        plan_sb_append(sb, "\n");
    }
    return end;
}

char *cbm_edit_plan_preview(const char *old_data, size_t old_len, int start_line, int end_line,
                            cbm_edit_action_t action, const char *content, size_t content_len,
                            int context_lines, size_t max_bytes) {
    if (!old_data || !content || start_line < 1 || end_line < start_line || max_bytes < 256) {
        return NULL;
    }
    int total_lines = cbm_edit_count_lines(old_data, old_len);
    if (end_line > total_lines) {
        return NULL;
    }
    if (context_lines < 0) {
        context_lines = 0;
    }

    /* Region [r0, r1): same math as edit_surgery_apply. */
    size_t r0 = 0;
    size_t r1 = 0;
    if (action == CBM_EDIT_INSERT_BEFORE) {
        if (cbm_edit_line_offset(old_data, old_len, start_line, &r0) != CBM_EDIT_OK) {
            return NULL;
        }
        r1 = r0;
    } else {
        if (cbm_edit_line_offset(old_data, old_len, start_line, &r0) != CBM_EDIT_OK) {
            return NULL;
        }
        if (end_line == total_lines) {
            r1 = old_len;
        } else if (cbm_edit_line_offset(old_data, old_len, end_line + 1, &r1) != CBM_EDIT_OK) {
            return NULL;
        }
        if (action == CBM_EDIT_INSERT_AFTER) {
            r0 = r1;
        }
    }

    plan_sb_t sb;
    plan_sb_init(&sb, max_bytes);

    int removed = (action == CBM_EDIT_REPLACE_BODY) ? (end_line - start_line + 1) : 0;
    int added = cbm_edit_count_lines(content, content_len);
    char header[128];
    snprintf(header, sizeof(header), "@@ -%d,%d +%d,%d @@\n", start_line, removed, start_line,
             added);
    plan_sb_append(&sb, header);

    /* Context before: walk back up to context_lines line starts from r0. */
    size_t ctx_start = r0;
    for (int i = 0; i < context_lines && ctx_start > 0; i++) {
        /* Find the start of the line preceding ctx_start. */
        size_t p = ctx_start;
        /* Step over the newline that ends the previous line. */
        if (p >= 1 && old_data[p - 1] == '\n') {
            p--;
        }
        if (p >= 1 && old_data[p - 1] == '\r') {
            p--;
        }
        while (p > 0 && old_data[p - 1] != '\n') {
            p--;
        }
        ctx_start = p;
    }
    for (size_t off = ctx_start; off < r0;) {
        off = emit_line(&sb, old_data, old_len, off, "  ");
    }

    /* Removed region. */
    for (size_t off = r0; off < r1;) {
        off = emit_line(&sb, old_data, old_len, off, "- ");
    }

    /* Added content (verbatim; EOL normalization happens at surgery time and
     * is irrelevant for review). */
    for (size_t off = 0; off < content_len;) {
        off = emit_line(&sb, content, content_len, off, "+ ");
    }

    /* Context after. */
    size_t off = r1;
    for (int i = 0; i < context_lines && off < old_len; i++) {
        off = emit_line(&sb, old_data, old_len, off, "  ");
    }

    return plan_sb_finish(&sb);
}
