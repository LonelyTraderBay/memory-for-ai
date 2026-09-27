/*
 * system_info.c — CPU core count and RAM detection.
 *
 * Windows: GetSystemInfo + GlobalMemoryStatusEx.
 *
 * Results are cached after first call (immutable hardware properties).
 */
#include "foundation/constants.h"

enum { DEFAULT_CORES = 1, MIN_WORKERS = 1, CBM_WORKERS_MAX = 256 };
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/system_info_internal.h"
#include <stdint.h> // uint64_t
#include <stdlib.h> // strtol
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* ── Windows detection ───────────────────────────────────────────── */

static cbm_system_info_t detect_system_windows(void) {
    cbm_system_info_t info;
    memset(&info, 0, sizeof(info));

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    info.total_cores = (int)si.dwNumberOfProcessors;
    if (info.total_cores < 1) {
        info.total_cores = SKIP_ONE;
    }
    info.perf_cores = info.total_cores;

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        info.total_ram = (size_t)ms.ullTotalPhys;
    }

    return info;
}

/* ── Public API ──────────────────────────────────────────────────── */

static int info_cached = 0;
static cbm_system_info_t cached_info;

cbm_system_info_t cbm_system_info(void) {
    if (!info_cached) {
        cached_info = detect_system_windows();
        info_cached = SKIP_ONE;
    }
    return cached_info;
}

int cbm_default_worker_count(bool initial) {
    /* CBM_WORKERS env override (clamped to [1, CBM_WORKERS_MAX]).
     * Useful inside containers where sysconf(_SC_NPROCESSORS_ONLN)
     * reports host CPUs rather than the cgroup's effective CPU quota.
     * Same precedence shape as other CBM_* env overrides:
     * explicit override > implicit detection. */
    char buf[CBM_SZ_32];
    if (cbm_safe_getenv("CBM_WORKERS", buf, sizeof(buf), NULL) != NULL) {
        long n = strtol(buf, NULL, CBM_DECIMAL_BASE);
        if (n >= MIN_WORKERS && n <= CBM_WORKERS_MAX) {
            return (int)n;
        }
        cbm_log_warn("workers.env.invalid", "value", buf, "fallback", "GetSystemInfo");
    }

    cbm_system_info_t info = cbm_system_info();
    if (initial) {
        /* Use all cores for initial indexing — user is waiting */
        return info.total_cores;
    }
    /* Incremental: leave headroom for user's apps */
    int workers = info.perf_cores - SKIP_ONE;
    return workers > 0 ? workers : MIN_WORKERS;
}
