/*
 * libFuzzer target for the pure OpenTelemetry trace normalization helpers.
 *
 * The input is treated as an arbitrary NUL-terminated attribute/timestamp
 * value. This exercises URL path extraction, strict duration parsing, HTTP
 * attribute scanning, service-name lookup, and percentile handling without
 * opening a database or starting the MCP server.
 */
#include "traces/traces.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == SIZE_MAX || (size > 0 && !data)) {
        return 0;
    }

    char *value = malloc(size + 1U);
    if (!value) {
        return 0;
    }
    if (size > 0) {
        memcpy(value, data, size);
    }
    value[size] = '\0';

    cbm_trace_attr_t resource_attrs[] = {
        {.key = "service.name", .string_value = value},
    };
    cbm_trace_resource_t resource = {
        .attributes = resource_attrs,
        .attr_count = 1,
    };
    (void)cbm_extract_service_name(&resource);

    cbm_trace_attr_t span_attrs[] = {
        {.key = "url.full", .string_value = value},
        {.key = "http.method", .string_value = value},
    };
    cbm_trace_span_t span = {
        .kind = 2,
        .attributes = span_attrs,
        .attr_count = 2,
        .start_time = value,
        .end_time = value,
    };
    cbm_http_span_info_t info;
    (void)cbm_extract_http_info(&span, value, &info);

    char path[CBM_TRACE_PATH_MAX];
    (void)cbm_extract_path_from_url(value, path, sizeof(path));
    (void)cbm_parse_duration(value, value);

    int64_t values[32];
    size_t value_count = size < (sizeof(values) / sizeof(values[0]))
                             ? size
                             : sizeof(values) / sizeof(values[0]);
    for (size_t i = 0; i < value_count; i++) {
        values[i] = (int64_t)data[i];
    }
    (void)cbm_calculate_p99(values, (int)value_count);

    free(value);
    return 0;
}
