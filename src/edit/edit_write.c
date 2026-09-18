/*
 * edit_write.c — File state tokens, whole-file reads, atomic writes.
 *
 * The write path mirrors the codebase's existing artifact publish pattern
 * (temp file in the same directory + rename-replace, see
 * src/pipeline/artifact.c) and adds the two guarantees edits need on top:
 * an mtime/size compare before anything is touched, and a backup copy of the
 * original for undo.
 */

#include "edit/edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "foundation/compat_fs.h"
#include "foundation/constants.h"

#ifdef _WIN32
#include <process.h>
#define EDIT_PID() _getpid()
#else
#include <unistd.h>
#define EDIT_PID() getpid()
#endif

#if defined(CBM_EDIT_TEST_API) && CBM_EDIT_TEST_API
/* One-shot fault injection for the write path (test builds only; the
 * production build has no hook or branch at the write entry). */
#include <stdatomic.h>
static atomic_bool g_edit_test_fail_write = false;

void cbm_edit_write_test_fail_once(void) {
    atomic_store(&g_edit_test_fail_write, true);
}

void cbm_edit_write_test_reset_faults(void) {
    atomic_store(&g_edit_test_fail_write, false);
}

static bool edit_test_take_write_failure(void) {
    return atomic_exchange(&g_edit_test_fail_write, false);
}
#endif

/* Platform-portable mtime in nanoseconds (mirrors watcher.c/artifact.c). */
static int64_t edit_stat_mtime_ns(const struct stat *st) {
#if defined(_WIN32)
    return (int64_t)st->st_mtime * 1000000000LL;
#elif defined(__APPLE__)
    return (int64_t)st->st_mtimespec.tv_sec * 1000000000LL + (int64_t)st->st_mtimespec.tv_nsec;
#else
    return (int64_t)st->st_mtim.tv_sec * 1000000000LL + (int64_t)st->st_mtim.tv_nsec;
#endif
}

int cbm_edit_file_stat(const char *abs_path, cbm_edit_file_state_t *out) {
    if (!abs_path || !out) {
        return CBM_EDIT_ERR_ARGS;
    }
    struct stat st;
    if (stat(abs_path, &st) != 0) {
        return CBM_EDIT_ERR_IO;
    }
    out->mtime_ns = edit_stat_mtime_ns(&st);
    out->size = (int64_t)st.st_size;
    return CBM_EDIT_OK;
}

int cbm_edit_read_file(const char *abs_path, char **out_data, size_t *out_len) {
    if (!abs_path || !out_data || !out_len) {
        return CBM_EDIT_ERR_ARGS;
    }
    FILE *fp = cbm_fopen(abs_path, "rb");
    if (!fp) {
        return CBM_EDIT_ERR_IO;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        (void)fclose(fp);
        return CBM_EDIT_ERR_IO;
    }
    long sz = ftell(fp);
    if (sz < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        (void)fclose(fp);
        return CBM_EDIT_ERR_IO;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        (void)fclose(fp);
        return CBM_EDIT_ERR_OOM;
    }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    if (got != (size_t)sz) {
        free(buf);
        (void)fclose(fp);
        return CBM_EDIT_ERR_IO;
    }
    (void)fclose(fp);
    buf[got] = '\0';
    *out_data = buf;
    *out_len = got;
    return CBM_EDIT_OK;
}

/* Base name of a path (after the last '/' or '\'), for backup file naming. */
static const char *edit_basename(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

int cbm_edit_write_atomic(const char *abs_path, const char *data, size_t len,
                          const cbm_edit_file_state_t *expected, const char *backup_dir,
                          char *backup_path_out, size_t backup_path_sz) {
    if (!abs_path || !data) {
        return CBM_EDIT_ERR_ARGS;
    }

#if defined(CBM_EDIT_TEST_API) && CBM_EDIT_TEST_API
    /* Simulated write-path failure: nothing is written, no backup is made —
     * callers cannot distinguish this from a real IO error, which is the
     * point of the exercise. */
    if (edit_test_take_write_failure()) {
        return CBM_EDIT_ERR_IO;
    }
#endif

    /* 1. Optimistic concurrency: refuse to write over a file that changed
     *    since the caller read it. */
    if (expected) {
        cbm_edit_file_state_t now = {0};
        if (cbm_edit_file_stat(abs_path, &now) != CBM_EDIT_OK) {
            return CBM_EDIT_ERR_IO;
        }
        if (now.mtime_ns != expected->mtime_ns || now.size != expected->size) {
            return CBM_EDIT_ERR_MTIME;
        }
    }

    /* 2. Backup the original before anything is replaced. */
    if (backup_dir) {
        if (!cbm_mkdir_p(backup_dir, 0755)) {
            return CBM_EDIT_ERR_IO;
        }
        char backup[CBM_SZ_4K];
        int n = snprintf(backup, sizeof(backup), "%s/bk_%lld_%d_%s", backup_dir,
                         (long long)time(NULL), EDIT_PID(), edit_basename(abs_path));
        if (n < 0 || (size_t)n >= sizeof(backup)) {
            return CBM_EDIT_ERR_IO;
        }
        if (cbm_clone_or_copy_file(abs_path, backup) != 0) {
            return CBM_EDIT_ERR_IO;
        }
        if (backup_path_out && backup_path_sz > 0) {
            snprintf(backup_path_out, backup_path_sz, "%s", backup);
        }
    }

    /* 3. Temp file in the same directory + rename-replace (atomic on both
     *    POSIX and Windows via cbm_rename_replace). */
    char tmp[CBM_SZ_4K];
    int n = snprintf(tmp, sizeof(tmp), "%s.cbm-edit-%d.tmp", abs_path, EDIT_PID());
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return CBM_EDIT_ERR_IO;
    }
    FILE *fp = cbm_fopen(tmp, "wb");
    if (!fp) {
        return CBM_EDIT_ERR_IO;
    }
    size_t written = fwrite(data, 1, len, fp);
    if (written != len) {
        (void)fclose(fp);
        (void)cbm_unlink(tmp);
        return CBM_EDIT_ERR_IO;
    }
    if (fclose(fp) != 0) {
        (void)cbm_unlink(tmp);
        return CBM_EDIT_ERR_IO;
    }
    if (cbm_rename_replace(tmp, abs_path) != 0) {
        (void)cbm_unlink(tmp);
        return CBM_EDIT_ERR_IO;
    }
    return CBM_EDIT_OK;
}

/* Parse a backup name of the form bk_<epoch>_<pid>_<basename>. Returns true
 * and fills epoch/pid/bn when the shape matches. */
static bool edit_backup_name_parse(const char *name, long long *epoch_out, long *pid_out,
                                   const char **bn_out) {
    if (strncmp(name, "bk_", 3) != 0) {
        return false;
    }
    const char *p = name + 3;
    if (*p < '0' || *p > '9') {
        return false;
    }
    char *end1 = NULL;
    long long epoch = strtoll(p, &end1, 10);
    if (end1 == p || *end1 != '_') {
        return false;
    }
    const char *q = end1 + 1;
    if (*q < '0' || *q > '9') {
        return false;
    }
    char *end2 = NULL;
    long pid = strtol(q, &end2, 10);
    if (end2 == q || *end2 != '_') {
        return false;
    }
    *epoch_out = epoch;
    *pid_out = pid;
    *bn_out = end2 + 1;
    return true;
}

int cbm_edit_latest_backup(const char *backup_dir, const char *basename, char *out, size_t out_sz) {
    if (!backup_dir || !basename || !out || out_sz == 0 || basename[0] == '\0') {
        return CBM_EDIT_ERR_ARGS;
    }
    cbm_dir_t *d = cbm_opendir(backup_dir);
    if (!d) {
        return CBM_EDIT_ERR_IO;
    }
    bool found = false;
    long long best_epoch = -1;
    long best_pid = -1;
    cbm_dirent_t *entry;
    while ((entry = cbm_readdir(d)) != NULL) {
        if (entry->is_dir) {
            continue;
        }
        long long epoch = 0;
        long pid = 0;
        const char *bn = NULL;
        if (!edit_backup_name_parse(entry->name, &epoch, &pid, &bn)) {
            continue;
        }
        if (strcmp(bn, basename) != 0) {
            continue;
        }
        if (!found || epoch > best_epoch || (epoch == best_epoch && pid > best_pid)) {
            /* entry->name is only valid until the next cbm_readdir — copy the
             * full path out now. */
            int n = snprintf(out, out_sz, "%s/%s", backup_dir, entry->name);
            if (n < 0 || (size_t)n >= out_sz) {
                continue; /* path would be truncated — not a usable candidate */
            }
            best_epoch = epoch;
            best_pid = pid;
            found = true;
        }
    }
    cbm_closedir(d);
    return found ? CBM_EDIT_OK : CBM_EDIT_ERR_RANGE;
}
