/*
 * edit_surgery.c — Pure in-memory line-range text surgery.
 *
 * No filesystem, no store, no globals: buffers in, buffer out. All ranges are
 * 1-based inclusive line numbers taken from graph nodes. The output buffer is
 * always a complete file image ready for an atomic write.
 */

#include "edit/edit.h"

#include <stdlib.h>
#include <string.h>

int cbm_edit_count_lines(const char *data, size_t len) {
    if (!data || len == 0) {
        return 0;
    }
    int lines = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            lines++;
        }
    }
    if (data[len - 1] != '\n') {
        lines++;
    }
    return lines;
}

int cbm_edit_line_offset(const char *data, size_t len, int line, size_t *out_offset) {
    if (!data || !out_offset || line < 1) {
        return CBM_EDIT_ERR_ARGS;
    }
    size_t offset = 0;
    int current = 1;
    while (current < line) {
        const char *nl = memchr(data + offset, '\n', len - offset);
        if (!nl) {
            return CBM_EDIT_ERR_RANGE;
        }
        offset = (size_t)(nl - data) + 1;
        current++;
    }
    *out_offset = offset;
    return CBM_EDIT_OK;
}

/* Dominant line ending of the file: CRLF when the first newline is preceded
 * by a CR, else LF. Files without any newline are treated as LF. */
static bool file_uses_crlf(const char *data, size_t len) {
    const char *nl = memchr(data, '\n', len);
    return nl && nl != data && nl[-1] == '\r';
}

/* Normalize `content` to the file's line ending and guarantee a trailing
 * newline (whole-line splicing never leaves a half line). Returns a malloc'd
 * buffer, NULL on OOM. */
static char *normalize_content(const char *content, size_t content_len, bool crlf,
                               size_t *out_len) {
    /* Worst case: every LF becomes CRLF (+1 byte each), plus trailing LF. */
    size_t lf_count = 0;
    for (size_t i = 0; i < content_len; i++) {
        if (content[i] == '\n') {
            lf_count++;
        }
    }
    size_t cap = content_len + (crlf ? lf_count : 0) + 2;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }

    size_t n = 0;
    for (size_t i = 0; i < content_len; i++) {
        char c = content[i];
        if (crlf) {
            if (c == '\n' && (i == 0 || content[i - 1] != '\r')) {
                buf[n++] = '\r';
            }
            buf[n++] = c;
        } else {
            /* LF file: strip CRs that precede a LF so pasted CRLF content
             * does not smuggle mixed endings into an LF file. */
            if (c == '\r' && i + 1 < content_len && content[i + 1] == '\n') {
                continue;
            }
            buf[n++] = c;
        }
    }
    if (n == 0 || buf[n - 1] != '\n') {
        if (crlf) {
            buf[n++] = '\r';
        }
        buf[n++] = '\n';
    }
    *out_len = n;
    return buf;
}

int cbm_edit_surgery_delete(const char *old_data, size_t old_len, int start_line, int end_line,
                            char **out_data, size_t *out_len, cbm_edit_surgery_stats_t *out_stats) {
    if (!old_data || !out_data || !out_len || start_line < 1 || end_line < start_line) {
        return CBM_EDIT_ERR_ARGS;
    }
    int total_lines = cbm_edit_count_lines(old_data, old_len);
    if (end_line > total_lines) {
        return CBM_EDIT_ERR_RANGE;
    }

    size_t r0 = 0;
    size_t r1 = 0;
    if (cbm_edit_line_offset(old_data, old_len, start_line, &r0) != CBM_EDIT_OK) {
        return CBM_EDIT_ERR_RANGE;
    }
    if (end_line == total_lines) {
        r1 = old_len;
    } else if (cbm_edit_line_offset(old_data, old_len, end_line + 1, &r1) != CBM_EDIT_OK) {
        return CBM_EDIT_ERR_RANGE;
    }

    /* Absorb ONE adjacent blank line so the deletion never leaves a double
     * blank line behind: prefer the blank line right after the region; when
     * the region ends at EOF, take the blank line right before it instead. */
    if (r1 < old_len) {
        size_t p = r1;
        if (p < old_len && old_data[p] == '\r') {
            p++;
        }
        if (p < old_len && old_data[p] == '\n') {
            r1 = p + 1; /* following line is blank — swallow it */
        }
    } else if (r0 > 0) {
        /* Region ends at EOF. Walk back to the start of the preceding line
         * and swallow it when it is blank (empty or a lone CR). */
        size_t p = r0;
        if (p > 0 && old_data[p - 1] == '\n') {
            p--;
        }
        if (p > 0 && old_data[p - 1] == '\r') {
            p--;
        }
        size_t prev_end = p;
        while (p > 0 && old_data[p - 1] != '\n') {
            p--;
        }
        /* [p, prev_end) is the previous line's content. */
        if (prev_end == p && r0 > prev_end) {
            r0 = p; /* previous line is blank — swallow it (keep its newline's predecessor) */
        }
    }

    size_t new_len = r0 + (old_len - r1);
    char *new_data = malloc(new_len + 1);
    if (!new_data) {
        return CBM_EDIT_ERR_OOM;
    }
    memcpy(new_data, old_data, r0);
    memcpy(new_data + r0, old_data + r1, old_len - r1);
    new_data[new_len] = '\0';

    if (out_stats) {
        out_stats->start_line = start_line;
        out_stats->end_line = end_line;
        out_stats->lines_removed = end_line - start_line + 1;
        out_stats->lines_added = 0;
    }
    *out_data = new_data;
    *out_len = new_len;
    return CBM_EDIT_OK;
}

int cbm_edit_surgery_apply(const char *old_data, size_t old_len, int start_line, int end_line,
                           cbm_edit_action_t action, const char *content, size_t content_len,
                           char **out_data, size_t *out_len, cbm_edit_surgery_stats_t *out_stats) {
    if (!old_data || !content || !out_data || !out_len || start_line < 1 || end_line < 1 ||
        end_line < start_line) {
        return CBM_EDIT_ERR_ARGS;
    }

    int total_lines = cbm_edit_count_lines(old_data, old_len);
    if (end_line > total_lines) {
        return CBM_EDIT_ERR_RANGE;
    }

    /* Affected byte region [r0, r1). For inserts the region is empty and sits
     * exactly at the insertion point. */
    size_t r0 = 0;
    size_t r1 = 0;
    if (action == CBM_EDIT_INSERT_BEFORE) {
        if (cbm_edit_line_offset(old_data, old_len, start_line, &r0) != CBM_EDIT_OK) {
            return CBM_EDIT_ERR_RANGE;
        }
        r1 = r0;
    } else {
        if (cbm_edit_line_offset(old_data, old_len, start_line, &r0) != CBM_EDIT_OK) {
            return CBM_EDIT_ERR_RANGE;
        }
        if (end_line == total_lines) {
            r1 = old_len;
        } else if (cbm_edit_line_offset(old_data, old_len, end_line + 1, &r1) != CBM_EDIT_OK) {
            return CBM_EDIT_ERR_RANGE;
        }
        if (action == CBM_EDIT_INSERT_AFTER) {
            r0 = r1;
        }
    }

    bool crlf = file_uses_crlf(old_data, old_len);
    size_t norm_len = 0;
    char *norm = normalize_content(content, content_len, crlf, &norm_len);
    if (!norm) {
        return CBM_EDIT_ERR_OOM;
    }

    /* Inserting right after an unterminated final line: the previous line
     * must be terminated first, else content glues onto it. */
    size_t prefix_len = 0;
    const char *prefix = "";
    if (r0 > 0 && r0 == r1 && old_data[r0 - 1] != '\n') {
        prefix = crlf ? "\r\n" : "\n";
        prefix_len = crlf ? 2 : 1;
    }

    size_t new_len = r0 + prefix_len + norm_len + (old_len - r1);
    char *new_data = malloc(new_len + 1);
    if (!new_data) {
        free(norm);
        return CBM_EDIT_ERR_OOM;
    }
    memcpy(new_data, old_data, r0);
    memcpy(new_data + r0, prefix, prefix_len);
    memcpy(new_data + r0 + prefix_len, norm, norm_len);
    memcpy(new_data + r0 + prefix_len + norm_len, old_data + r1, old_len - r1);
    new_data[new_len] = '\0';
    free(norm);

    if (out_stats) {
        out_stats->start_line = start_line;
        out_stats->end_line = end_line;
        out_stats->lines_removed = (action == CBM_EDIT_REPLACE_BODY) ? (end_line - start_line + 1)
                                                                     : 0;
        out_stats->lines_added = cbm_edit_count_lines(new_data + r0 + prefix_len, norm_len);
    }
    *out_data = new_data;
    *out_len = new_len;
    return CBM_EDIT_OK;
}
