/*
 * pass_project_graph.c — build, test, dependency, and local security graph.
 *
 * This pass deliberately consumes repository manifests as evidence. It does
 * not execute package managers, compilers, test runners, or network-backed
 * vulnerability scanners. That keeps indexing deterministic and safe while
 * making the relationships available to query_graph and get_architecture.
 */
#include "foundation/constants.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/compat_fs.h"
#include "foundation/limits.h"
#include "foundation/log.h"
#include "foundation/str_util.h"
#include <yyjson/yyjson.h>

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLEN(s) (sizeof(s) - SKIP_ONE)

enum {
    PG_MAX_SOURCE = 2 * 1024 * 1024,
    PG_MAX_TARGETS = 256,
    PG_MAX_TEST_SUITES = 512,
    PG_MAX_AUDIT_IDS = 128,
    PG_MAX_TOKEN = CBM_SZ_256,
};

typedef struct {
    int64_t id;
    char name[PG_MAX_TOKEN];
    bool test_target;
} pg_target_t;

static const char *pg_itoa(int value) {
    static char buffers[CBM_SZ_4][CBM_SZ_32];
    static int index = 0;
    int slot = index;
    index = (index + SKIP_ONE) & (CBM_SZ_4 - SKIP_ONE);
    snprintf(buffers[slot], sizeof(buffers[slot]), "%d", value);
    return buffers[slot];
}

static const char *pg_basename(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + SKIP_ONE : (path ? path : "");
}

static bool pg_eq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static bool pg_contains(const char *s, const char *needle) {
    return s && needle && strstr(s, needle) != NULL;
}

static void pg_trim(char *s) {
    if (!s) {
        return;
    }
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + SKIP_ONE);
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - SKIP_ONE])) {
        s[--n] = '\0';
    }
}

static void pg_copy_token(char *out, size_t out_sz, const char *src) {
    if (!out || out_sz == 0) {
        return;
    }
    out[0] = '\0';
    if (!src) {
        return;
    }
    while (*src && isspace((unsigned char)*src)) {
        src++;
    }
    char quote = '\0';
    if (*src == '\'' || *src == '"') {
        quote = *src++;
    }
    size_t n = 0;
    while (src[n] && !isspace((unsigned char)src[n]) && src[n] != ',' && src[n] != ')' &&
           src[n] != ']' && src[n] != '}' && src[n] != '=' && src[n] != '\r' && src[n] != '\n' &&
           n + SKIP_ONE < out_sz) {
        if (quote != '\0' && src[n] == quote) {
            break;
        }
        out[n] = src[n];
        n++;
    }
    out[n] = '\0';
}

static bool pg_is_test_target(const char *name) {
    if (!name) {
        return false;
    }
    char lower[PG_MAX_TOKEN];
    size_t n = strlen(name);
    if (n >= sizeof(lower)) {
        n = sizeof(lower) - SKIP_ONE;
    }
    for (size_t i = 0; i < n; i++) {
        lower[i] = (char)tolower((unsigned char)name[i]);
    }
    lower[n] = '\0';
    return strstr(lower, "test") != NULL || strstr(lower, "check") != NULL ||
           strstr(lower, "verify") != NULL || strstr(lower, "lint") != NULL;
}

static char *pg_read_file(const char *path, int *out_len) {
    if (!path || !out_len) {
        return NULL;
    }
    FILE *f = cbm_fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size <= 0 || size > PG_MAX_SOURCE || size > cbm_max_file_bytes() || size > (long)INT_MAX) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *source = malloc((size_t)size + SKIP_ONE);
    if (!source) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(source, SKIP_ONE, (size_t)size, f);
    fclose(f);
    source[got] = '\0';
    *out_len = (int)got;
    return source;
}

static const cbm_gbuf_node_t *pg_file_node(cbm_pipeline_ctx_t *ctx, const char *rel) {
    char *qn = cbm_pipeline_fqn_compute(ctx->project_name, rel, "__file__");
    const cbm_gbuf_node_t *node = qn ? cbm_gbuf_find_by_qn(ctx->gbuf, qn) : NULL;
    free(qn);
    return node;
}

/* Discovery deliberately keeps selected JSON files out of source extraction.
 * Project-graph evidence still needs a File node and a bounded path back to the
 * manifest, so validate the relative path before joining it to repo_path. */
static bool pg_safe_relative_path(const char *rel) {
    if (!rel || !rel[0] || rel[0] == '/' || rel[0] == '\\') {
        return false;
    }
    const char *component = rel;
    for (const char *p = rel;; p++) {
        if (*p == '\\') {
            return false;
        }
        if (*p == '/' || *p == '\0') {
            size_t length = (size_t)(p - component);
            if (length == 2 && component[0] == '.' && component[1] == '.') {
                return false;
            }
            if (*p == '\0') {
                break;
            }
            component = p + SKIP_ONE;
        }
    }
    return true;
}

static bool pg_ensure_file_node(cbm_pipeline_ctx_t *ctx, const char *rel) {
    if (!ctx || !ctx->gbuf || !pg_safe_relative_path(rel)) {
        return false;
    }
    if (pg_file_node(ctx, rel)) {
        return true;
    }
    char *file_qn = cbm_pipeline_fqn_compute(ctx->project_name, rel, "__file__");
    if (!file_qn) {
        return false;
    }
    const char *basename = pg_basename(rel);
    const char *ext = strrchr(basename, '.');
    char props[CBM_SZ_256];
    snprintf(props, sizeof(props), "{\"extension\":\"%s\",\"source\":\"ignored-manifest\"}",
             ext ? ext : "");
    int64_t id = cbm_gbuf_upsert_node(ctx->gbuf, "File", basename, file_qn, rel, 0, 0, props);
    free(file_qn);
    return id > 0;
}

static char *pg_absolute_path(const char *repo_path, const char *rel) {
    if (!repo_path || !repo_path[0] || !pg_safe_relative_path(rel)) {
        return NULL;
    }
    size_t repo_len = strlen(repo_path);
    size_t rel_len = strlen(rel);
    bool trailing_separator =
        repo_path[repo_len - SKIP_ONE] == '/' || repo_path[repo_len - SKIP_ONE] == '\\';
    size_t separator_len = trailing_separator ? 0 : SKIP_ONE;
    if (repo_len > SIZE_MAX - rel_len - separator_len - SKIP_ONE) {
        return NULL;
    }
    size_t total = repo_len + separator_len + rel_len + SKIP_ONE;
    char *path = (char *)malloc(total);
    if (!path) {
        return NULL;
    }
    memcpy(path, repo_path, repo_len);
    size_t offset = repo_len;
    if (!trailing_separator) {
        path[offset++] = '/';
    }
    memcpy(path + offset, rel, rel_len);
    path[offset + rel_len] = '\0';
    return path;
}

static void pg_json_props(char *out, size_t out_sz, const char *kind, const char *scope,
                          const char *version) {
    char e_kind[PG_MAX_TOKEN];
    char e_scope[PG_MAX_TOKEN];
    char e_version[PG_MAX_TOKEN];
    cbm_json_escape(e_kind, sizeof(e_kind), kind ? kind : "unknown");
    cbm_json_escape(e_scope, sizeof(e_scope), scope ? scope : "runtime");
    cbm_json_escape(e_version, sizeof(e_version), version ? version : "");
    snprintf(out, out_sz,
             "{\"source\":\"manifest\",\"ecosystem\":\"%s\","
             "\"scope\":\"%s\",\"version\":\"%s\",\"direct\":true}",
             e_kind, e_scope, e_version);
}

static int pg_emit_dependency(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *src, const char *rel,
                              const char *ecosystem, const char *name, const char *version,
                              const char *scope) {
    if (!src || !name || !name[0]) {
        return 0;
    }
    char safe_name[PG_MAX_TOKEN];
    pg_copy_token(safe_name, sizeof(safe_name), name);
    if (!safe_name[0]) {
        return 0;
    }
    char dep_qn[CBM_SZ_512];
    snprintf(dep_qn, sizeof(dep_qn), "%s.__%s_dep__.%s", ctx->project_name,
             ecosystem ? ecosystem : "unknown", safe_name);
    char props[CBM_SZ_1K];
    char e_ecosystem[PG_MAX_TOKEN];
    cbm_json_escape(e_ecosystem, sizeof(e_ecosystem), ecosystem ? ecosystem : "unknown");
    char e_name[PG_MAX_TOKEN];
    cbm_json_escape(e_name, sizeof(e_name), safe_name);
    pg_json_props(props, sizeof(props), ecosystem, scope, version);
    char node_props[CBM_SZ_1K];
    char e_version[PG_MAX_TOKEN];
    cbm_json_escape(e_version, sizeof(e_version), version ? version : "");
    snprintf(node_props, sizeof(node_props),
             "{\"external\":true,\"ecosystem\":\"%s\","
             "\"package\":\"%s\",\"declared_version\":\"%s\"}",
             e_ecosystem, e_name, e_version);
    int64_t dep_id =
        cbm_gbuf_upsert_node(ctx->gbuf, "Package", safe_name, dep_qn, "", 0, 0, node_props);
    if (dep_id <= 0 || dep_id == src->id) {
        return 0;
    }
    cbm_gbuf_insert_edge(ctx->gbuf, src->id, dep_id, "DEPENDS_ON", props);
    return 1;
}

static bool pg_is_dependency_section(const char *section) {
    return pg_contains(section, "dependencies") || pg_contains(section, "dev-dependencies") ||
           pg_contains(section, "devDependencies") || pg_contains(section, "peerDependencies") ||
           pg_contains(section, "optionalDependencies") ||
           pg_contains(section, "build-dependencies") ||
           pg_contains(section, "workspace.dependencies");
}

static const char *pg_scope(const char *section) {
    if (section && (pg_contains(section, "dev") || pg_contains(section, "test"))) {
        return "dev";
    }
    if (section && pg_contains(section, "build")) {
        return "build";
    }
    return "runtime";
}

/* Parse the line-oriented dependency tables used by Cargo/TOML, package
 * manifests, and simple INI-style project files. */
static int pg_parse_key_value_sections(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *src,
                                       const char *rel, const char *source, const char *ecosystem) {
    int count = 0;
    char section[PG_MAX_TOKEN] = "";
    bool list_mode = false;
    const char *p = source;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[CBM_SZ_1K];
        size_t cp = len < sizeof(line) - SKIP_ONE ? len : sizeof(line) - SKIP_ONE;
        memcpy(line, p, cp);
        line[cp] = '\0';
        pg_trim(line);
        if (line[0] == '[') {
            snprintf(section, sizeof(section), "%s", line);
            list_mode = false;
        } else if (line[0] != '#' && line[0] != ';' && line[0] != '\0') {
            char key[PG_MAX_TOKEN];
            char value[PG_MAX_TOKEN];
            const char *eq = strchr(line, '=');
            if (eq) {
                size_t klen = (size_t)(eq - line);
                if (klen >= sizeof(key)) {
                    klen = sizeof(key) - SKIP_ONE;
                }
                memcpy(key, line, klen);
                key[klen] = '\0';
                pg_trim(key);
                pg_copy_token(value, sizeof(value), eq + SKIP_ONE);
                bool is_list =
                    strcmp(key, "dependencies") == 0 && strchr(eq + SKIP_ONE, '[') != NULL;
                if (pg_is_dependency_section(section) && !is_list) {
                    count +=
                        pg_emit_dependency(ctx, src, rel, ecosystem, key, value, pg_scope(section));
                }
                if (is_list) {
                    const char *item = strchr(eq + SKIP_ONE, '[');
                    item = item ? item + SKIP_ONE : eq + SKIP_ONE;
                    while (*item && *item != ']') {
                        while (*item && *item != '\'' && *item != '"' && *item != ']') {
                            item++;
                        }
                        if (*item == ']') {
                            break;
                        }
                        char item_value[PG_MAX_TOKEN];
                        pg_copy_token(item_value, sizeof(item_value), item);
                        count += pg_emit_dependency(ctx, src, rel, ecosystem, item_value, "",
                                                    pg_scope(section));
                        char quote = *item++;
                        while (*item && *item != quote) {
                            item++;
                        }
                        if (*item) {
                            item++;
                        }
                    }
                    list_mode = true;
                }
            } else if (list_mode && line[0] == '"') {
                char value[PG_MAX_TOKEN];
                pg_copy_token(value, sizeof(value), line + SKIP_ONE);
                count += pg_emit_dependency(ctx, src, rel, ecosystem, value, "", pg_scope(section));
            }
            if (list_mode && strchr(line, ']') != NULL) {
                list_mode = false;
            }
        }
        if (!eol) {
            break;
        }
        p = eol + SKIP_ONE;
    }
    return count;
}

static int pg_parse_python_setup(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *src,
                                 const char *rel, const char *source) {
    int count = 0;
    bool in_install_requires = false;
    const char *p = source;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[CBM_SZ_1K];
        size_t cp = len < sizeof(line) - SKIP_ONE ? len : sizeof(line) - SKIP_ONE;
        memcpy(line, p, cp);
        line[cp] = '\0';
        if (strstr(line, "install_requires") && strchr(line, '[')) {
            in_install_requires = true;
        }
        if (in_install_requires) {
            const char *item = line;
            const char *
                requires
            = strstr(line, "install_requires");
            if (requires) {
                const char *open = strchr(requires, '[');
                item = open ? open + SKIP_ONE : line;
            }
            while ((item = strpbrk(item, "'\"")) != NULL) {
                char value[PG_MAX_TOKEN];
                pg_copy_token(value, sizeof(value), item);
                count += pg_emit_dependency(ctx, src, rel, "pypi", value, "", "runtime");
                char quote = *item++;
                while (*item && *item != quote) {
                    item++;
                }
                if (*item) {
                    item++;
                }
            }
            if (strchr(line, ']')) {
                in_install_requires = false;
            }
        }
        if (!eol) {
            break;
        }
        p = eol + SKIP_ONE;
    }
    return count;
}

/* Parse the dependency maps in Dart's pubspec.yaml.  YAML values may be
 * scalars or nested maps (sdk/path/git); the dependency key itself is the
 * stable package identity, so emit it once at the first two-space entry and
 * deliberately ignore nested option keys. */
static int pg_parse_pubspec(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *src, const char *rel,
                            const char *source) {
    int count = 0;
    char section[PG_MAX_TOKEN] = "";
    const char *p = source;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[CBM_SZ_1K];
        size_t cp = len < sizeof(line) - SKIP_ONE ? len : sizeof(line) - SKIP_ONE;
        memcpy(line, p, cp);
        line[cp] = '\0';
        size_t indent = 0;
        while (line[indent] == ' ' || line[indent] == '\t') {
            indent++;
        }
        pg_trim(line);
        if (line[0] == '#' || line[0] == '\0') {
            /* no-op */
        } else if (indent == 0 && line[strlen(line) - SKIP_ONE] == ':') {
            line[strlen(line) - SKIP_ONE] = '\0';
            snprintf(section, sizeof(section), "%s", line);
        } else if (indent == 2 &&
                   (pg_eq(section, "dependencies") || pg_eq(section, "dev_dependencies") ||
                    pg_eq(section, "dependency_overrides"))) {
            char *colon = strchr(line, ':');
            if (colon) {
                *colon = '\0';
                pg_trim(line);
                if (line[0] && !pg_eq(line, "sdk") && !pg_eq(line, "path") && !pg_eq(line, "git") &&
                    !pg_eq(line, "hosted")) {
                    char version[PG_MAX_TOKEN];
                    pg_copy_token(version, sizeof(version), colon + SKIP_ONE);
                    count += pg_emit_dependency(ctx, src, rel, "pub", line, version,
                                                pg_eq(section, "dependencies") ? "runtime" : "dev");
                }
            }
        }
        if (!eol) {
            break;
        }
        p = eol + SKIP_ONE;
    }
    return count;
}

/* Parse Hex dependencies from mix.exs without evaluating Elixir.  Only literal
 * atoms inside deps: [...] are accepted; computed expressions are ignored. */
static int pg_parse_mix(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *src, const char *rel,
                        const char *source) {
    int count = 0;
    bool in_deps = false;
    const char *p = source;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[CBM_SZ_1K];
        size_t cp = len < sizeof(line) - SKIP_ONE ? len : sizeof(line) - SKIP_ONE;
        memcpy(line, p, cp);
        line[cp] = '\0';
        if (strstr(line, "defp deps") || strstr(line, "def deps")) {
            in_deps = true;
        }
        if (in_deps) {
            const char *item = line;
            while ((item = strchr(item, '{')) != NULL) {
                if (item[SKIP_ONE] == ':') {
                    char name[PG_MAX_TOKEN];
                    pg_copy_token(name, sizeof(name), item + 2);
                    if (name[0]) {
                        count += pg_emit_dependency(ctx, src, rel, "hex", name, "", "runtime");
                    }
                }
                item += SKIP_ONE;
            }
            if (strchr(line, ']')) {
                in_deps = false;
            }
        }
        if (!eol) {
            break;
        }
        p = eol + SKIP_ONE;
    }
    return count;
}

/* Parse literal SwiftPM package URLs without executing Package.swift. The
 * package identity is the final URL component (with the conventional .git
 * suffix removed); path-only packages are intentionally not external
 * dependencies. */
static int pg_parse_swiftpm(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *src, const char *rel,
                            const char *source) {
    int count = 0;
    const char *p = source;
    while (p && *p) {
        const char *package = strstr(p, ".package(");
        if (!package) {
            break;
        }
        const char *url_key = strstr(package, "url:");
        const char *next = strchr(package, ')');
        if (!url_key || (next && url_key > next)) {
            p = package + SLEN(".package(");
            continue;
        }
        const char *quote = strpbrk(url_key + SLEN("url:"), "\"'");
        if (!quote || (next && quote > next)) {
            p = package + SLEN(".package(");
            continue;
        }
        char url[PG_MAX_TOKEN];
        pg_copy_token(url, sizeof(url), quote);
        char *name = strrchr(url, '/');
        name = name ? name + SKIP_ONE : url;
        char *query = strpbrk(name, "?#");
        if (query) {
            *query = '\0';
        }
        size_t name_len = strlen(name);
        if (name_len > SLEN(".git") && strcmp(name + name_len - SLEN(".git"), ".git") == 0) {
            name[name_len - SLEN(".git")] = '\0';
        }
        char version[PG_MAX_TOKEN] = "";
        const char *from = strstr(url_key, "from:");
        if (from && (!next || from < next)) {
            pg_copy_token(version, sizeof(version), from + SLEN("from:"));
        }
        if (name[0]) {
            count += pg_emit_dependency(ctx, src, rel, "swiftpm", name, version, "runtime");
        }
        p = next ? next + SKIP_ONE : package + SLEN(".package(");
    }
    return count;
}

/* Parse literal Ruby gem declarations from a .gemspec. */
static int pg_parse_gemspec(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *src, const char *rel,
                            const char *source) {
    int count = 0;
    const char *p = source;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[CBM_SZ_1K];
        size_t cp = len < sizeof(line) - SKIP_ONE ? len : sizeof(line) - SKIP_ONE;
        memcpy(line, p, cp);
        line[cp] = '\0';
        bool development = strstr(line, "add_development_dependency") != NULL;
        if (strstr(line, "add_dependency") || strstr(line, "add_runtime_dependency") ||
            development) {
            const char *quote = strchr(line, '\'');
            if (!quote) {
                quote = strchr(line, '"');
            }
            if (quote) {
                char name[PG_MAX_TOKEN];
                pg_copy_token(name, sizeof(name), quote + SKIP_ONE);
                count += pg_emit_dependency(ctx, src, rel, "rubygems", name, "",
                                            development ? "dev" : "runtime");
            }
        }
        if (!eol) {
            break;
        }
        p = eol + SKIP_ONE;
    }
    return count;
}

/* Parse JSON dependency sections without treating the package's own `name`
 * field as a dependency. Using the vendored parser handles both pretty and
 * minified manifests, comments, and trailing commas without guessing from
 * line layout. */
static int pg_parse_json_sections(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *src,
                                  const char *rel, const char *source, const char *ecosystem) {
    int count = 0;
    yyjson_read_flag flags = YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS;
    yyjson_doc *doc = yyjson_read(source, strlen(source), flags);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return 0;
    }

    static const char *const sections[] = {
        "dependencies",        "devDependencies", "peerDependencies", "optionalDependencies",
        "bundledDependencies", "require",         "require-dev",      NULL};
    for (int i = 0; sections[i]; i++) {
        yyjson_val *section = yyjson_obj_get(root, sections[i]);
        if (!section) {
            continue;
        }
        if (yyjson_is_arr(section)) {
            size_t idx, max;
            yyjson_val *item;
            yyjson_arr_foreach(section, idx, max, item) {
                const char *name = yyjson_get_str(item);
                if (name && name[0]) {
                    count += pg_emit_dependency(ctx, src, rel, ecosystem, name, "",
                                                pg_scope(sections[i]));
                }
            }
        } else if (yyjson_is_obj(section)) {
            yyjson_obj_iter iter;
            yyjson_obj_iter_init(section, &iter);
            yyjson_val *key;
            while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
                const char *name = yyjson_get_str(key);
                yyjson_val *value = yyjson_obj_iter_get_val(key);
                const char *version = yyjson_get_str(value);
                if (name && name[0]) {
                    count += pg_emit_dependency(ctx, src, rel, ecosystem, name,
                                                version ? version : "", pg_scope(sections[i]));
                }
            }
        }
    }
    yyjson_doc_free(doc);
    return count;
}

static int pg_parse_requirements(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *src,
                                 const char *rel, const char *source) {
    int count = 0;
    const char *p = source;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[CBM_SZ_1K];
        size_t cp = len < sizeof(line) - SKIP_ONE ? len : sizeof(line) - SKIP_ONE;
        memcpy(line, p, cp);
        line[cp] = '\0';
        pg_trim(line);
        if (line[0] && line[0] != '#' && line[0] != '-' && !strstr(line, "://")) {
            char name[PG_MAX_TOKEN];
            char version[PG_MAX_TOKEN];
            const char *split = line;
            while (*split && *split != '=' && *split != '<' && *split != '>' && *split != '!' &&
                   *split != '~' && *split != '[' && !isspace((unsigned char)*split)) {
                split++;
            }
            size_t name_len = (size_t)(split - line);
            if (name_len >= sizeof(name)) {
                name_len = sizeof(name) - SKIP_ONE;
            }
            memcpy(name, line, name_len);
            name[name_len] = '\0';
            const char *version_src = split;
            while (*version_src == '=' || *version_src == '<' || *version_src == '>' ||
                   *version_src == '!' || *version_src == '~' ||
                   isspace((unsigned char)*version_src)) {
                version_src++;
            }
            pg_copy_token(version, sizeof(version), version_src);
            count += pg_emit_dependency(ctx, src, rel, "pypi", name, version, "runtime");
        }
        if (!eol) {
            break;
        }
        p = eol + SKIP_ONE;
    }
    return count;
}

static int pg_parse_go_mod(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *src, const char *rel,
                           const char *source) {
    int count = 0;
    bool in_require = false;
    const char *p = source;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[CBM_SZ_1K];
        size_t cp = len < sizeof(line) - SKIP_ONE ? len : sizeof(line) - SKIP_ONE;
        memcpy(line, p, cp);
        line[cp] = '\0';
        pg_trim(line);
        if (line[0] == '#' || line[0] == '\0') {
            /* comments and blank lines do not carry module requirements */
        } else if (strncmp(line, "require (", SLEN("require (")) == 0 ||
                   strcmp(line, "require(") == 0) {
            in_require = true;
        } else if (in_require && line[0] == ')') {
            in_require = false;
        } else if (in_require || strncmp(line, "require ", SLEN("require ")) == 0) {
            const char *entry = line;
            if (strncmp(entry, "require ", SLEN("require ")) == 0) {
                entry += SLEN("require ");
            }
            char module[PG_MAX_TOKEN];
            char version[PG_MAX_TOKEN];
            pg_copy_token(module, sizeof(module), entry);
            while (*entry && !isspace((unsigned char)*entry)) {
                entry++;
            }
            while (*entry && isspace((unsigned char)*entry)) {
                entry++;
            }
            pg_copy_token(version, sizeof(version), entry);
            if (module[0] && module[0] != ')' && strcmp(module, "module") != 0 &&
                strcmp(module, "go") != 0 && strcmp(module, "toolchain") != 0 &&
                strcmp(module, "replace") != 0 && strcmp(module, "exclude") != 0 &&
                strcmp(module, "retract") != 0) {
                count += pg_emit_dependency(ctx, src, rel, "gomod", module, version, "runtime");
            }
        }
        if (!eol) {
            break;
        }
        p = eol + SKIP_ONE;
    }
    return count;
}

static int pg_parse_gradle(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *src, const char *rel,
                           const char *source) {
    int count = 0;
    const char *p = source;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[CBM_SZ_1K];
        size_t cp = len < sizeof(line) - SKIP_ONE ? len : sizeof(line) - SKIP_ONE;
        memcpy(line, p, cp);
        line[cp] = '\0';
        pg_trim(line);
        const char *quote = strchr(line, '\'');
        if (!quote) {
            quote = strchr(line, '"');
        }
        if (quote && (pg_contains(line, "implementation") || pg_contains(line, "api(") ||
                      pg_contains(line, "testImplementation") || pg_contains(line, "runtimeOnly") ||
                      pg_contains(line, "compileOnly"))) {
            char dep[PG_MAX_TOKEN];
            pg_copy_token(dep, sizeof(dep), quote + SKIP_ONE);
            count += pg_emit_dependency(ctx, src, rel, "maven", dep, "",
                                        pg_contains(line, "test") ? "dev" : "runtime");
        }
        if (!eol) {
            break;
        }
        p = eol + SKIP_ONE;
    }
    return count;
}

static int pg_parse_gemfile(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *src, const char *rel,
                            const char *source) {
    int count = 0;
    const char *p = source;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[CBM_SZ_1K];
        size_t cp = len < sizeof(line) - SKIP_ONE ? len : sizeof(line) - SKIP_ONE;
        memcpy(line, p, cp);
        line[cp] = '\0';
        pg_trim(line);
        if (strncmp(line, "gem ", SLEN("gem ")) == 0 || strncmp(line, "gem(", SLEN("gem(")) == 0) {
            const char *quote = strchr(line, '\'');
            if (!quote) {
                quote = strchr(line, '"');
            }
            char dep[PG_MAX_TOKEN];
            pg_copy_token(dep, sizeof(dep), quote ? quote + SKIP_ONE : "");
            count += pg_emit_dependency(ctx, src, rel, "rubygems", dep, "", "runtime");
        }
        if (!eol) {
            break;
        }
        p = eol + SKIP_ONE;
    }
    return count;
}

static int pg_parse_pom(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *src, const char *rel,
                        const char *source) {
    int count = 0;
    const char *p = source;
    bool in_dependency = false;
    char artifact[PG_MAX_TOKEN] = "";
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[CBM_SZ_1K];
        size_t cp = len < sizeof(line) - SKIP_ONE ? len : sizeof(line) - SKIP_ONE;
        memcpy(line, p, cp);
        line[cp] = '\0';
        if (strstr(line, "<dependency>") != NULL) {
            in_dependency = true;
            artifact[0] = '\0';
        }
        if (in_dependency) {
            const char *a = strstr(line, "<artifactId>");
            if (a) {
                a += SLEN("<artifactId>");
                const char *end = strstr(a, "</artifactId>");
                if (end) {
                    size_t n = (size_t)(end - a);
                    if (n >= sizeof(artifact)) {
                        n = sizeof(artifact) - SKIP_ONE;
                    }
                    memcpy(artifact, a, n);
                    artifact[n] = '\0';
                    pg_trim(artifact);
                }
            }
            if (strstr(line, "</dependency>") != NULL) {
                count += pg_emit_dependency(ctx, src, rel, "maven", artifact, "", "runtime");
                in_dependency = false;
            }
        }
        if (!eol) {
            break;
        }
        p = eol + SKIP_ONE;
    }
    return count;
}

static bool pg_build_manifest(const char *rel, const char *base, const char **kind) {
    if (pg_eq(base, "Makefile") || pg_eq(base, "GNUmakefile") || pg_eq(base, "makefile")) {
        *kind = "make";
        return true;
    }
    if (pg_eq(base, "CMakeLists.txt")) {
        *kind = "cmake";
        return true;
    }
    if (pg_eq(base, "meson.build")) {
        *kind = "meson";
        return true;
    }
    if (pg_eq(base, "package.json")) {
        *kind = "npm";
        return true;
    }
    if (pg_eq(base, "composer.json")) {
        *kind = "composer";
        return true;
    }
    if (pg_eq(base, "Cargo.toml")) {
        *kind = "cargo";
        return true;
    }
    if (pg_eq(base, "pubspec.yaml")) {
        *kind = "pub";
        return true;
    }
    if (pg_eq(base, "pyproject.toml") || pg_eq(base, "setup.py") || pg_eq(base, "tox.ini") ||
        pg_eq(base, "pytest.ini") || pg_eq(base, "requirements.txt") ||
        (strncmp(base, "requirements-", SLEN("requirements-")) == 0 &&
         strstr(base, ".txt") != NULL)) {
        *kind = "python";
        return true;
    }
    if (pg_eq(base, "pom.xml")) {
        *kind = "maven";
        return true;
    }
    if (pg_eq(base, "build.gradle") || pg_eq(base, "build.gradle.kts")) {
        *kind = "gradle";
        return true;
    }
    if (pg_eq(base, "Gemfile")) {
        *kind = "rubygems";
        return true;
    }
    if (pg_eq(base, "mix.exs")) {
        *kind = "hex";
        return true;
    }
    size_t base_len = strlen(base);
    if (base_len >= SLEN(".gemspec") &&
        strcmp(base + base_len - SLEN(".gemspec"), ".gemspec") == 0) {
        *kind = "rubygems";
        return true;
    }
    if (pg_eq(base, "Package.swift")) {
        *kind = "swiftpm";
        return true;
    }
    if (pg_eq(base, "go.mod")) {
        *kind = "gomod";
        return true;
    }
    if (rel && (strncmp(rel, ".github/workflows/", SLEN(".github/workflows/")) == 0 ||
                strncmp(rel, ".github\\workflows\\", SLEN(".github\\workflows\\")) == 0)) {
        *kind = "github-actions";
        return true;
    }
    if (strncmp(base, "Dockerfile", SLEN("Dockerfile")) == 0) {
        *kind = "docker";
        return true;
    }
    return false;
}

static int pg_add_target(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *src, const char *rel,
                         const char *kind, const char *name, int line, pg_target_t *targets,
                         int *target_count) {
    if (!name || !name[0] || !targets || !target_count || *target_count >= PG_MAX_TARGETS) {
        return 0;
    }
    char target_name[PG_MAX_TOKEN];
    pg_copy_token(target_name, sizeof(target_name), name);
    if (!target_name[0] || strcmp(target_name, ".PHONY") == 0 || strcmp(target_name, "all") == 0) {
        return 0;
    }
    for (int i = 0; i < *target_count; i++) {
        if (strcmp(targets[i].name, target_name) == 0) {
            return 0;
        }
    }
    char qn[CBM_SZ_1K];
    snprintf(qn, sizeof(qn), "%s.__build__.%s.%s", ctx->project_name, kind, target_name);
    char e_kind[PG_MAX_TOKEN];
    char e_name[PG_MAX_TOKEN];
    cbm_json_escape(e_kind, sizeof(e_kind), kind);
    cbm_json_escape(e_name, sizeof(e_name), target_name);
    char props[CBM_SZ_1K];
    snprintf(props, sizeof(props), "{\"kind\":\"%s\",\"test_target\":%s,\"source\":\"manifest\"}",
             e_kind, pg_is_test_target(target_name) ? "true" : "false");
    int64_t id =
        cbm_gbuf_upsert_node(ctx->gbuf, "BuildTarget", target_name, qn, rel, line, line, props);
    if (id <= 0) {
        return 0;
    }
    targets[*target_count].id = id;
    snprintf(targets[*target_count].name, sizeof(targets[*target_count].name), "%s", target_name);
    targets[*target_count].test_target = pg_is_test_target(target_name);
    (*target_count)++;
    if (src->id != id) {
        cbm_gbuf_insert_edge(ctx->gbuf, src->id, id, "BUILDS", "{\"evidence\":\"manifest\"}");
    }
    return 1;
}

static void pg_make_targets(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *src, const char *rel,
                            const char *source, const char *kind, pg_target_t *targets,
                            int *target_count) {
    const char *p = source;
    int line_no = 1;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[CBM_SZ_1K];
        size_t cp = len < sizeof(line) - SKIP_ONE ? len : sizeof(line) - SKIP_ONE;
        memcpy(line, p, cp);
        line[cp] = '\0';
        bool recipe = line[0] == '\t' || line[0] == ' ';
        pg_trim(line);
        if (!recipe && line[0] && line[0] != '#' && line[0] != '.') {
            char *colon = strchr(line, ':');
            if (colon && colon[SKIP_ONE] != '=') {
                *colon = '\0';
                pg_trim(line);
                pg_add_target(ctx, src, rel, kind, line, line_no, targets, target_count);
            }
        }
        if (!eol) {
            break;
        }
        p = eol + SKIP_ONE;
        line_no++;
    }

    /* A second bounded pass resolves only prerequisites that are themselves
     * named targets. Shell variables and filenames stay unlinked rather than
     * becoming false graph nodes. */
    p = source;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[CBM_SZ_1K];
        size_t cp = len < sizeof(line) - SKIP_ONE ? len : sizeof(line) - SKIP_ONE;
        memcpy(line, p, cp);
        line[cp] = '\0';
        bool recipe = line[0] == '\t' || line[0] == ' ';
        pg_trim(line);
        if (!recipe && line[0] && line[0] != '#' && line[0] != '.') {
            char *colon = strchr(line, ':');
            if (colon && colon[SKIP_ONE] != '=') {
                *colon = '\0';
                pg_trim(line);
                int target_index = -1;
                for (int i = 0; i < *target_count; i++) {
                    if (strcmp(targets[i].name, line) == 0) {
                        target_index = i;
                        break;
                    }
                }
                if (target_index >= 0) {
                    const char *dep = colon + SKIP_ONE;
                    char dep_name[PG_MAX_TOKEN];
                    while (*dep) {
                        while (*dep && isspace((unsigned char)*dep)) {
                            dep++;
                        }
                        if (!*dep) {
                            break;
                        }
                        pg_copy_token(dep_name, sizeof(dep_name), dep);
                        for (int i = 0; i < *target_count; i++) {
                            if (strcmp(targets[i].name, dep_name) == 0 && i != target_index) {
                                cbm_gbuf_insert_edge(ctx->gbuf, targets[target_index].id,
                                                     targets[i].id, "DEPENDS_ON",
                                                     "{\"source\":\"build-target\"}");
                            }
                        }
                        while (*dep && !isspace((unsigned char)*dep)) {
                            dep++;
                        }
                    }
                }
            }
        }
        if (!eol) {
            break;
        }
        p = eol + SKIP_ONE;
    }
}

static void pg_cmake_targets(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *src, const char *rel,
                             const char *source, const char *kind, pg_target_t *targets,
                             int *target_count) {
    static const char *const cmake_calls[] = {"add_executable(", "add_library(",
                                              "add_custom_target(", NULL};
    static const char *const meson_calls[] = {
        "executable(", "library(", "shared_library(", "static_library(", "custom_target(",
        "test(",       NULL};
    const char *const *calls = strcmp(kind, "meson") == 0 ? meson_calls : cmake_calls;
    const char *p = source;
    int line_no = 1;
    while (p && *p) {
        for (int i = 0; calls[i]; i++) {
            const char *hit = strstr(p, calls[i]);
            if (hit) {
                pg_add_target(ctx, src, rel, kind, hit + strlen(calls[i]), line_no, targets,
                              target_count);
            }
        }
        const char *eol = strchr(p, '\n');
        if (!eol) {
            break;
        }
        p = eol + SKIP_ONE;
        line_no++;
    }

    const char *pdeps = source;
    while (pdeps && *pdeps) {
        const char *hit = strstr(pdeps, "add_dependencies(");
        if (!hit) {
            break;
        }
        hit += SLEN("add_dependencies(");
        char target_name[PG_MAX_TOKEN];
        pg_copy_token(target_name, sizeof(target_name), hit);
        int target_index = -1;
        for (int i = 0; i < *target_count; i++) {
            if (strcmp(targets[i].name, target_name) == 0) {
                target_index = i;
                break;
            }
        }
        if (target_index >= 0) {
            while (*hit && *hit != ' ' && *hit != '\t' && *hit != '\n') {
                hit++;
            }
            while (*hit) {
                while (*hit && (isspace((unsigned char)*hit) || *hit == ',')) {
                    hit++;
                }
                if (!*hit || *hit == ')') {
                    break;
                }
                char dep_name[PG_MAX_TOKEN];
                pg_copy_token(dep_name, sizeof(dep_name), hit);
                for (int i = 0; i < *target_count; i++) {
                    if (strcmp(targets[i].name, dep_name) == 0 && i != target_index) {
                        cbm_gbuf_insert_edge(ctx->gbuf, targets[target_index].id, targets[i].id,
                                             "DEPENDS_ON", "{\"source\":\"build-target\"}");
                    }
                }
                while (*hit && !isspace((unsigned char)*hit) && *hit != ')') {
                    hit++;
                }
            }
        }
        pdeps = hit;
    }
}

static void pg_json_script_targets(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *src,
                                   const char *rel, const char *source, const char *kind,
                                   pg_target_t *targets, int *target_count) {
    const char *p = strstr(source, "\"scripts\"");
    if (!p) {
        return;
    }
    int line_no = 1;
    for (const char *q = source; q < p; q++) {
        if (*q == '\n') {
            line_no++;
        }
    }
    p = strchr(p, '{');
    if (!p) {
        return;
    }
    while (*p && *p != '}') {
        if (*p == '"') {
            const char *end = strchr(p + SKIP_ONE, '"');
            const char *colon = end ? strchr(end, ':') : NULL;
            if (end && colon && colon - end < 16) {
                char name[PG_MAX_TOKEN];
                size_t n = (size_t)(end - (p + SKIP_ONE));
                if (n >= sizeof(name)) {
                    n = sizeof(name) - SKIP_ONE;
                }
                memcpy(name, p + SKIP_ONE, n);
                name[n] = '\0';
                pg_add_target(ctx, src, rel, kind, name, line_no, targets, target_count);
                p = colon;
            }
        }
        if (*p == '\n') {
            line_no++;
        }
        p++;
    }
}

static int pg_docker_targets(cbm_pipeline_ctx_t *ctx, const cbm_gbuf_node_t *src, const char *rel,
                             const char *source, const char *kind, pg_target_t *targets,
                             int *target_count) {
    int dependency_count = 0;
    const char *p = source;
    int line_no = 1;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[CBM_SZ_1K];
        size_t cp = len < sizeof(line) - SKIP_ONE ? len : sizeof(line) - SKIP_ONE;
        memcpy(line, p, cp);
        line[cp] = '\0';
        pg_trim(line);
        if (strncmp(line, "FROM ", SLEN("FROM ")) == 0) {
            const char *from = line + SLEN("FROM ");
            while (strncmp(from, "--platform=", SLEN("--platform=")) == 0) {
                while (*from && !isspace((unsigned char)*from)) {
                    from++;
                }
                while (*from && isspace((unsigned char)*from)) {
                    from++;
                }
            }
            char image[PG_MAX_TOKEN];
            char stage[PG_MAX_TOKEN];
            pg_copy_token(image, sizeof(image), from);
            pg_copy_token(stage, sizeof(stage), image);
            const char *as = strstr(from, " AS ");
            if (!as) {
                as = strstr(from, " as ");
            }
            if (as) {
                pg_copy_token(stage, sizeof(stage), as + SLEN(" AS "));
            }
            pg_add_target(ctx, src, rel, kind, stage, line_no, targets, target_count);
            dependency_count += pg_emit_dependency(ctx, src, rel, "container", image, "", "build");
        }
        if (!eol) {
            break;
        }
        p = eol + SKIP_ONE;
        line_no++;
    }
    return dependency_count;
}

static bool pg_is_audit_report(const char *base) {
    return pg_eq(base, "npm-audit.json") || pg_eq(base, "osv-scanner.json") ||
           pg_eq(base, "trivy.json") || pg_eq(base, "dependency-check-report.json") ||
           pg_eq(base, "audit.json");
}

static bool pg_security_id_at(const char *p, char *out, size_t out_sz) {
    static const char *const prefixes[] = {"CVE-", "GHSA-", "OSV-", "RUSTSEC-", NULL};
    const char *start = NULL;
    for (int i = 0; prefixes[i]; i++) {
        const char *hit = strstr(p, prefixes[i]);
        if (hit && (!start || hit < start)) {
            start = hit;
        }
    }
    if (!start || !out || out_sz == 0) {
        return false;
    }
    size_t n = 0;
    while (start[n] && (isalnum((unsigned char)start[n]) || start[n] == '-') &&
           n + SKIP_ONE < out_sz) {
        out[n] = start[n];
        n++;
    }
    out[n] = '\0';
    return n > 0;
}

static int pg_process_audit_report(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *file,
                                   const char *source) {
    const cbm_gbuf_node_t *src = pg_file_node(ctx, file->rel_path);
    if (!src) {
        return 0;
    }
    char report_qn[CBM_SZ_1K];
    snprintf(report_qn, sizeof(report_qn), "%s.__security_report__.%s", ctx->project_name,
             file->rel_path);
    int64_t report =
        cbm_gbuf_upsert_node(ctx->gbuf, "SecurityReport", pg_basename(file->rel_path), report_qn,
                             file->rel_path, 1, 0, "{\"source\":\"local-audit-report\"}");
    if (report > 0 && report != src->id) {
        cbm_gbuf_insert_edge(ctx->gbuf, src->id, report, "DEFINES", "{}");
    }

    char ids[PG_MAX_AUDIT_IDS][PG_MAX_TOKEN];
    int id_count = 0;
    const char *p = source;
    while (p && *p && id_count < PG_MAX_AUDIT_IDS) {
        char id[PG_MAX_TOKEN];
        if (pg_security_id_at(p, id, sizeof(id))) {
            bool duplicate = false;
            for (int i = 0; i < id_count; i++) {
                if (strcmp(ids[i], id) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                snprintf(ids[id_count], sizeof(ids[id_count]), "%s", id);
                id_count++;
            }
            p = strstr(p, id) + strlen(id);
        } else {
            break;
        }
    }
    for (int i = 0; i < id_count; i++) {
        char e_id[PG_MAX_TOKEN];
        cbm_json_escape(e_id, sizeof(e_id), ids[i]);
        char qn[CBM_SZ_1K];
        snprintf(qn, sizeof(qn), "%s.__security_advisory__.%s", ctx->project_name, ids[i]);
        char props[CBM_SZ_512];
        snprintf(props, sizeof(props),
                 "{\"advisory_id\":\"%s\",\"severity\":\"unknown\","
                 "\"source\":\"local-audit-report\",\"verified\":false}",
                 e_id);
        int64_t advisory = cbm_gbuf_upsert_node(ctx->gbuf, "SecurityAdvisory", ids[i], qn,
                                                file->rel_path, 1, 0, props);
        if (report > 0 && advisory > 0 && report != advisory) {
            cbm_gbuf_insert_edge(ctx->gbuf, report, advisory, "REPORTS",
                                 "{\"evidence\":\"advisory-id\"}");
        }
    }
    return id_count;
}

static int pg_add_test_suites(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                              int file_count) {
    int count = 0;
    for (int i = 0; i < file_count && count < PG_MAX_TEST_SUITES; i++) {
        const char *rel = files[i].rel_path;
        if (!cbm_is_test_path(rel)) {
            continue;
        }
        const cbm_gbuf_node_t *file = pg_file_node(ctx, rel);
        if (!file) {
            continue;
        }
        char qn[CBM_SZ_1K];
        snprintf(qn, sizeof(qn), "%s.__test_suite__.%s", ctx->project_name, rel);
        char props[CBM_SZ_512];
        const char *framework = strstr(rel, ".test.") || strstr(rel, ".spec.") ? "jest-like"
                                : strstr(rel, "_test.go")                      ? "go-test"
                                : strstr(rel, ".py")                           ? "pytest-like"
                                                                               : "path-inferred";
        snprintf(props, sizeof(props), "{\"is_test\":true,\"framework\":\"%s\"}", framework);
        int64_t suite =
            cbm_gbuf_upsert_node(ctx->gbuf, "TestSuite", pg_basename(rel), qn, rel, 1, 0, props);
        if (suite > 0 && suite != file->id) {
            cbm_gbuf_insert_edge(ctx->gbuf, suite, file->id, "CONTAINS_TEST", "{}");
            count++;
        }
    }
    return count;
}

static void pg_link_build_test_targets(cbm_pipeline_ctx_t *ctx) {
    const cbm_gbuf_node_t **targets = NULL;
    const cbm_gbuf_node_t **suites = NULL;
    int target_count = 0;
    int suite_count = 0;
    if (cbm_gbuf_find_by_label(ctx->gbuf, "BuildTarget", &targets, &target_count) != 0 ||
        cbm_gbuf_find_by_label(ctx->gbuf, "TestSuite", &suites, &suite_count) != 0) {
        return;
    }
    for (int i = 0; i < target_count; i++) {
        if (!targets[i]->properties_json ||
            !strstr(targets[i]->properties_json, "\"test_target\":true")) {
            continue;
        }
        for (int j = 0; j < suite_count && j < PG_MAX_TEST_SUITES; j++) {
            cbm_gbuf_insert_edge(ctx->gbuf, targets[i]->id, suites[j]->id, "RUNS_TESTS",
                                 "{\"evidence\":\"target-name\"}");
        }
    }
}

static int pg_process_manifest(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *file,
                               const char *kind, const char *source) {
    const cbm_gbuf_node_t *src = pg_file_node(ctx, file->rel_path);
    if (!src) {
        return 0;
    }
    pg_target_t targets[PG_MAX_TARGETS];
    int target_count = 0;
    int deps = 0;
    if (strcmp(kind, "make") == 0) {
        pg_make_targets(ctx, src, file->rel_path, source, kind, targets, &target_count);
    } else if (strcmp(kind, "cmake") == 0 || strcmp(kind, "meson") == 0) {
        pg_cmake_targets(ctx, src, file->rel_path, source, kind, targets, &target_count);
    } else if (strcmp(kind, "npm") == 0) {
        pg_json_script_targets(ctx, src, file->rel_path, source, kind, targets, &target_count);
    } else if (strcmp(kind, "docker") == 0) {
        deps = pg_docker_targets(ctx, src, file->rel_path, source, kind, targets, &target_count);
    }
    if (target_count == 0) {
        char synthetic[PG_MAX_TOKEN];
        snprintf(synthetic, sizeof(synthetic), "%s:default", kind);
        pg_add_target(ctx, src, file->rel_path, kind, synthetic, 1, targets, &target_count);
    }

    if (strcmp(kind, "npm") == 0 || strcmp(kind, "composer") == 0) {
        deps = pg_parse_json_sections(ctx, src, file->rel_path, source, kind);
    } else if (strcmp(kind, "cargo") == 0 || strcmp(kind, "python") == 0) {
        deps = pg_parse_key_value_sections(ctx, src, file->rel_path, source,
                                           strcmp(kind, "cargo") == 0 ? "cargo" : "pypi");
        if (strcmp(kind, "python") == 0 &&
            (pg_eq(pg_basename(file->rel_path), "requirements.txt") ||
             strstr(pg_basename(file->rel_path), "requirements-") == pg_basename(file->rel_path))) {
            deps += pg_parse_requirements(ctx, src, file->rel_path, source);
        }
        if (strcmp(kind, "python") == 0 && pg_eq(pg_basename(file->rel_path), "setup.py")) {
            deps += pg_parse_python_setup(ctx, src, file->rel_path, source);
        }
    } else if (strcmp(kind, "pub") == 0) {
        deps = pg_parse_pubspec(ctx, src, file->rel_path, source);
    } else if (strcmp(kind, "hex") == 0) {
        deps = pg_parse_mix(ctx, src, file->rel_path, source);
    } else if (strcmp(kind, "swiftpm") == 0) {
        deps = pg_parse_swiftpm(ctx, src, file->rel_path, source);
    } else if (strcmp(kind, "gomod") == 0) {
        deps = pg_parse_go_mod(ctx, src, file->rel_path, source);
    } else if (strcmp(kind, "gradle") == 0) {
        deps = pg_parse_gradle(ctx, src, file->rel_path, source);
    } else if (strcmp(kind, "rubygems") == 0) {
        size_t base_len = strlen(pg_basename(file->rel_path));
        if (base_len >= SLEN(".gemspec") &&
            strcmp(pg_basename(file->rel_path) + base_len - SLEN(".gemspec"), ".gemspec") == 0) {
            deps = pg_parse_gemspec(ctx, src, file->rel_path, source);
        } else {
            deps = pg_parse_gemfile(ctx, src, file->rel_path, source);
        }
    } else if (strcmp(kind, "maven") == 0) {
        deps = pg_parse_pom(ctx, src, file->rel_path, source);
    }

    bool dependency_manifest =
        strcmp(kind, "npm") == 0 || strcmp(kind, "composer") == 0 || strcmp(kind, "cargo") == 0 ||
        strcmp(kind, "python") == 0 || strcmp(kind, "gomod") == 0 || strcmp(kind, "gradle") == 0 ||
        strcmp(kind, "rubygems") == 0 || strcmp(kind, "maven") == 0 || strcmp(kind, "pub") == 0 ||
        strcmp(kind, "hex") == 0 || strcmp(kind, "swiftpm") == 0 || strcmp(kind, "docker") == 0;
    if (!dependency_manifest) {
        return target_count + deps;
    }

    char audit_qn[CBM_SZ_1K];
    snprintf(audit_qn, sizeof(audit_qn), "%s.__security_audit__.%s", ctx->project_name,
             file->rel_path);
    char audit_props[CBM_SZ_512];
    snprintf(audit_props, sizeof(audit_props),
             "{\"status\":\"not_run\",\"evidence\":\"%s\","
             "\"dependency_count\":%d}",
             kind, deps);
    int64_t audit = cbm_gbuf_upsert_node(ctx->gbuf, "SecurityAudit", pg_basename(file->rel_path),
                                         audit_qn, file->rel_path, 1, 0, audit_props);
    if (audit > 0 && audit != src->id) {
        cbm_gbuf_insert_edge(ctx->gbuf, src->id, audit, "SECURITY_SCANS",
                             "{\"status\":\"not_run\"}");
        const cbm_gbuf_edge_t **dep_edges = NULL;
        int dep_edge_count = 0;
        if (cbm_gbuf_find_edges_by_source_type(ctx->gbuf, src->id, "DEPENDS_ON", &dep_edges,
                                               &dep_edge_count) == 0) {
            for (int i = 0; i < dep_edge_count; i++) {
                if (dep_edges[i]->target_id > 0) {
                    cbm_gbuf_insert_edge(ctx->gbuf, audit, dep_edges[i]->target_id, "AUDITS",
                                         "{\"status\":\"not_run\"}");
                }
            }
        }
    }
    return target_count + deps;
}

static int pg_process_candidate(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *file,
                                bool *processed) {
    if (processed) {
        *processed = false;
    }
    if (!ctx || !file || !file->rel_path) {
        return 0;
    }
    const char *base = pg_basename(file->rel_path);
    const char *kind = NULL;
    bool audit = pg_is_audit_report(base);
    if (!audit && !pg_build_manifest(file->rel_path, base, &kind)) {
        return 0;
    }
    if (!pg_ensure_file_node(ctx, file->rel_path)) {
        return 0;
    }
    int source_len = 0;
    char *source = pg_read_file(file->path, &source_len);
    if (!source) {
        return 0;
    }
    (void)source_len;
    int items = audit ? pg_process_audit_report(ctx, file, source)
                      : pg_process_manifest(ctx, file, kind, source);
    free(source);
    if (processed) {
        *processed = true;
    }
    return items;
}

int cbm_pipeline_pass_project_graph(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                                    int file_count) {
    if (!ctx || !ctx->gbuf || file_count < 0 || (file_count > 0 && !files) ||
        (file_count == 0 && (!ctx->ignored_files || ctx->ignored_count <= 0))) {
        return 0;
    }
    int manifests = 0;
    int graph_items = 0;
    for (int i = 0; i < file_count; i++) {
        if (cbm_pipeline_check_cancel(ctx)) {
            return CBM_NOT_FOUND;
        }
        bool processed = false;
        graph_items += pg_process_candidate(ctx, &files[i], &processed);
        if (processed) {
            manifests++;
        }
    }

    /* package.json and composer.json are intentionally classified as ignored
     * JSON during discovery. Re-introduce only graph-relevant manifests as
     * synthetic File evidence; explicit gitignore/cbmignore decisions remain
     * respected because they carry a different reason. */
    for (int i = 0; i < ctx->ignored_count; i++) {
        if (cbm_pipeline_check_cancel(ctx)) {
            return CBM_NOT_FOUND;
        }
        const cbm_ignored_file_t *ignored = &ctx->ignored_files[i];
        if (!ignored->rel_path || !pg_eq(ignored->reason, "ignored-json")) {
            continue;
        }
        const char *kind = NULL;
        if (!pg_build_manifest(ignored->rel_path, pg_basename(ignored->rel_path), &kind)) {
            continue;
        }
        char *path = pg_absolute_path(ctx->repo_path, ignored->rel_path);
        if (!path) {
            continue;
        }
        cbm_file_info_t manifest = {
            .path = path,
            .rel_path = ignored->rel_path,
            .language = CBM_LANG_JSON,
            .size = 0,
        };
        bool processed = false;
        graph_items += pg_process_candidate(ctx, &manifest, &processed);
        if (processed) {
            manifests++;
        }
        free(path);
    }
    int suites = pg_add_test_suites(ctx, files, file_count);
    pg_link_build_test_targets(ctx);
    cbm_log_info("pass.project_graph", "manifests", pg_itoa(manifests), "items",
                 pg_itoa(graph_items), "test_suites", pg_itoa(suites));
    return 0;
}
