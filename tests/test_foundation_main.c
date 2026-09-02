/*
 * test_foundation_main.c — dedicated main for `make test-foundation`.
 *
 * tests/test_main.c references every suite in the tree (store, cypher, mcp,
 * daemon, ...) through plain non-weak externs, so linking it against only the
 * foundation sources — which is exactly what TEST_FOUNDATION_SRCS does —
 * leaves every non-foundation suite_* symbol undefined and the target fails
 * at link time. This main keeps the advertised "Foundation tests only (fast)"
 * lane buildable: same counters, same TEST_SUMMARY() exit contract, with the
 * foundation suites in test_main.c's declaration order.
 */
#include "test_framework.h"

/* The framework's counters are defined by whichever main links the suites. */
int tf_pass_count = 0;
int tf_fail_count = 0;
int tf_skip_count = 0;

extern void suite_arena(void);
extern void suite_hash_table(void);
extern void suite_dyn_array(void);
extern void suite_str_intern(void);
extern void suite_log(void);
extern void suite_str_util(void);
extern void suite_workspace(void);
extern void suite_platform(void);
extern void suite_diagnostics(void);
extern void suite_complexity(void);
extern void suite_dump_verify(void);
extern void suite_subprocess(void);
extern void suite_private_file_lock(void);
extern void suite_lock_registry(void);

int main(void) {
    printf("\n  memory-for-ai  C foundation test suite\n");

    RUN_SUITE(arena);
    RUN_SUITE(hash_table);
    RUN_SUITE(dyn_array);
    RUN_SUITE(str_intern);
    RUN_SUITE(log);
    RUN_SUITE(str_util);
    RUN_SUITE(workspace);
    RUN_SUITE(platform);
    RUN_SUITE(diagnostics);
    RUN_SUITE(complexity);
    RUN_SUITE(dump_verify);
    RUN_SUITE(subprocess);
    RUN_SUITE(private_file_lock);
    RUN_SUITE(lock_registry);

    TEST_SUMMARY();
}
