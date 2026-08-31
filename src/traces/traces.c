/*
 * traces.c — OTLP trace processing helpers.
 */
#include <stdint.h>
#include "traces/traces.h"
#include "foundation/constants.h"

enum {
    TRACE_PATH_NONE = 0,
    TRACE_PATH_URL_FULL = 1,
    TRACE_PATH_HTTP_TARGET = 2,
    TRACE_PATH_URL = 3,
    TRACE_PATH_ROUTE = 4
};
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <errno.h>

/* ── extractServiceName ──────────────────────────────────────────── */

const char *cbm_extract_service_name(const cbm_trace_resource_t *r) {
    if (!r || !r->attributes || r->attr_count <= 0) {
        return "";
    }
    for (int i = 0; i < r->attr_count; i++) {
        if (r->attributes[i].key && strcmp(r->attributes[i].key, "service.name") == 0) {
            return r->attributes[i].string_value ? r->attributes[i].string_value : "";
        }
    }
    return "";
}

/* ── extractPathFromURL ──────────────────────────────────────────── */

const char *cbm_extract_path_from_url(const char *url, char *buf, size_t buf_sz) {
    if (!url || !buf || buf_sz == 0) {
        if (buf) {
            buf[0] = '\0';
        }
        return buf ? buf : "";
    }

    size_t authority_start;
    if (strncmp(url, "https://", 8) == 0) {
        authority_start = 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        authority_start = 7;
    } else {
        buf[0] = '\0';
        return buf;
    }

    /* Locate the first path delimiter without looking inside the query or
     * fragment. Reject an empty authority and URLs that have no path. */
    if (url[authority_start] == '\0' || url[authority_start] == '/' ||
        url[authority_start] == '?' || url[authority_start] == '#') {
        buf[0] = '\0';
        return buf;
    }
    size_t path_start = authority_start;
    while (url[path_start] && url[path_start] != '/' && url[path_start] != '?' &&
           url[path_start] != '#') {
        path_start++;
    }
    if (url[path_start] != '/') {
        buf[0] = '\0';
        return buf;
    }

    /* Copy path, stopping at query or fragment delimiters. */
    size_t j = 0;
    for (size_t i = path_start; url[i] && url[i] != '?' && url[i] != '#' &&
         j < buf_sz - SKIP_ONE; i++) {
        buf[j++] = url[i];
    }
    buf[j] = '\0';
    return buf;
}

/* ── parseDuration ───────────────────────────────────────────────── */

int64_t cbm_parse_duration(const char *start_nano, const char *end_nano) {
    if (!start_nano || !end_nano || !*start_nano || !*end_nano) {
        return 0;
    }

    /* Timestamps arrive at the JSON boundary as decimal strings. Reject
     * whitespace, signs, suffixes, and overflow instead of allowing strtoll
     * to silently return a partial or saturated value. */
    for (const char *p = start_nano; *p; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
    }
    for (const char *p = end_nano; *p; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
    }

    errno = 0;
    char *start_end = NULL;
    uintmax_t start_value = strtoumax(start_nano, &start_end, CBM_DECIMAL_BASE);
    if (errno == ERANGE || start_end == start_nano || *start_end != '\0' ||
        start_value > (uintmax_t)INT64_MAX) {
        return 0;
    }
    errno = 0;
    char *end_end = NULL;
    uintmax_t end_value = strtoumax(end_nano, &end_end, CBM_DECIMAL_BASE);
    if (errno == ERANGE || end_end == end_nano || *end_end != '\0' ||
        end_value > (uintmax_t)INT64_MAX) {
        return 0;
    }

    int64_t start = (int64_t)start_value;
    int64_t end = (int64_t)end_value;
    return (end > start) ? (end - start) : 0;
}

/* ── extractHTTPInfo ─────────────────────────────────────────────── */

bool cbm_extract_http_info(const cbm_trace_span_t *span, const char *service_name,
                           cbm_http_span_info_t *out) {
    if (!span || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->service_name = service_name ? service_name : "";
    out->span_kind = span->kind;

    /* A malformed or partially decoded request must not turn into a native
     * null-dereference. Treat a positive count without storage as invalid. */
    if (span->attr_count > 0 && !span->attributes) {
        return false;
    }

    bool has_http = false;
    int path_priority = TRACE_PATH_NONE;

    for (int i = 0; i < span->attr_count; i++) {
        const char *key = span->attributes[i].key;
        const char *val = span->attributes[i].string_value;
        if (!key || !val) {
            continue;
        }

        if (strcmp(key, "http.method") == 0 || strcmp(key, "http.request.method") == 0) {
            out->method = val;
            has_http = true;
        } else if (strcmp(key, "http.route") == 0) {
            /* The route template is the most stable cross-service identity. */
            if (val[0] != '\0' && path_priority < TRACE_PATH_ROUTE) {
                out->path = val;
                path_priority = TRACE_PATH_ROUTE;
                has_http = true;
            }
        } else if (strcmp(key, "url.path") == 0) {
            if (val[0] != '\0' && path_priority < TRACE_PATH_URL) {
                out->path = val;
                path_priority = TRACE_PATH_URL;
                has_http = true;
            }
        } else if (strcmp(key, "http.target") == 0) {
            if (val[0] != '\0' && path_priority < TRACE_PATH_HTTP_TARGET) {
                out->path = val;
                path_priority = TRACE_PATH_HTTP_TARGET;
                has_http = true;
            }
        } else if (strcmp(key, "http.status_code") == 0) {
            out->status_code = val;
        } else if (strcmp(key, "url.full") == 0 && path_priority < TRACE_PATH_URL_FULL) {
            const char *path = cbm_extract_path_from_url(val, out->path_storage,
                                                         sizeof(out->path_storage));
            if (path[0] != '\0') {
                out->path = path;
                path_priority = TRACE_PATH_URL_FULL;
                has_http = true;
            }
        }
    }

    if (!has_http || !out->path || out->path[0] == '\0') {
        return false;
    }

    out->duration_ns = cbm_parse_duration(span->start_time, span->end_time);
    return true;
}

/* ── calculateP99 ────────────────────────────────────────────────── */

static int cmp_int64(const void *a, const void *b) {
    int64_t va = *(const int64_t *)a;
    int64_t vb = *(const int64_t *)b;
    return (va > vb) - (va < vb);
}

int64_t cbm_calculate_p99(int64_t *values, int count) {
    if (!values || count <= 0) {
        return 0;
    }
    qsort(values, count, sizeof(int64_t), cmp_int64);
#define P99_PERCENTILE 0.99

    int idx = (int)((double)count * P99_PERCENTILE);
    if (idx >= count) {
        idx = count - SKIP_ONE;
    }
    return values[idx];
}
