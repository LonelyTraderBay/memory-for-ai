/*
 * edit_rename.c — Identifier sweep and splice machinery for rename_symbol.
 *
 * Pure text work, no store access: the MCP layer collects occurrences per
 * file, classifies them into confidence tiers (HIGH / REVIEW / SKIP) using
 * graph knowledge, then calls cbm_edit_rename_in_buffer with an apply mask.
 *
 * Matching is whole-identifier: a position only matches when both neighbours
 * are non-identifier bytes, so "order" never matches inside "reorder" or
 * "order_id".
 */

#include "edit/edit.h"

#include <stdlib.h>
#include <string.h>

static bool is_ident_byte(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool cbm_edit_is_valid_identifier(const char *s) {
    if (!s || !s[0]) {
        return false;
    }
    if (!(s[0] == '_' || (s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) {
        return false;
    }
    for (size_t i = 1; s[i]; i++) {
        if (!is_ident_byte((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

bool cbm_edit_line_looks_like_comment(const char *data, size_t len, size_t line_start) {
    size_t p = line_start;
    while (p < len && (data[p] == ' ' || data[p] == '\t')) {
        p++;
    }
    if (p >= len) {
        return false;
    }
    /* Line comment openers across the indexed languages: C-like, script
     * (#/Python/shell), SQL (--), HTML, and docstring markers. A leading '*'
     * covers the body lines of block comments. */
    if (data[p] == '#' || data[p] == '*') {
        return true;
    }
    if (p + 1 < len && data[p] == '-' && data[p + 1] == '-') {
        return true;
    }
    if (p + 1 < len && data[p] == '/' && (data[p + 1] == '/' || data[p + 1] == '*')) {
        return true;
    }
    if (p + 3 < len && data[p] == '<' && data[p + 1] == '!' && data[p + 2] == '-' &&
        data[p + 3] == '-') {
        return true;
    }
    if (p + 2 < len && ((data[p] == '"' && data[p + 1] == '"' && data[p + 2] == '"') ||
                        (data[p] == '\'' && data[p + 1] == '\'' && data[p + 2] == '\''))) {
        return true;
    }
    return false;
}

int cbm_edit_scan_identifier(const char *data, size_t len, const char *name,
                             cbm_edit_occurrence_t **out, int *count, int cap) {
    if (!data || !name || !name[0] || !out || !count || cap < 1) {
        return CBM_EDIT_ERR_ARGS;
    }
    *out = NULL;
    *count = 0;

    size_t name_len = strlen(name);
    if (name_len > len) {
        return CBM_EDIT_OK; /* zero occurrences */
    }

    cbm_edit_occurrence_t *occs = malloc(sizeof(*occs) * (size_t)cap);
    if (!occs) {
        return CBM_EDIT_ERR_OOM;
    }
    int n = 0;
    int line = 1;
    size_t i = 0;
    while (i + name_len <= len && n < cap) {
        const char *hit = memchr(data + i, name[0], len - i - name_len + 1);
        if (!hit) {
            break;
        }
        size_t pos = (size_t)(hit - data);
        /* Advance the running line counter to the hit position. */
        for (size_t k = i; k < pos; k++) {
            if (data[k] == '\n') {
                line++;
            }
        }
        bool left_ok = pos == 0 || !is_ident_byte((unsigned char)data[pos - 1]);
        bool right_ok =
            pos + name_len >= len || !is_ident_byte((unsigned char)data[pos + name_len]);
        if (left_ok && right_ok && memcmp(data + pos, name, name_len) == 0) {
            occs[n].line = line;
            occs[n].offset = pos;
            n++;
        }
        i = pos + 1;
    }
    if (n == 0) {
        free(occs);
        return CBM_EDIT_OK;
    }
    *out = occs;
    *count = n;
    return CBM_EDIT_OK;
}

int cbm_edit_rename_in_buffer(const char *data, size_t len, const char *old_name,
                              const char *new_name, const cbm_edit_occurrence_t *occs,
                              const bool *apply, int count, char **out_data, size_t *out_len) {
    if (!data || !old_name || !new_name || !occs || !apply || !out_data || !out_len || count < 1) {
        return CBM_EDIT_ERR_ARGS;
    }
    size_t old_len_name = strlen(old_name);
    size_t new_len_name = strlen(new_name);

    int applied = 0;
    for (int i = 0; i < count; i++) {
        if (apply[i]) {
            applied++;
        }
    }
    if (applied == 0) {
        return CBM_EDIT_ERR_ARGS;
    }

    /* Exact output size: every applied occurrence swaps old for new. */
    size_t new_size = len;
    if (new_len_name > old_len_name) {
        new_size = len + (size_t)applied * (new_len_name - old_len_name);
    } else if (old_len_name > new_len_name) {
        size_t shrink = (size_t)applied * (old_len_name - new_len_name);
        new_size = len > shrink ? len - shrink : 0;
    }
    char *buf = malloc(new_size + 1);
    if (!buf) {
        return CBM_EDIT_ERR_OOM;
    }

    size_t src = 0;
    size_t dst = 0;
    for (int i = 0; i < count; i++) {
        if (!apply[i]) {
            continue;
        }
        size_t off = occs[i].offset;
        if (off < src || off + old_len_name > len ||
            memcmp(data + off, old_name, old_len_name) != 0) {
            free(buf);
            return CBM_EDIT_ERR_ARGS; /* unsorted, out-of-range, or mismatched occurrence */
        }
        memcpy(buf + dst, data + src, off - src);
        dst += off - src;
        memcpy(buf + dst, new_name, new_len_name);
        dst += new_len_name;
        src = off + old_len_name;
    }
    memcpy(buf + dst, data + src, len - src);
    dst += len - src;
    buf[dst] = '\0';
    *out_data = buf;
    *out_len = dst;
    return CBM_EDIT_OK;
}
