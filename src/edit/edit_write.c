/*
 * edit_write.c — File state tokens, whole-file reads, atomic writes.
 *
 * The write path mirrors the codebase's existing artifact publish pattern
 * (temp file in the same directory + rename-replace, see
 * src/pipeline/artifact.c) and adds the two guarantees edits need on top:
 * a content/mtime/size compare before anything is touched, and a backup copy of the
 * original for undo.
 */

#include "edit/edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "foundation/compat_fs.h"
#include "foundation/compat.h"
#include "foundation/sha256.h"
#include "foundation/constants.h"

enum { EDIT_BACKUP_PREFIX_LEN = 3, EDIT_BACKUP_NAME_LEN = 23, EDIT_BACKUP_DIR_MODE = 0700 };

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

/* Use the UTF-8/high-resolution filesystem API and hash bytes as well as
 * metadata: same-size edits and restored mtimes must not bypass the guard. */
int cbm_edit_file_stat(const char *abs_path, cbm_edit_file_state_t *out) {
    if (!abs_path || !out) {
        return CBM_EDIT_ERR_ARGS;
    }
    cbm_path_info_t before;
    cbm_path_info_t after;
    if (cbm_path_info_utf8(abs_path, &before) != 0 || !before.is_regular) {
        return CBM_EDIT_ERR_IO;
    }
    FILE *fp = cbm_fopen(abs_path, "rb");
    if (!fp) {
        return CBM_EDIT_ERR_IO;
    }
    cbm_sha256_ctx hash;
    cbm_sha256_init(&hash);
    unsigned char buf[CBM_SZ_64K];
    size_t n;
    while ((n = fread(buf, SKIP_ONE, sizeof(buf), fp)) > 0) {
        cbm_sha256_update(&hash, buf, n);
    }
    bool ok = !ferror(fp);
    if (fclose(fp) != 0) {
        ok = false;
    }
    if (!ok || cbm_path_info_utf8(abs_path, &after) != 0 || !after.is_regular) {
        return CBM_EDIT_ERR_IO;
    }
    if (before.mtime_ns != after.mtime_ns || before.size != after.size) {
        return CBM_EDIT_ERR_MTIME;
    }
    out->mtime_ns = after.mtime_ns;
    out->size = after.size;
    cbm_sha256_final(&hash, out->content_hash);
    return CBM_EDIT_OK;
}

static int edit_check_state(const char *path, const cbm_edit_file_state_t *expected) {
    if (!expected) {
        return CBM_EDIT_OK;
    }
    cbm_edit_file_state_t now;
    int rc = cbm_edit_file_stat(path, &now);
    if (rc != CBM_EDIT_OK) {
        return rc;
    }
    return now.mtime_ns == expected->mtime_ns && now.size == expected->size &&
                   memcmp(now.content_hash, expected->content_hash, sizeof(now.content_hash)) == 0
               ? CBM_EDIT_OK
               : CBM_EDIT_ERR_MTIME;
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
    char *buf = malloc((size_t)sz + SKIP_ONE);
    if (!buf) {
        (void)fclose(fp);
        return CBM_EDIT_ERR_OOM;
    }
    size_t got = fread(buf, SKIP_ONE, (size_t)sz, fp);
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
            base = p + SKIP_ONE;
        }
    }
    return base;
}

/* Hash the canonical parent plus filename, so lookup still works after the
 * source file is deleted. Different projects/directories never share history.
 * Legacy flat backups intentionally have no automatic lookup path. */
static int edit_backup_directory(const char *root, const char *path, char *out, size_t cap) {
    const char *base = edit_basename(path);
    if (base == path || !*base || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        return CBM_EDIT_ERR_ARGS;
    }
    char parent[CBM_SZ_4K];
    char canonical[CBM_SZ_4K];
    char identity[CBM_SZ_4K];
    size_t len = (size_t)(base - path);
    if (len >= sizeof(parent)) {
        return CBM_EDIT_ERR_ARGS;
    }
    memcpy(parent, path, len);
    parent[len] = '\0';
    if (!cbm_canonical_path(parent, canonical, sizeof(canonical))) {
        return CBM_EDIT_ERR_IO;
    }
    int n = snprintf(identity, sizeof(identity), "%s/%s", canonical, base);
    if (n < 0 || (size_t)n >= sizeof(identity)) {
        return CBM_EDIT_ERR_ARGS;
    }
#ifdef _WIN32
    /* The same DOS path may be passed with either slash. Preserve case-sensitive directory
     * identities. */
    for (char *p = identity; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
#endif
    char key[CBM_SHA256_HEX_LEN + SKIP_ONE];
    cbm_sha256_hex(identity, strlen(identity), key);
    n = snprintf(out, cap, "%s/%s", root, key);
    return n >= 0 && (size_t)n < cap ? CBM_EDIT_OK : CBM_EDIT_ERR_ARGS;
}

/* Only complete published records count. A staged or malformed file cannot
 * become an undo candidate after interruption. */
static uint64_t edit_backup_sequence(const char *name) {
    if (strlen(name) != EDIT_BACKUP_NAME_LEN || strncmp(name, "bk_", EDIT_BACKUP_PREFIX_LEN) != 0) {
        return 0;
    }
    for (int i = EDIT_BACKUP_PREFIX_LEN; i < EDIT_BACKUP_NAME_LEN; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return 0;
        }
    }
    errno = 0;
    unsigned long long value = strtoull(name + EDIT_BACKUP_PREFIX_LEN, NULL, CBM_DECIMAL_BASE);
    return errno == ERANGE ? 0 : (uint64_t)value;
}

static int edit_backup_last_sequence(const char *dir, uint64_t *last) {
    *last = 0;
    cbm_dir_t *d = cbm_opendir(dir);
    if (!d) {
        return CBM_EDIT_ERR_IO;
    }
    cbm_dirent_t *entry;
    while ((entry = cbm_readdir(d)) != NULL) {
        uint64_t seq = entry->is_dir ? 0 : edit_backup_sequence(entry->name);
        if (seq > *last) {
            *last = seq;
        }
    }
    cbm_closedir(d);
    return CBM_EDIT_OK;
}

/* Creates a unique, complete temporary file; removes it on every failure. */
static int edit_stage_bytes(char *tmp, const char *data, size_t len) {
    int fd = cbm_mkstemp(tmp);
    if (fd < 0) {
        return CBM_EDIT_ERR_IO;
    }
    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
        close(fd);
        cbm_unlink(tmp);
        return CBM_EDIT_ERR_IO;
    }
    bool ok = fwrite(data, SKIP_ONE, len, fp) == len;
    if (fclose(fp) != 0) {
        ok = false;
    }
    if (!ok) {
        cbm_unlink(tmp);
    }
    return ok ? CBM_EDIT_OK : CBM_EDIT_ERR_IO;
}

static int edit_publish_backup(const char *backup_dir, const char *abs_path, char *backup_path_out,
                               size_t backup_path_sz) {
    int rc;
    char dir[CBM_SZ_4K];
    char backup[CBM_SZ_4K];
    char tmp[CBM_SZ_4K];
    rc = edit_backup_directory(backup_dir, abs_path, dir, sizeof(dir));
    if (rc != CBM_EDIT_OK) {
        return rc;
    }
    if (!cbm_mkdir_p(dir, EDIT_BACKUP_DIR_MODE)) {
        return CBM_EDIT_ERR_IO;
    }
    uint64_t last;
    rc = edit_backup_last_sequence(dir, &last);
    if (rc != CBM_EDIT_OK || last == UINT64_MAX) {
        return CBM_EDIT_ERR_IO;
    }
    int n = snprintf(backup, sizeof(backup), "%s/bk_%020" PRIu64, dir, last + SKIP_ONE);
    if (n < 0 || (size_t)n >= sizeof(backup) || (backup_path_out && (size_t)n >= backup_path_sz)) {
        return CBM_EDIT_ERR_ARGS;
    }
    n = snprintf(tmp, sizeof(tmp), "%s/pending-XXXXXX", dir);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return CBM_EDIT_ERR_ARGS;
    }
    char *original = NULL;
    size_t original_len = 0;
    rc = cbm_edit_read_file(abs_path, &original, &original_len);
    if (rc != CBM_EDIT_OK) {
        return rc;
    }
    rc = edit_stage_bytes(tmp, original, original_len);
    free(original);
    if (rc != CBM_EDIT_OK) {
        return rc;
    }
    /* No overwrite and no retry: a competing publication fails safely. */
    if (cbm_rename_noreplace(tmp, backup) != 0) {
        cbm_unlink(tmp);
        return CBM_EDIT_ERR_IO;
    }
    if (backup_path_out) {
        /* The complete name was checked against backup_path_sz before publication. */
        size_t backup_len = strlen(backup);
        memcpy(backup_path_out, backup, backup_len);
        backup_path_out[backup_len] = '\0';
    }

    return CBM_EDIT_OK;
}

int cbm_edit_write_atomic(const char *abs_path, const char *data, size_t len,
                          const cbm_edit_file_state_t *expected, const char *backup_dir,
                          char *backup_path_out, size_t backup_path_sz) {
    if (!abs_path || !data || (backup_path_out && backup_path_sz == 0)) {
        return CBM_EDIT_ERR_ARGS;
    }
    if (backup_path_out) {
        backup_path_out[0] = '\0';
    }
#if defined(CBM_EDIT_TEST_API) && CBM_EDIT_TEST_API
    if (edit_test_take_write_failure()) {
        return CBM_EDIT_ERR_IO;
    }
#endif
    int rc = edit_check_state(abs_path, expected);
    if (rc != CBM_EDIT_OK) {
        return rc;
    }

    if (backup_dir) {
        rc = edit_publish_backup(backup_dir, abs_path, backup_path_out, backup_path_sz);
        if (rc != CBM_EDIT_OK) {
            return rc;
        }
    }

    char tmp[CBM_SZ_4K];
    int n = snprintf(tmp, sizeof(tmp), "%s.cbm-edit-XXXXXX", abs_path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return CBM_EDIT_ERR_ARGS;
    }
    rc = edit_stage_bytes(tmp, data, len);
    if (rc != CBM_EDIT_OK) {
        return rc;
    }
    rc = edit_check_state(abs_path, expected);
    if (rc == CBM_EDIT_OK && cbm_rename_replace(tmp, abs_path) != 0) {
        rc = CBM_EDIT_ERR_IO;
    }
    if (rc != CBM_EDIT_OK) {
        cbm_unlink(tmp);
    }
    return rc;
}

int cbm_edit_latest_backup(const char *backup_dir, const char *abs_path, char *out, size_t out_sz) {
    if (!backup_dir || !abs_path || !out || out_sz == 0) {
        return CBM_EDIT_ERR_ARGS;
    }
    char dir[CBM_SZ_4K];
    int rc = edit_backup_directory(backup_dir, abs_path, dir, sizeof(dir));
    if (rc != CBM_EDIT_OK) {
        return rc;
    }
    cbm_path_info_t info;
    if (cbm_path_info_utf8(dir, &info) != 0) {
        return CBM_EDIT_ERR_RANGE;
    }
    uint64_t last;
    rc = edit_backup_last_sequence(dir, &last);
    if (rc != CBM_EDIT_OK) {
        return rc;
    }
    if (last == 0) {
        return CBM_EDIT_ERR_RANGE;
    }
    int n = snprintf(out, out_sz, "%s/bk_%020" PRIu64, dir, last);
    return n >= 0 && (size_t)n < out_sz ? CBM_EDIT_OK : CBM_EDIT_ERR_ARGS;
}
