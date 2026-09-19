/*
 * edit_move.c — Pure text machinery for move_symbol (Phase 5).
 *
 * No store, no filesystem: buffers in, buffer out. The MCP layer supplies the
 * graph knowledge (which file is source/destination, which modules import the
 * symbol); this module does the deterministic text work:
 *
 *   extract  — copy a symbol's line range out of the source file
 *   append   — splice the definition at the end of the destination file with
 *              exactly one blank separator line, EOL-normalized
 *   rewrite  — update Python from-imports that referenced the old module
 *
 * Python rewrite scope (docs/THIET-KE-EDIT-TOOLS.md §12.3):
 *   - `from mod_a import f`            -> `from mod_b import f`
 *   - `from mod_a import f, g`         -> split: keep `from mod_a import g`,
 *                                       insert `from mod_b import f` after
 *   - `from mod_a import f as f1`      -> `from mod_b import f as f1`
 *     (alias kept; counted as aliases_kept so the MCP layer can tier REVIEW)
 *   - `import mod_a`, `import mod_a as m`, star imports, __all__ mentions and
 *     parenthesized multi-line imports are NOT touched — only counted so the
 *     caller can list them in the REVIEW tier.
 *
 * Module matching is an exact dotted-name comparison (`mod_a` never matches
 * `mod_a.sub` or `pkg.mod_a`); relative imports (`from .mod_a import f`) are
 * left untouched by design.
 */

#include "edit/edit.h"

#include <stdlib.h>
#include <string.h>

/* ── small EOL helpers (mirrors edit_surgery.c statics) ─────────── */

static bool mv_file_uses_crlf(const char *data, size_t len) {
    const char *nl = memchr(data, '\n', len);
    return nl && nl != data && nl[-1] == '\r';
}

/* Normalize `content` to the target line ending and guarantee a trailing
 * newline. Returns a malloc'd buffer, NULL on OOM. */
static char *mv_normalize(const char *content, size_t content_len, bool crlf, size_t *out_len) {
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

/* True when the buffer already ends with a blank line (the last line is
 * terminated and the line before it is empty). */
static bool mv_ends_with_blank_line(const char *data, size_t len) {
    if (len < 2 || data[len - 1] != '\n') {
        return false;
    }
    size_t p = len - 1; /* on the final '\n' */
    if (p > 0 && data[p - 1] == '\r') {
        p--;
    }
    return p > 0 && data[p - 1] == '\n';
}

/* ── growing buffer ─────────────────────────────────────────────── */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool oom;
} mv_sb_t;

static void mv_sb_append(mv_sb_t *sb, const char *s, size_t n) {
    if (sb->oom || n == 0) {
        return;
    }
    if (sb->len + n + 1 > sb->cap) {
        size_t ncap = sb->cap ? sb->cap * 2 : 256;
        while (ncap < sb->len + n + 1) {
            ncap *= 2;
        }
        char *nb = realloc(sb->buf, ncap);
        if (!nb) {
            sb->oom = true;
            return;
        }
        sb->buf = nb;
        sb->cap = ncap;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
}

static void mv_sb_append_str(mv_sb_t *sb, const char *s) {
    mv_sb_append(sb, s, strlen(s));
}

/* ── extract / append ───────────────────────────────────────────── */

int cbm_edit_move_extract_lines(const char *data, size_t len, int start_line, int end_line,
                                char **out_data, size_t *out_len) {
    if (!data || !out_data || !out_len || start_line < 1 || end_line < start_line) {
        return CBM_EDIT_ERR_ARGS;
    }
    int total = cbm_edit_count_lines(data, len);
    if (end_line > total) {
        return CBM_EDIT_ERR_RANGE;
    }
    size_t r0 = 0;
    size_t r1 = 0;
    if (cbm_edit_line_offset(data, len, start_line, &r0) != CBM_EDIT_OK) {
        return CBM_EDIT_ERR_RANGE;
    }
    if (end_line == total) {
        r1 = len;
    } else if (cbm_edit_line_offset(data, len, end_line + 1, &r1) != CBM_EDIT_OK) {
        return CBM_EDIT_ERR_RANGE;
    }
    char *buf = malloc(r1 - r0 + 1);
    if (!buf) {
        return CBM_EDIT_ERR_OOM;
    }
    memcpy(buf, data + r0, r1 - r0);
    buf[r1 - r0] = '\0';
    *out_data = buf;
    *out_len = r1 - r0;
    return CBM_EDIT_OK;
}

int cbm_edit_move_append_definition(const char *data, size_t len, const char *def, size_t def_len,
                                    char **out_data, size_t *out_len) {
    if (!data || !def || !out_data || !out_len || def_len == 0) {
        return CBM_EDIT_ERR_ARGS;
    }
    bool crlf = mv_file_uses_crlf(data, len);
    size_t norm_len = 0;
    char *norm = mv_normalize(def, def_len, crlf, &norm_len);
    if (!norm) {
        return CBM_EDIT_ERR_OOM;
    }
    if (len == 0) {
        *out_data = norm;
        *out_len = norm_len;
        return CBM_EDIT_OK;
    }

    const char *eol = crlf ? "\r\n" : "\n";
    size_t eol_len = crlf ? 2 : 1;
    bool need_term = data[len - 1] != '\n';
    bool need_blank = !need_term && !mv_ends_with_blank_line(data, len);
    /* Unterminated file: one EOL to terminate the last line plus one EOL for
     * the separator blank line. */
    size_t sep_len = need_term ? 2 * eol_len : (need_blank ? eol_len : 0);

    size_t new_len = len + sep_len + norm_len;
    char *buf = malloc(new_len + 1);
    if (!buf) {
        free(norm);
        return CBM_EDIT_ERR_OOM;
    }
    size_t n = 0;
    memcpy(buf, data, len);
    n += len;
    if (need_term) {
        /* Unterminated final line: terminate it, then the separator blank. */
        memcpy(buf + n, eol, eol_len);
        n += eol_len;
        memcpy(buf + n, eol, eol_len);
        n += eol_len;
    } else if (need_blank) {
        memcpy(buf + n, eol, eol_len);
        n += eol_len;
    }
    memcpy(buf + n, norm, norm_len);
    n += norm_len;
    buf[n] = '\0';
    free(norm);
    *out_data = buf;
    *out_len = n;
    return CBM_EDIT_OK;
}

/* ── Python import rewriting ────────────────────────────────────── */

static bool mv_is_ident_byte(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* One parsed entry of a from-import name list: `name` or `name as alias`. */
typedef struct {
    size_t name_off;
    size_t name_len;
    size_t alias_off; /* valid only when has_alias */
    size_t alias_len;
    bool has_alias;
} mv_import_entry_t;

/* Parse the names region [s, s+n) of a from-import line into entries.
 * Returns entry count (>=1), 0 when the region is not a plain comma list
 * (parenthesized, star, or empty), -1 on OOM. *out is malloc'd on success. */
static int mv_parse_import_entries(const char *s, size_t n, mv_import_entry_t **out) {
    *out = NULL;
    /* Trim outer whitespace. */
    while (n > 0 && (s[0] == ' ' || s[0] == '\t')) {
        s++;
        n--;
    }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        n--;
    }
    if (n == 0 || s[0] == '(' || s[0] == '*') {
        return 0;
    }

    int cap = 4;
    int count = 0;
    mv_import_entry_t *entries = malloc(sizeof(*entries) * (size_t)cap);
    if (!entries) {
        return -1;
    }
    size_t i = 0;
    while (i <= n) {
        /* entry spans [i, j) up to the next comma (or end) */
        size_t j = i;
        while (j < n && s[j] != ',') {
            j++;
        }
        size_t a = i;
        size_t b = j;
        while (a < b && (s[a] == ' ' || s[a] == '\t')) {
            a++;
        }
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) {
            b--;
        }
        if (a < b) {
            /* split "name" / "name as alias" */
            size_t w1 = a;
            while (w1 < b && mv_is_ident_byte((unsigned char)s[w1])) {
                w1++;
            }
            if (w1 == a) {
                free(entries); /* not an identifier — give up on this line */
                return 0;
            }
            if (count == cap) {
                cap *= 2;
                mv_import_entry_t *ne = realloc(entries, sizeof(*entries) * (size_t)cap);
                if (!ne) {
                    free(entries);
                    return -1;
                }
                entries = ne;
            }
            mv_import_entry_t *e = &entries[count++];
            e->name_off = a;
            e->name_len = w1 - a;
            e->has_alias = false;
            e->alias_off = 0;
            e->alias_len = 0;
            size_t p = w1;
            while (p < b && (s[p] == ' ' || s[p] == '\t')) {
                p++;
            }
            if (p + 2 <= b && s[p] == 'a' && s[p + 1] == 's' &&
                (p + 2 == b || s[p + 2] == ' ' || s[p + 2] == '\t')) {
                p += 2;
                while (p < b && (s[p] == ' ' || s[p] == '\t')) {
                    p++;
                }
                size_t al = p;
                while (al < b && mv_is_ident_byte((unsigned char)s[al])) {
                    al++;
                }
                if (al > p && al == b) {
                    e->has_alias = true;
                    e->alias_off = p;
                    e->alias_len = al - p;
                }
            }
        }
        if (j >= n) {
            break;
        }
        i = j + 1;
    }
    if (count == 0) {
        free(entries);
        return 0;
    }
    *out = entries;
    return count;
}

/* Append one normalized entry text (`name` or `name as alias`). */
static void mv_sb_append_entry(mv_sb_t *sb, const char *line, const mv_import_entry_t *e) {
    mv_sb_append(sb, line + e->name_off, e->name_len);
    if (e->has_alias) {
        mv_sb_append_str(sb, " as ");
        mv_sb_append(sb, line + e->alias_off, e->alias_len);
    }
}

/* True when the line content [ls, ce) contains `__all__` and a quoted
 * occurrence of `symbol` ('sym' or "sym"). */
static bool mv_line_is_all_ref(const char *data, size_t ls, size_t ce, const char *symbol) {
    static const char all[] = "__all__";
    size_t sym_len = strlen(symbol);
    bool has_all = false;
    for (size_t i = ls; i + sizeof(all) - 1 <= ce; i++) {
        if (memcmp(data + i, all, sizeof(all) - 1) == 0) {
            has_all = true;
            break;
        }
    }
    if (!has_all) {
        return false;
    }
    for (size_t i = ls; i + sym_len + 2 <= ce; i++) {
        if ((data[i] == '\'' || data[i] == '"') && memcmp(data + i + 1, symbol, sym_len) == 0 &&
            data[i + 1 + sym_len] == data[i]) {
            return true;
        }
    }
    return false;
}

/* True when the line is a plain `import <module>` (optionally `as x`, and
 * optionally inside a comma list `import os, mod_a`). */
static bool mv_line_plain_imports_module(const char *data, size_t ls, size_t ce, const char *mod,
                                         size_t mod_len) {
    size_t p = ls;
    while (p < ce && (data[p] == ' ' || data[p] == '\t')) {
        p++;
    }
    if (p + 6 >= ce || memcmp(data + p, "import", 6) != 0 ||
        (data[p + 6] != ' ' && data[p + 6] != '\t')) {
        return false;
    }
    p += 6;
    /* walk the comma list */
    while (p < ce) {
        while (p < ce && (data[p] == ' ' || data[p] == '\t')) {
            p++;
        }
        size_t tok = p;
        while (tok < ce && (mv_is_ident_byte((unsigned char)data[tok]) || data[tok] == '.')) {
            tok++;
        }
        if (tok - p == (size_t)mod_len && memcmp(data + p, mod, mod_len) == 0) {
            return true;
        }
        /* skip optional `as alias` */
        p = tok;
        while (p < ce && data[p] != ',') {
            p++;
        }
        if (p < ce && data[p] == ',') {
            p++;
        }
    }
    return false;
}

int cbm_edit_move_rewrite_python_imports(const char *data, size_t len, const char *old_module,
                                         const char *new_module, const char *symbol,
                                         char **out_data, size_t *out_len,
                                         cbm_edit_move_py_stats_t *stats) {
    if (!data || !old_module || !new_module || !symbol || !out_data || !out_len || !old_module[0] ||
        !new_module[0] || !symbol[0]) {
        return CBM_EDIT_ERR_ARGS;
    }
    if (stats) {
        memset(stats, 0, sizeof(*stats));
    }

    size_t old_len = strlen(old_module);
    size_t sym_len = strlen(symbol);
    mv_sb_t sb = {0};
    bool oom = false;

    size_t pos = 0;
    while (pos < len) {
        size_t ls = pos;
        const char *nl = memchr(data + pos, '\n', len - pos);
        size_t le = nl ? (size_t)(nl - data) : len; /* before '\n' */
        size_t next = nl ? le + 1 : len;
        size_t ce = le; /* content end, excluding CR */
        if (ce > ls && data[ce - 1] == '\r') {
            ce--;
        }
        const char *eol = data + ce; /* [ce, next): "\r\n" | "\n" | "" */
        size_t eol_len = next - ce;

        /* parse: indent + "from" + module + "import" + names [+ comment] */
        size_t p = ls;
        while (p < ce && (data[p] == ' ' || data[p] == '\t')) {
            p++;
        }
        size_t indent_end = p;
        bool copied_plain = false;

        if (p + 4 < ce && memcmp(data + p, "from", 4) == 0 &&
            (data[p + 4] == ' ' || data[p + 4] == '\t')) {
            p += 4;
            while (p < ce && (data[p] == ' ' || data[p] == '\t')) {
                p++;
            }
            size_t mod_off = p;
            while (p < ce && (mv_is_ident_byte((unsigned char)data[p]) || data[p] == '.')) {
                p++;
            }
            size_t mod_len = p - mod_off;
            size_t save = p;
            while (p < ce && (data[p] == ' ' || data[p] == '\t')) {
                p++;
            }
            bool is_from_import = p + 6 <= ce && memcmp(data + p, "import", 6) == 0 &&
                                  (p + 6 == ce || data[p + 6] == ' ' || data[p + 6] == '\t');
            if (is_from_import && mod_len == old_len &&
                memcmp(data + mod_off, old_module, old_len) == 0) {
                p += 6;
                while (p < ce && (data[p] == ' ' || data[p] == '\t')) {
                    p++;
                }
                size_t names_off = p;
                /* comment starts at the first '#' (from-imports contain no
                 * string literals, so this is safe) */
                size_t names_end = ce;
                size_t comment_off = 0;
                for (size_t i = names_off; i < ce; i++) {
                    if (data[i] == '#') {
                        names_end = i;
                        comment_off = i;
                        break;
                    }
                }
                while (names_end > names_off &&
                       (data[names_end - 1] == ' ' || data[names_end - 1] == '\t')) {
                    names_end--;
                }

                mv_import_entry_t *entries = NULL;
                int nent =
                    mv_parse_import_entries(data + names_off, names_end - names_off, &entries);
                if (nent < 0) {
                    oom = true;
                    break;
                }
                if (nent == 0) {
                    /* parenthesized / star / unparsable — leave for REVIEW */
                    if (stats) {
                        const char *nm = data + names_off;
                        size_t nn = names_end - names_off;
                        while (nn > 0 && (*nm == ' ' || *nm == '\t')) {
                            nm++;
                            nn--;
                        }
                        if (nn > 0 && *nm == '*') {
                            stats->star_import_refs++;
                        } else {
                            stats->parenthesized_skipped++;
                        }
                    }
                } else {
                    int hit = -1;
                    for (int i = 0; i < nent; i++) {
                        if (entries[i].name_len == sym_len &&
                            memcmp(data + names_off + entries[i].name_off, symbol, sym_len) == 0) {
                            hit = i;
                            break;
                        }
                    }
                    if (hit < 0) {
                        free(entries);
                        entries = NULL;
                    } else {
                        if (entries[hit].has_alias && stats) {
                            stats->aliases_kept++;
                        }
                        /* indent */
                        mv_sb_append(&sb, data + ls, indent_end - ls);
                        if (nent == 1) {
                            mv_sb_append_str(&sb, "from ");
                            mv_sb_append_str(&sb, new_module);
                            mv_sb_append_str(&sb, " import ");
                            mv_sb_append_entry(&sb, data + names_off, &entries[hit]);
                            if (comment_off) {
                                mv_sb_append_str(&sb, "  ");
                                mv_sb_append(&sb, data + comment_off, ce - comment_off);
                            }
                            mv_sb_append(&sb, eol, eol_len);
                            if (stats) {
                                stats->import_lines_rewritten++;
                            }
                        } else {
                            /* keep the others on the original module line */
                            mv_sb_append_str(&sb, "from ");
                            mv_sb_append_str(&sb, old_module);
                            mv_sb_append_str(&sb, " import ");
                            bool first = true;
                            for (int i = 0; i < nent; i++) {
                                if (i == hit) {
                                    continue;
                                }
                                if (!first) {
                                    mv_sb_append_str(&sb, ", ");
                                }
                                mv_sb_append_entry(&sb, data + names_off, &entries[i]);
                                first = false;
                            }
                            if (comment_off) {
                                mv_sb_append_str(&sb, "  ");
                                mv_sb_append(&sb, data + comment_off, ce - comment_off);
                            }
                            mv_sb_append(&sb, eol, eol_len);
                            /* new line for the moved symbol on the new module */
                            mv_sb_append(&sb, data + ls, indent_end - ls);
                            mv_sb_append_str(&sb, "from ");
                            mv_sb_append_str(&sb, new_module);
                            mv_sb_append_str(&sb, " import ");
                            mv_sb_append_entry(&sb, data + names_off, &entries[hit]);
                            mv_sb_append(&sb, eol, eol_len);
                            if (stats) {
                                stats->import_lines_split++;
                            }
                        }
                        free(entries);
                        copied_plain = true;
                    }
                }
            } else {
                p = save; /* not our module / not a from-import: fall through */
            }
        }

        if (!copied_plain) {
            if (stats) {
                if (mv_line_plain_imports_module(data, ls, ce, old_module, old_len)) {
                    stats->plain_import_refs++;
                } else if (mv_line_is_all_ref(data, ls, ce, symbol)) {
                    stats->all_refs++;
                }
            }
            mv_sb_append(&sb, data + ls, next - ls); /* line + its terminator */
        }
        pos = next;
    }

    if (oom || sb.oom) {
        free(sb.buf);
        return CBM_EDIT_ERR_OOM;
    }
    if (!sb.buf) {
        /* empty input */
        sb.buf = malloc(1);
        if (!sb.buf) {
            return CBM_EDIT_ERR_OOM;
        }
        sb.len = 0;
    }
    sb.buf[sb.len] = '\0';
    *out_data = sb.buf;
    *out_len = sb.len;
    return CBM_EDIT_OK;
}
