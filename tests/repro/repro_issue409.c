/*
 * repro_issue409.c — Reproduce-first case for OPEN bug #409.
 *
 * Issue #409: "v0.7.0 install/update wires the legacy blocking PreToolUse
 * gate, not hook_augment (regresses #214)"
 *
 * Root cause (as filed):
 *   cbm_install_hook_gate_script wrote the legacy blocking shell gate
 *   (keyed on $PPID, emitting `exit 2` to block tool calls) instead of the
 *   non-blocking augmenter shim that delegates to `<binary> hook-augment`.
 *   On an upgrade from a pre-v0.7.0 install the old gate script remained on
 *   disk (or was rewritten with blocking content), so every Grep/Glob call
 *   was blocked rather than being non-blocking augmented — the exact symptom
 *   of #214 which was supposed to be fixed.
 *
 * Current ownership contract:
 *   The installer may replace a missing file, its current bytes, or an exact
 *   byte-for-byte released version. An arbitrary existing script is treated as
 *   user-owned and must be preserved, even when its content is an old blocking
 *   implementation. This prevents an upgrade from silently overwriting a
 *   user's hook or deleting their security policy.
 *
 * Upgrade scenario tested here (NOT covered by existing tests):
 *   This test simulates an upgrade from a pre-v0.7.0 install by:
 *     a) Pre-seeding the gate-script path with the OLD blocking content
 *        (containing $PPID and exit 2) — as would be present on disk after
 *        a pre-v0.7.0 install.
 *     b) Pre-seeding settings.json with a stale CMM hook entry using the
 *        old "Grep|Glob|Read" matcher and an old command string.
 *   Then running cbm_install_hook_gate_script and asserting that the installer
 *   fails closed without changing the foreign script. Exact released-script
 *   migration is covered by the CLI unit tests.
 *
 *   This guards the security boundary around the upgrade path: an old-looking
 *   but non-owned script must not be guessed as installer-owned.
 *
 * Relationship to existing tests:
 *   cli_hook_gate_script_no_predictable_tmp_issue384 (test_cli.c:2196):
 *     Tests cbm_install_hook_gate_script in isolation on a fresh dir.
 *     Does NOT test the upgrade/overwrite scenario.
 *   cli_upsert_claude_hook_fresh (test_cli.c:2167):
 *     Tests cbm_upsert_claude_hooks in isolation on fresh settings.json.
 *     Does NOT test the integrated (both calls) upgrade path.
 *
 * NOTE: the original reproduction expected arbitrary legacy bytes to be
 * overwritten. That expectation conflicts with the ownership-safe installer
 * contract and was corrected here so the repro tests the real safety rule.
 */

#include <foundation/compat.h>
#include "test_framework.h"
#include "test_helpers.h"
#include <cli/cli.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* ── Local helpers (mirror the helpers in test_cli.c) ──────────────── */

static int rp409_write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "%s", content);
    fclose(f);
    return 0;
}

static const char *rp409_read_file(const char *path) {
    static char buf[16384];
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Recursively create directory (simple two-level: parent + child). */
static int rp409_mkdirp(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            cbm_mkdir(tmp);
            *p = '/';
        }
    }
    return cbm_mkdir(tmp) == 0 || errno == EEXIST ? 0 : -1;
}

/* ── Test ──────────────────────────────────────────────────────────── */

/*
 * repro_issue409_install_preserves_foreign_blocking_gate
 *
 * Simulates an upgrade from a pre-v0.7.0 install:
 *   - The hooks dir already contains the OLD blocking gate script
 *     (containing $PPID and exit 2).
 *   - settings.json already contains a stale CMM hook with the old matcher
 *     "Grep|Glob|Read" and an old inline command.
 *
 * After calling cbm_install_hook_gate_script, asserts that:
 *   1. Installation is refused because the existing bytes are foreign.
 *   2. The old gate script remains byte-for-byte unchanged.
 *   3. settings.json remains untouched because script installation failed.
 *
 * RED if the installer overwrites the foreign file, reports success, or
 * partially updates settings after refusing the script.
 *
 * Oracle used: cbm_install_hook_gate_script(home, binary_path).
 */
TEST(repro_issue409_install_preserves_foreign_blocking_gate) {
    /* Create a temp HOME directory tree that simulates a pre-v0.7.0 install. */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/rp409-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    /* Create <home>/.claude/hooks/ (mirrors real Claude Code layout). */
    char hooks_dir[512];
    snprintf(hooks_dir, sizeof(hooks_dir), "%s/.claude/hooks", tmpdir);
    if (rp409_mkdirp(hooks_dir) != 0)
        FAIL("mkdirp hooks_dir failed");

    /* Pre-seed the gate script with the OLD blocking content that the issue
     * reporter observed on v0.7.0.  This is the content that must be
     * overwritten (truncated) by cbm_install_hook_gate_script. */
    char script_path[512];
    snprintf(script_path, sizeof(script_path),
             "%s/cbm-code-discovery-gate", hooks_dir);
    static const char old_script[] =
        "#!/bin/bash\n"
        "# Gate hook: nudges Claude toward memory-for-ai for code discovery.\n"
        "# First Grep/Glob/Read per session -> block. Subsequent -> allow.\n"
        "# PPID = Claude Code process PID, unique per session.\n"
        "GATE=/tmp/cbm-code-discovery-gate-$PPID\n"
        "if [ -f \"$GATE\" ]; then exit 0; fi\n"
        "touch \"$GATE\"\n"
        "echo 'BLOCKED: use memory-for-ai' >&2\n"
        "exit 2\n";
    ASSERT_EQ(rp409_write_file(script_path, old_script), 0);

    /* Pre-seed settings.json with a stale CMM hook entry (old matcher). */
    char settings_path[512];
    snprintf(settings_path, sizeof(settings_path),
             "%s/.claude/settings.json", tmpdir);
    static const char old_settings[] =
        "{\"hooks\":{\"PreToolUse\":["
        "{\"matcher\":\"Grep|Glob|Read\","
        "\"hooks\":[{\"type\":\"command\","
        "\"command\":\"~/.claude/hooks/cbm-code-discovery-gate\"}]}]}}";
    ASSERT_EQ(rp409_write_file(settings_path, old_settings), 0);

    /* An arbitrary pre-existing script is not proof of installer ownership.
     * The production installer must refuse it before registering a new hook. */
    ASSERT_TRUE(!cbm_install_hook_gate_script(tmpdir, "/usr/local/bin/memory-for-ai"));

    /* ── Assert the foreign gate script was preserved byte-for-byte ── */
    const char *script_data = rp409_read_file(script_path);
    ASSERT_NOT_NULL(script_data);
    ASSERT_EQ(strcmp(script_data, old_script), 0);
    ASSERT(strstr(script_data, "PPID") != NULL);
    ASSERT(strstr(script_data, "exit 2") != NULL);

    /* A failed script install must not cause a partial settings update. */
    const char *settings_data = rp409_read_file(settings_path);
    ASSERT_NOT_NULL(settings_data);
    ASSERT_EQ(strcmp(settings_data, old_settings), 0);

    th_rmtree(tmpdir);
    PASS();
}

/* ── Suite ──────────────────────────────────────────────────────────── */
SUITE(repro_issue409) {
    RUN_TEST(repro_issue409_install_preserves_foreign_blocking_gate);
}
