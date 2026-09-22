# Development standard

This is the human-readable companion to the repository rules in [AGENTS.md](../AGENTS.md). `AGENTS.md` is normative for AI coding agents; this document explains how a person or an agent should use the project safely and efficiently.

## Before changing code

1. Read the relevant requirement, public contract, module documentation, tests, and current diff.
2. Map the affected entry point, state, data ownership, side effects, failure paths, and callers.
3. Write a small design note when the change has more than one state or side effect:

```text
Objects:       affected objects
States:        states that change behaviour
Inputs:        required inputs
Events:        state-changing events
Transitions:   RULE-001, RULE-002, ...
Invariants:    facts that must always hold
Side effects:  files, database, network, process, thread, UI
Failure mode:  reject, rollback, partial, bounded retry, or stop
Tests:         state, transition, error, and invariant coverage
```

Prefer the smallest local change that preserves the existing public contract. Do not add state, retries, caches, abstractions, or dependencies without a current requirement and a testable benefit.

## Code quality and warnings

The native build is warning-as-error. Fix warnings at their cause: bounds, format strings, ownership, overflow, nullability, cleanup, or API contracts. Do not weaken `-Werror`, add a blanket cast, or hide a new warning with a pragma or `-Wno-*` flag. Existing GCC-only compatibility suppressions in `Makefile.cbm` are narrowly scoped and documented; they are not permission to ignore a logic defect.

For C/C++ changes, review NULL handling, integer overflow, buffer capacity, allocation ownership, double-free/use-after-free, races, deadlocks, rollback, and cleanup. For MCP or mutation tools, preserve schema compatibility, dry-run defaults, drift/mtime checks, backups, partial-state reporting, and re-index/verification after writes.

## Verification matrix

Run the smallest relevant checks first, then the full checks when the environment supports them:

```powershell
python scripts/check-product-metadata.py
scripts/test.sh --suites <related-suite>
scripts/lint.sh

cd graph-ui
npm.cmd test -- --run
npm.cmd run build
cd ..
```

For the bundled native Windows MinGW toolchain, run the unsanitized full
runner explicitly because its GCC installation does not ship the ASan/UBSan
runtime archives:

```powershell
$make = (Resolve-Path '.toolchain/mingw64/bin/mingw32-make.exe').Path
$gcc = (Resolve-Path '.toolchain/mingw64/bin/gcc.exe').Path
$gxx = (Resolve-Path '.toolchain/mingw64/bin/g++.exe').Path
& $make -f Makefile.cbm build/c/test-runner CC=$gcc CXX=$gxx SANITIZE= -j1
& .\build\c\test-runner.exe
```

Run `scripts/test.sh` in Linux/WSL or CI for ASan/UBSan/TSan coverage. Do not
claim sanitizer coverage from the Windows native runner; report the native
result and the unavailable sanitizer lane separately.

At handoff, report the exact commands, pass/fail result, and any environment limitation. A compile-only result is not completion for a behaviour change.

## Documentation synchronization

Update documentation in the same change when behaviour, schemas, tool count, CLI flags, build/test commands, module boundaries, or limitations change:

- `README.md`: user-facing capabilities and navigation.
- `CONTRIBUTING.md`: contributor workflow and required checks.
- `docs/AGENT_GUIDE.md`: exact tool semantics and AI playbooks.
- `docs/llms.txt`: concise machine-readable project facts.
- `AGENTS.md`: rules that every future AI coding task must follow.

Keep counts and commands consistent with generated product metadata and the actual build. Describe coverage, stale-index risk, dry-run/mutation boundaries, rollback, and evidence requirements explicitly. Never claim completeness based only on a partial index or a successful compile.

## Review checklist

- [ ] Requirement, callers, invariants, and side effects were inspected.
- [ ] The implementation is minimal and has a clear owner for mutable state.
- [ ] Error paths preserve data consistency and clean up resources.
- [ ] Regression tests cover the failure or boundary that motivated the change.
- [ ] Build/lint/test warnings are resolved or explicitly explained.
- [ ] Public docs and machine-readable metadata are synchronized.
- [ ] The final report names changed files and exact verification results.
