# Project graph extensions

`memory-for-ai` now indexes project-level relationships in addition to source
definitions and calls. These relationships are evidence extracted from files;
the indexer does not execute build tools, test runners, package managers, or
network vulnerability scanners.

## Build and test graph

Build manifests produce `BuildTarget` nodes and `BUILDS` edges from the
manifest `File` node. Supported target evidence includes Make, CMake/Meson,
npm scripts, Docker `FROM` stages, and a safe synthetic default target for
other supported project manifests.

Test files produce `TestSuite` nodes and `CONTAINS_TEST` edges. Existing
`TESTS`/`TESTS_FILE` edges remain the symbol/file-level test graph. Build
targets whose names look like test, check, verify, or lint targets receive
`RUNS_TESTS` edges to the discovered suites.

Example:

```cypher
MATCH (b:BuildTarget)-[:RUNS_TESTS]->(s:TestSuite)
RETURN b.name, s.file_path
```

## Dependency graph

Dependency manifests produce external `Package` nodes and `DEPENDS_ON` edges.
The edge properties include `ecosystem`, `scope`, `version`, `direct`, and
`source`. The graph covers the existing Go/Python/Kubernetes/Helm paths and
adds npm, Composer, Cargo, Maven, Gradle, RubyGems, Dart pub, Elixir Hex,
SwiftPM, and Python requirements variants. Parsers are bounded and literal:
they never execute a manifest or invoke a package manager.

```cypher
MATCH (f:File)-[d:DEPENDS_ON]->(p:Package)
RETURN f.file_path, p.name, d.scope, d.version
```

## Security evidence graph

Each dependency manifest gets a `SecurityAudit` node with `status: "not_run"`.
This is an explicit state: a package has been discovered, but no scanner has
been invoked by the indexer. Local report files named `npm-audit.json`,
`osv-scanner.json`, `trivy.json`, `dependency-check-report.json`, or
`audit.json` are also represented as `SecurityReport` nodes. Recognized CVE,
GHSA, OSV, and RUSTSEC identifiers become `SecurityAdvisory` nodes joined by
`REPORTS` edges. Their properties retain `verified: false` until a user or an
external scanner verifies the finding.

`SecurityAudit` also has `AUDITS` edges to the packages declared by its
manifest. This keeps “declared dependency” and “security evidence” separate:
`status: "not_run"` means the project has not supplied a scanner result, not
that the packages are safe.

Use `get_code_actions` on a dependency or source range to receive read-only
follow-up suggestions for coverage, complexity, missing test edges, and local
security evidence. No action mutates source automatically.
