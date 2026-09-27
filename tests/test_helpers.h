/*
 * test_helpers.h — Cross-platform test fixture helpers.
 *
 * Replaces system("rm -rf"), system("mkdir -p && echo >"), chmod(),
 * and hardcoded /tmp/ paths with portable C implementations.
 * Uses compat.h/compat_fs.h underneath — same abstraction layer
 * as production code.
 *
 * All functions are static inline to avoid linker issues when
 * included from multiple test files.
 */
#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/constants.h"
#include "../src/foundation/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include "../src/foundation/win_utf8.h"
#define TH_CHDIR _chdir
#define TH_GETCWD _getcwd
#else
#include <unistd.h>
#define TH_CHDIR chdir
#define TH_GETCWD getcwd
#endif

/* ── Path building ────────────────────────────────────────────── */

/* Build a path from base + relative. Uses a static buffer — not reentrant.
 * Usage: th_write_file(TH_PATH(base, "src/main.go"), "content"); */
#define TH_PATH(base, rel) th_path_join(base, rel)

static inline const char *th_path_join(const char *base, const char *rel) {
    static char buf[4][1024];
    static int idx = 0;
    int i = idx;
    idx = (idx + 1) & 3;
    snprintf(buf[i], sizeof(buf[i]), "%s/%s", base, rel);
    return buf[i];
}

/* ── File writing ─────────────────────────────────────────────── */

/* Write content to a file, creating parent directories as needed. */
static inline int th_write_file(const char *path, const char *content) {
    /* Create parent directories */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *last_slash = strrchr(dir, '/');
#ifdef _WIN32
    char *last_bslash = strrchr(dir, '\\');
    if (last_bslash && (!last_slash || last_bslash > last_slash)) {
        last_slash = last_bslash;
    }
#endif
    if (last_slash) {
        *last_slash = '\0';
        cbm_mkdir_p(dir, 0755);
    }

    FILE *f = cbm_fopen(path, "wb");
    if (!f) {
        return -1;
    }
    if (content && content[0]) {
        fputs(content, f);
    }
    fclose(f);
    return 0;
}

/* Append content to a file. */
static inline int th_append_file(const char *path, const char *content) {
    FILE *f = cbm_fopen(path, "ab");
    if (!f) {
        return -1;
    }
    if (content && content[0]) {
        fputs(content, f);
    }
    fclose(f);
    return 0;
}

/* ── Directory creation ───────────────────────────────────────── */

/* Create a directory and all parents. Returns 0 on success. */
static inline int th_mkdir_p(const char *path) {
    return cbm_mkdir_p(path, 0755) ? 0 : -1;
}

/* ── Recursive directory removal ──────────────────────────────── */

/* Git for Windows marks loose objects read-only. Match rm -rf semantics in
 * test cleanup without broadening production cbm_unlink behavior. */
static inline int th_unlink_force(const char *path) {
    int result = cbm_unlink(path);
#ifdef _WIN32
    if (result != 0) {
        wchar_t *wide = cbm_path_to_wide(path);
        DWORD attributes = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0U &&
            SetFileAttributesW(wide, attributes & ~((DWORD)FILE_ATTRIBUTE_READONLY))) {
            result = cbm_unlink(path);
        }
        free(wide);
    }
#endif
    return result;
}

/* Remove a file or directory tree recursively. Cross-platform rm -rf. */
static inline int th_rmtree(const char *path) {
    /* The platform wrappers preserve UTF-8 and add the extended-length prefix
     * on Windows; narrow CRT stat() silently treats paths beyond MAX_PATH as
     * absent and leaves their parents nonempty. */
    if (!cbm_file_exists(path)) {
        return 0; /* doesn't exist — success */
    }

    if (!cbm_is_dir(path)) {
        return th_unlink_force(path);
    }

    /* Directory — recurse into children */
    cbm_dir_t *d = cbm_opendir(path);
    if (!d) {
        return -1;
    }

    cbm_dirent_t *entry;
    int rc = 0;
    while ((entry = cbm_readdir(d)) != NULL) {
        if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0) {
            continue;
        }
        /* A directory entry can be close to CBM_DIRENT_NAME_MAX while the
         * parent path is also long. Allocate for the complete path instead
         * of silently recursing into a truncated sibling. */
        size_t path_len = strlen(path);
        size_t name_len = strlen(entry->name);
        if (path_len > (size_t)-1 - name_len - 2U) {
            rc = -1;
            continue;
        }
        size_t child_size = path_len + name_len + 2U;
        char *child = (char *)malloc(child_size);
        if (!child) {
            rc = -1;
            continue;
        }
        int child_len = snprintf(child, child_size, "%s/%s", path, entry->name);
        if (child_len < 0 || (size_t)child_len >= child_size) {
            free(child);
            rc = -1;
            continue;
        }
        if (entry->is_dir) {
            if (th_rmtree(child) != 0) {
                rc = -1;
            }
        } else {
            if (th_unlink_force(child) != 0) {
                rc = -1;
            }
        }
        free(child);
    }
    cbm_closedir(d);
    if (cbm_rmdir(path) != 0) {
        rc = -1;
    }
    return rc;
}

static inline int th_remove_path_limit_tree(const char *root, size_t component_count);

static inline int th_path_limit_component(size_t index, char *out, size_t out_size) {
    int written = snprintf(out, out_size,
                           "path_limit_%04zu_abcdefghijklmnopqrstuvwxyz"
                           "_ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789",
                           index);
    return written > 0 && (size_t)written < out_size ? written : -1;
}

/* Create a directory chain whose final absolute path exceeds the discovery
 * path limit. POSIX creates each level relative to its parent so no syscall
 * receives a path longer than PATH_MAX; Windows uses the extended-path-aware
 * filesystem wrapper. Caller removes it with th_remove_path_limit_tree. */
static inline int th_make_path_limit_tree(const char *root, size_t *component_count,
                                          size_t *final_path_len) {
    if (!root || !component_count || !final_path_len) {
        return -1;
    }
    *component_count = 0;
    *final_path_len = 0;

#ifdef _WIN32
    char path[CBM_SZ_4K + 128U];
    int root_len = snprintf(path, sizeof(path), "%s", root);
    if (root_len <= 0 || (size_t)root_len >= sizeof(path)) {
        return -1;
    }
    size_t path_len = (size_t)root_len;
    int result = -1;
    for (size_t index = 0; index < CBM_SZ_4K; index++) {
        char component[96];
        int component_len = th_path_limit_component(index, component, sizeof(component));
        if (component_len < 0 || path_len > (size_t)-1 - (size_t)component_len - 1U) {
            break;
        }
        size_t next_len = path_len + (size_t)component_len + 1U;
        if (next_len >= sizeof(path)) {
            break;
        }
        int append_len = snprintf(path + path_len, sizeof(path) - path_len, "/%s", component);
        if (append_len <= 0 || (size_t)append_len >= sizeof(path) - path_len ||
            !cbm_mkdir_p(path, 0755)) {
            break;
        }
        (*component_count)++;
        path_len = next_len;
        if (path_len > CBM_SZ_4K - 1U) {
            *final_path_len = path_len;
            result = 0;
            break;
        }
    }
#else
    char original_cwd[CBM_SZ_4K];
    if (!TH_GETCWD(original_cwd, sizeof(original_cwd)) || TH_CHDIR(root) != 0) {
        return -1;
    }

    size_t path_len = strlen(root);
    int result = -1;
    for (size_t index = 0; index < CBM_SZ_4K; index++) {
        char component[96];
        int component_len = th_path_limit_component(index, component, sizeof(component));
        if (component_len < 0 || path_len > (size_t)-1 - (size_t)component_len - 1U) {
            break;
        }
        size_t next_len = path_len + (size_t)component_len + 1U;
        if (!cbm_mkdir_p(component, 0755)) {
            break;
        }
        if (TH_CHDIR(component) != 0) {
            (void)cbm_rmdir(component);
            break;
        }
        (*component_count)++;
        path_len = next_len;
        if (path_len > CBM_SZ_4K - 1U) {
            *final_path_len = path_len;
            result = 0;
            break;
        }
    }

    if (TH_CHDIR(original_cwd) != 0) {
        result = -1;
    }
#endif
    if (result != 0 && *component_count > 0U) {
        (void)th_remove_path_limit_tree(root, *component_count);
        *component_count = 0;
    }
    return result;
}

/* Remove the relative component chain created by th_make_path_limit_tree. */
static inline int th_remove_path_limit_tree(const char *root, size_t component_count) {
    if (!root) {
        return -1;
    }
#ifdef _WIN32
    char path[CBM_SZ_4K + 128U];
    int root_len = snprintf(path, sizeof(path), "%s", root);
    if (root_len <= 0 || (size_t)root_len >= sizeof(path)) {
        return -1;
    }
    size_t root_path_len = (size_t)root_len;
    size_t path_len = root_path_len;
    size_t entered = 0;
    int result = 0;
    for (; entered < component_count; entered++) {
        char component[96];
        int component_len = th_path_limit_component(entered, component, sizeof(component));
        if (component_len < 0 || path_len > (size_t)-1 - (size_t)component_len - 1U ||
            path_len + (size_t)component_len + 1U >= sizeof(path)) {
            result = -1;
            break;
        }
        int append_len = snprintf(path + path_len, sizeof(path) - path_len, "/%s", component);
        if (append_len <= 0 || (size_t)append_len >= sizeof(path) - path_len) {
            result = -1;
            break;
        }
        path_len += (size_t)append_len;
    }
    if (entered != component_count) {
        return -1;
    }
    while (entered > 0U) {
        if (cbm_rmdir(path) != 0) {
            result = -1;
        }
        size_t parent_len = path_len;
        while (parent_len > root_path_len && path[parent_len - 1U] != '/') {
            parent_len--;
        }
        if (parent_len <= root_path_len) {
            result = -1;
            break;
        }
        path_len = parent_len - 1U;
        path[path_len] = '\0';
        entered--;
    }
    return result;
#else
    char original_cwd[CBM_SZ_4K];
    if (!TH_GETCWD(original_cwd, sizeof(original_cwd)) || TH_CHDIR(root) != 0) {
        return -1;
    }

    size_t entered = 0;
    for (; entered < component_count; entered++) {
        char component[96];
        int component_len = th_path_limit_component(entered, component, sizeof(component));
        if (component_len < 0 || TH_CHDIR(component) != 0) {
            break;
        }
    }

    int result = entered == component_count ? 0 : -1;
    while (entered > 0U) {
        char component[96];
        size_t index = entered - 1U;
        int component_len = th_path_limit_component(index, component, sizeof(component));
        if (TH_CHDIR("..") != 0 || component_len <= 0 ||
            (size_t)component_len >= sizeof(component) || cbm_rmdir(component) != 0) {
            result = -1;
            break;
        }
        entered--;
    }
    if (TH_CHDIR(original_cwd) != 0) {
        result = -1;
    }
    return result;
#endif
}

/* ── Temp directory creation ──────────────────────────────────── */

/* Create a temporary directory. Returns static buffer with path.
 * Pattern: prefix is used as part of the dirname. */
static inline char *th_mktempdir(const char *prefix) {
    static char buf[256];
#ifdef _WIN32
    const char *tmp = getenv("TEMP");
    if (!tmp)
        tmp = getenv("TMP");
    if (!tmp)
        tmp = "C:\\Temp";
    snprintf(buf, sizeof(buf), "%s\\%s_XXXXXX", tmp, prefix);
#else
    snprintf(buf, sizeof(buf), "/tmp/%s_XXXXXX", prefix);
#endif
    if (!cbm_mkdtemp(buf)) {
        return NULL;
    }
    return buf;
}

/* Runtime IPC validates the complete Windows directory ancestry, so an MSYS2
 * TEMP rooted under C:/msys64/tmp is intentionally unsuitable even when the
 * leaf made by cbm_mkdtemp() has a private DACL. Keep these security-sensitive
 * fixtures under LocalAppData on Windows; preserve the ordinary temporary-root
 * behavior on POSIX. */
static inline bool th_secure_runtime_parent_new(char *out, size_t out_cap, const char *tag) {
    if (!out || out_cap == 0 || !tag || !tag[0]) {
        return false;
    }
#ifdef _WIN32
    const char *base = cbm_app_local_dir();
    if (!base || !base[0]) {
        out[0] = '\0';
        return false;
    }
    /* Named-pipe IPC addresses carry no sun_path limit; keep the full tag. */
    int written = snprintf(out, out_cap, "%s/cbm-runtime-%s-XXXXXX", base, tag);
#else
    const char *base = cbm_tmpdir();
    if (!base || !base[0]) {
        out[0] = '\0';
        return false;
    }
    /* The POSIX rendezvous address is
     *   <canonical parent>/memory-for-ai-daemon-<uid>/mfa-<key>.sock
     * and sockaddr_un::sun_path holds at most 104 bytes on macOS (108 on
     * Linux), while /tmp canonicalizes to /private/tmp on macOS (+8 bytes).
     * A fixture tag long enough to push the composed address to the cap makes
     * cbm_daemon_ipc_endpoint_new fail its unix_address_set check, so clip the
     * tag — debug-only decoration that cbm_mkdtemp's random suffix already
     * disambiguates — to the largest length that fits on this platform. */
    char uid_text[24];
    int uid_length = snprintf(uid_text, sizeof(uid_text), "%lu", (unsigned long)geteuid());
    size_t canonical_base = strlen(base);
    size_t sun_cap = 108;
#ifdef __APPLE__
    canonical_base += strlen("private/");
    sun_cap = 104;
#endif
    size_t fixed = strlen("/cbm-runtime--XXXXXX") + strlen("/memory-for-ai-daemon-") +
                   (uid_length > 0 ? (size_t)uid_length : (size_t)1) +
                   strlen("/mfa-0123456789abcdef.sock");
    size_t tag_cap = sun_cap - 1 - canonical_base - fixed;
    size_t tag_length = strlen(tag);
    if (tag_cap < 1) {
        tag_cap = 1;
    }
    if (tag_length > tag_cap) {
        tag_length = tag_cap;
    }
    int written = snprintf(out, out_cap, "%s/cbm-runtime-%.*s-XXXXXX", base, (int)tag_length, tag);
#endif
    if (written <= 0 || (size_t)written >= out_cap) {
        out[0] = '\0';
        return false;
    }
    return cbm_mkdtemp(out) != NULL;
}

/* ── File permissions (no-op on Windows) ──────────────────────── */

/* Make a file executable. On Windows this is a no-op (all files are
 * "executable" if they have the right extension). */
static inline void th_make_executable(const char *path) {
#ifndef _WIN32
    chmod(path, 0755);
#else
    (void)path;
#endif
}

/* ── Cleanup helper ───────────────────────────────────────────── */

/* Remove a temp directory tree. Safe to call with NULL. */
static inline void th_cleanup(const char *path) {
    if (path && path[0]) {
        th_rmtree(path);
    }
}

#endif /* TEST_HELPERS_H */
