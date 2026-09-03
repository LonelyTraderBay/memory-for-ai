# Agent operating guide — memory-for-ai

**Audience.** You are an AI coding agent (or the person configuring one) and the memory-for-ai MCP server is available in your session. This document is the complete operating manual: what the graph is, what each of the 18 tools does exactly, which tool to pick for which task, how to avoid wrong conclusions, and how to fit the index to your specific project.

**How to read.** Sections 1–2 get you productive in five minutes. Section 3 (tool catalog) and Section 4 (task playbook) are the reference you will return to. Section 7 (correctness protocol) is mandatory before you make claims like "X is never called" or "this list is complete".

---

## 1. Mental model

- **One index per repository**, persisted in SQLite under `~/.cache/memory-for-ai/` (override: `MFA_CACHE_DIR`). The index **survives across sessions** — this is the product's long-term memory. Re-indexing is incremental and usually automatic (background watcher).
- **The graph**: nodes (`Function`, `Method`, `Class`, `Interface`, `Route`, `Package`, `File`, `Folder`, `Module`, `TestSuite`, `BuildTarget`, `Resource`, …) and edges (`CALLS`, `CALL_REFERENCE`, `USAGE`, `IMPORTS`, `IMPLEMENTS`, `INHERITS`, `DATA_FLOWS`, `HTTP_CALLS`, `ASYNC_CALLS`, `EMITS`, `LISTENS_ON`, `SIMILAR_TO`, `SEMANTICALLY_RELATED`, `CROSS_*`, `BUILDS`, `RUNS_TESTS`, `DEPENDS_ON`, `TESTS`, `AUDITS`, `REPORTS`, …).
- **Project name**: derived from the repository path (see `list_projects`). A bare folder name is accepted as an alias when it uniquely matches. Most tools take `project` as a required argument.
- **Qualified name (`qn`)**: `<project>.<path_parts>.<symbol>`. `get_code_snippet` wants the full qn — always discover it with `search_graph` first.
- **Structural vs textual**: `CALLS` edges come from type-aware resolution (Hybrid LSP). A grep hit tells you a name appears; a `CALLS` edge tells you a specific callable invokes another. Transitive chains (callers of callers) exist **only** in the graph — no amount of grep recovers them reliably.
- **No LLM inside**: you are the intelligence layer. The server executes queries and returns structured rows.

## 2. First-session checklist

```
1. list_projects                          → is this repo already indexed?
2. index_repository(repo_path=...)        → if not (or stale). Wait for completion.
3. get_graph_schema(project=...)          → node labels, edge types, property keys
4. get_architecture(project=...)          → languages, packages, entry points, hotspots
5. store the project name; you will pass it to every call
```

After indexing, note the response's coverage fields (`skipped`, `parse_partial`, `excluded`) — see Section 7 before trusting completeness.

## 3. Tool catalog (18 tools)

Annotations: 🌱 mutates state · everything else is read-only against the graph.

### Indexing & projects

| Tool | What it does · key parameters |
|---|---|
| `index_repository` 🌱 | Index a repo. `repo_path` (absolute) required. `mode`: `full` (all files + similarity/semantic edges), `moderate` (filtered files + similarity/semantic), `fast` (filtered, no similarity/semantic) — **all modes run type-aware LSP resolution**; `cross-repo-intelligence` (extraction skipped; matches Routes/Channels across `target_projects` to build `CROSS_*` edges; index targets first). `name` overrides the derived project name. `persistence=true` writes the team-shareable artifact `.memory-for-ai/graph.db.zst`. Response includes the coverage report (Section 7) and the per-run `logfile` path. |
| `list_projects` | Indexed projects with deterministic pagination (`offset`/`limit`). `include_details=true` adds branch, node/edge counts, DB size, label/type counts. |
| `index_status` | Node/edge counts, root path, git context, and the coverage report for one project. `verbose=true` adds git worktree detail (debugging only). |
| `delete_project` 🌱 | Remove a project and all its graph data. |
| `check_index_coverage` | **Coverage oracle** for exact `paths` (≤128, repo-relative) or `scopes` (≤32 prefixes; `.` = whole repo). Returns per-path status, parse-error ranges, quantitative `coverage_summary`, and `confidence_reasons`. Use it after discovery for every file you cite or modify; use scopes before negative/exhaustive claims. |

### Discovery & reading

| Tool | What it does · key parameters |
|---|---|
| `search_graph` | **Primary discovery tool — use instead of grep/glob for finding definitions.** Three independent modes, combinable: (1) `query="update settings"` — BM25 full-text, camelCase split (`updateCloudClient` → update, cloud, client), structural boosting (Functions/Methods +10, Routes +8, Classes/Interfaces +5; noise labels filtered); (2) `name_pattern=".*regex.*"` exact pattern (ignored when `query` present); (3) `semantic_query=["send","publish"]` — vector cosine search bridging vocabulary gaps (**array of strings, not one string**; requires moderate/full index; results in `semantic_results`). Filters: `label`, `qn_pattern`, `file_pattern`, `relationship`, `min_degree`/`max_degree`, `exclude_entry_points`, `include_connected`. Pagination: `limit` (default 50) + `offset`, driven by response `total` and `has_more`. `fields=["complexity","signature","docstring",…]` adds property columns; `detail="ids"` = cheapest bare-qn enumeration; `format="json"` = same model as JSON. |
| `get_code_snippet` | Read a symbol's source. Pass the **full qualified name** from `search_graph` (short name returns suggestions if ambiguous). `include_neighbors=true` for surrounding defs. If the response carries `coverage_note`, the file is partially indexed — treat returned source as ground truth and grep the flagged ranges for anything the graph claims about that file. |
| `search_code` | Graph-augmented grep when you truly need text: matches pattern, dedupes hits into containing functions, ranks by structural importance (definitions first, tests last). `mode`: `compact` (signatures — default, token-efficient), `full` (60-line window around first match), `files`. `file_pattern` (glob for grep include), `path_filter` (regex on paths), `regex=true`. No offset — compare `total_results`/`total_grep_matches` to `limit` to detect truncation, then narrow or raise `limit`. |
| `get_graph_schema` | Node/edge counts, relationship patterns, per-label property definitions. Run once per project before writing Cypher. |
| `get_architecture` | One-call overview: default (no `aspects`) = compact summary (counts, languages, packages, entry points). `aspects=["structure","dependencies","routes","hotspots","boundaries","layers","clusters","file_tree","cycles","all"]`. `clusters` = community detection over the call/import graph — the *de-facto* modules, which often cut across folder layout. `cycles` (SCCs) is opt-in only, never via `all`. `path` scopes analysis to a directory prefix. |
| `get_code_actions` | Deterministic, read-only suggestions for a file range (`path`, `start_line`, `end_line`) from coverage, complexity, test-relationship, and dependency-security evidence. Never edits source. |

### Relationship & impact analysis

| Tool | What it does · key parameters |
|---|---|
| `trace_path` | **Use instead of grep for callers/dependencies/impact/data flow.** `function_name` + `project` required. `direction`: `inbound` (callers), `outbound` (callees), `both` (default). `depth` default 3 (1–5 useful). `mode`: `calls` (default), `data_flow` (value propagation with arg expressions at each hop; `parameter_name` scopes it), `cross_service` (through Route nodes + `CROSS_*` edges to other services). Response: tree rows grouped by qn-prefix; **`callers_total`/`callees_total` are exact transitive counts** on every page. `risk_labels=true` adds CRITICAL/HIGH/MEDIUM/LOW by hop distance. Test files are excluded unless `include_tests=true`. `include_evidence=true` adds per-hop resolution strategy (`lsp` / `language_rule` / `heuristic` / `unresolved`) + confidence — use it to judge whether to trust an edge. Paginate with the `next` cursor passed back as `cursor` (all other args identical). |
| `detect_changes` | **Blast radius of a git diff.** `base_branch` (default `main`) or `since` (any ref/tag, e.g. `HEAD~5`, `v0.5.0` → diffs `<ref>...HEAD`). Resolves changed files to the symbols they define, then one traversal to the transitive impact set. `scope`: `impact` (default) or `files` (no traversal). `direction`: `inbound` (default — what your change breaks, i.e. transitive callers), `outbound` (what the change depends on), `both`. `impacted_total` is always exact; `impacted_modules` is a complete rollup. Seeds (the changed symbols themselves) are excluded from `impacted`. |
| `query_graph` | Read-only Cypher subset (Section 6) for multi-hop patterns and aggregations grep cannot express. 100k-row hard ceiling — add `LIMIT` or use `search_graph` for browsing. Every Function/Method carries queryable quality properties: `complexity` (cyclomatic), `cognitive`, `loop_count`, `loop_depth`, `transitive_loop_depth`, `linear_scan_in_loop`, `alloc_in_loop`, `recursion_in_loop`, `unguarded_recursion`, `recursive`, `param_count`, `max_access_depth`. `graph="missed"` queries the miss graph instead (Section 7). |
| `compare_graphs` | Deterministic diff of two indexed project snapshots: target-only additions and base-only removals for stable node/edge identities. Bounded by `limit` and a 512 KiB encoded budget; exact totals and truncation reasons always reported. Index both states as separate projects first (e.g. worktree at `main` vs at `feature`). |

### Memory & runtime evidence

| Tool | What it does · key parameters |
|---|---|
| `manage_adr` | Persistent Architecture Decision Records per project — cross-session memory. `mode`: `get`, `update` (**replaces the whole document**), `set_sections` (rewrites only the named sections, leaving every other byte untouched; writing the same body twice is a no-op → safe retries), `sections` (list headings). Any `## Heading` works; names match exactly **including case**. Conventional sections: PURPOSE, STACK, ARCHITECTURE, PATTERNS, TRADEOFFS, PHILOSOPHY. |
| `ingest_traces` 🌱 | Persist runtime call observations into an isolated sidecar (never merged into the static graph). Default wire `compact-v1`: `traces[]` of `{caller, callee, count?, duration_ns?, error?}` (caller/callee required), ≤10,000 items / 16 MiB; same `(project, source_batch_id)` + same payload = idempotent success, different payload = no-mutation conflict. Opt-in `canonical-v2` adds producer-scoped dedup (`producer_id`, `producer_epoch`, `source_batch_id`; lowercase-hex `trace_id`/`span_id`). |
| `get_runtime_traces` | Read runtime aggregates, deterministically ordered by call count, caller, callee. `include_overlay=true` reads a pinned `RUNTIME_CALL` publication (with `runtime_generation` to pin, `cursor` from `next_cursor` to page). Runtime data never leaks into `search_graph` or `trace_path` results. |

## 4. Task → tool playbook

| You need to… | Do this | Notes |
|---|---|---|
| Orient in an unfamiliar repo | `get_architecture` → then `aspects=["clusters","routes"]` if relevant | Cheapest full picture (~hundreds of tokens). Re-run any session to refresh; it is cheap. |
| Find a symbol by description ("the thing that refreshes tokens") | `search_graph(query="refresh token")` | BM25 + structural boosting. Add `label="Function"` to focus. |
| Find a symbol when vocabulary may differ ("send" vs "publish") | `search_graph(semantic_query=["send","notification"])` | Needs moderate/full index. Check `semantic_results`, not `results`. |
| Find a symbol by exact/partial name | `search_graph(name_pattern="(?i).*order.*")` | Regex on names. `detail="ids"` for wide sweeps. |
| Read a function's source | `search_graph` first → `get_code_snippet(qualified_name=<qn>)` | Honor `coverage_note` if present. |
| Who calls X? / what does X call? | `trace_path(function_name="X", direction="inbound"/"outbound", depth=3)` | Totals are transitive and exact. Add `include_evidence=true` to audit edge trust. |
| What breaks if I change these files? | `detect_changes(since="HEAD~3", direction="inbound")` (committed work) or `trace_path` inbound per symbol (uncommitted idea) | `impacted_modules` rollup is complete even when rows truncate. |
| Find dead code | `query_graph("MATCH (f:Function) WHERE NOT EXISTS { (f)<-[:CALLS]-() } RETURN f.qualified_name")` — then filter entry points (`search_graph(exclude_entry_points=true, max_degree=0)`) | Always check coverage scope first (Section 7): a "dead" symbol may live in a skipped file's caller. |
| Complexity / perf hotspots | `query_graph("MATCH (f:Function) WHERE f.transitive_loop_depth >= 3 OR f.linear_scan_in_loop >= 1 RETURN f.qualified_name, f.complexity ORDER BY f.complexity DESC")` | These properties are per-node; one query sweeps the repo. |
| Service-to-service impact | `index_repository(mode="cross-repo-intelligence", target_projects=["*"])` → `trace_path(mode="cross_service")` | Index each service first; run the intelligence pass after any of them re-indexes. |
| Text search that must not miss string literals/comments | `search_code(pattern="...", mode="compact")` | This is the honest fallback when you need raw text, not structure. |
| Diff two snapshots (refactor audit, vendor bump) | Index both worktrees as projects → `compare_graphs(base_project, target_project)` | Stable identities → deterministic additions/removals. |
| Remember a decision for future sessions | `manage_adr(mode="set_sections", section_updates={"TRADEOFFS": "…"})` | Byte-preserving; safe to retry. |
| Recall what the runtime actually did | `get_runtime_traces(project=…)` after your instrumentation `ingest_traces` | Static graph = what code says; sidecar = what ran. |
| Ship the index with the repo | `index_repository(persistence=true)` → commit `.memory-for-ai/graph.db.zst` | Section 9. |

## 5. Response formats, pagination, cursors

- Most list-shaped tools return **prefix-grouped tree rows** (shared qn-prefix printed once per group) by default; `format="json"` returns the identical model as structured JSON for programmatic use.
- **Truncation signals differ per tool** — read them before concluding a list is complete:
  - `search_graph`: `total` + `has_more` → page with `offset += limit`.
  - `trace_path`: `truncated: true` + `next` cursor → pass back as `cursor` with identical args. Cursors **do not survive a re-index** (explicit `stale_cursor` error — just re-run).
  - `search_code`: no offset — compare `total_results` to `limit`, then narrow (`path_filter`/`file_pattern`) or raise `limit`.
  - `query_graph`: 100k-row ceiling — put `LIMIT` in the query.
  - `compare_graphs`: exact totals + truncation reason always present.
- `search_graph`'s `in`/`out` columns are **degree summaries over selected edge types, not caller counts** — use `trace_path` for callers.
- Test files are excluded from `trace_path` by default (`include_tests=false`); `search_graph` reports an `is_test` field you can filter on.

## 6. Cypher subset (`query_graph`)

Read-only openCypher subset; anything outside it fails with a clear `unsupported …` error rather than empty results.

- **Clauses**: `MATCH`, `OPTIONAL MATCH`, multiple `MATCH`, `WHERE`, `WITH` (+ `WITH … WHERE`), `RETURN`, `ORDER BY`, `SKIP`, `LIMIT`, `DISTINCT`, `UNWIND`, `UNION` / `UNION ALL`, `CASE`.
- **Patterns**: labelled nodes, label alternation `(n:A|B)`, relationship types/direction, variable-length paths `[*1..3]`, inline property maps.
- **WHERE**: comparisons, `AND/OR/XOR/NOT`, `IN`, `CONTAINS`, `STARTS WITH`, `ENDS WITH`, `IS [NOT] NULL`, regex `=~`, label test `n:Label`, and `EXISTS { (n)-[:TYPE]->() }` (single-hop existence — the dead-code workhorse).
- **Aggregates**: `count` (+`DISTINCT`), `sum`, `avg`, `min`, `max`, `collect`.
- **Functions**: `labels`, `type`, `id`, `keys`, `properties`; `toLower/toUpper/toString/toInteger/toFloat/toBoolean`; `size`, `length`, `trim/ltrim/rtrim`, `reverse`; `coalesce`, `substring`, `replace`, `left`, `right`.

Not supported (fails explicitly): write/`MERGE`/`CALL` clauses, unsupported functions, list/map literals, comprehensions, path functions, parameters.

## 7. Correctness protocol (read before claiming anything)

The graph is evidence, not truth. The indexer reports what it could not fully index — use those signals.

1. **Coverage after discovery.** For every file you cite, modify, or reason about: `check_index_coverage(paths=[...])`. Statuses include `indexed_no_recorded_gap` — which is *not* a completeness guarantee, only "no recorded gap".
2. **`parse_partial` files** were indexed, but constructs in the flagged line ranges may be missing from the graph. For those ranges: read the source directly (`get_code_snippet` returns ground-truth source; or grep) instead of trusting graph absence.
3. **`skipped` files** were not indexed at all (oversized / read / parse failure) — **they cannot appear in any graph result**, so their callers don't exist as far as the graph knows. Before any negative or exhaustive claim ("nothing calls X", "this list is complete"), run `check_index_coverage(scopes=["."])` (or scope to the relevant subtree) and account for skipped + excluded files first.
4. **`excluded` / `not_indexed`** entries are deliberate (`.gitignore`, `.cbmignore`, skip-lists) — deterministic by design, not failures. Change ignore rules and re-index to include them.
5. **Structural misses**: `query_graph(graph="missed", query="MATCH (f:File) WHERE f.kind=\"parse_partial\" RETURN f.file_path, f.detail")` lists every flagged file with its ranges. Absence from this graph is still not a guarantee.
6. **Coverage summary semantics** (from `index_status` / `check_index_coverage`): `coverage_ratio` = `indexed_file_hashes / (indexed_file_hashes + skipped_files + excluded_files)` — a file-hash accounting ratio, **not** parser accuracy; `null` when the denominator can't be proven. `confidence_score`/`confidence_level`/`confidence_reasons` grade the trustworthiness of the summary itself (metadata availability, generation match, hash completeness). High confidence means the accounting is internally consistent — it does not replace reading source for security-critical conclusions.
7. **Freshness.** The watcher keeps indexes fresh after Git/filesystem changes, but verify when it matters: `index_status` reports git context and coverage; if the repo moved ahead (e.g. big rebase), re-run `index_repository` — incremental cost is proportional to the change. Stale trace cursors error loudly (`stale_cursor`) rather than returning wrong pages.
8. **Trust per edge.** `trace_path(include_evidence=true)` labels each hop `lsp` (type-aware, strongest), `language_rule`, `heuristic`, or `unresolved`. For conclusions that will drive destructive action, prefer `lsp`-resolved edges and corroborate with source.
9. **Entry points and tests.** Dead-code style queries must exclude entry points (`exclude_entry_points=true`, or negative degree + manual review); remember test files are invisible to `trace_path` by default — a "dead" private helper may be called from a test.

## 8. Tuning the index to your project

### Choose the index mode

`index_repository(mode=...)`:

| Mode | Files | Similarity/semantic edges | Use when |
|---|---|---|---|
| `full` (default) | all | yes | default choice; also required for best team artifact tier |
| `moderate` | filtered (skip-lists prune docs/examples/testdata…) | yes | big monorepos where generated/vendored trees are noise |
| `fast` | filtered | no | quick orientation; also the mode the watcher uses for cheap refreshes |

`semantic_query` search requires moderate/full. All modes run full type-aware LSP call resolution — `fast` is not "dumber" about call edges, it just indexes fewer files and skips the similarity/semantic pass.

### `.cbmignore` — shape what the indexer sees

Repo-root file, gitignore syntax, applied at discovery time (initial index, manual re-index, watcher). Nested copies are not read. Commit it to share excludes with the team.

```gitignore
# generated protobuf output, anywhere
*.pb.go
# a specific top-level directory
/third_party/
# any directory named "snapshots", at any depth
snapshots/
# everything under any fixtures directory
**/fixtures/**
# ignore all YAML, keep CI configs (last matching pattern wins)
*.yaml
!ci.yaml
```

Precedence (first rejecting layer wins, with one exception): (1) built-in skip list (`.git`, `node_modules`, `dist`, `target`, `vendor`, tool caches — 60+ names; fast/moderate add `docs`, `examples`, `testdata`); (2) repo `.gitignore` + `info/exclude`; (3) nested `.gitignore`s; (4) `.cbmignore` — whose **negations** (`!pattern`) can un-skip ordinary built-in dirs (not the safety core: `.git`, `node_modules`, `.worktrees`, `.claude-worktrees`) and rescue paths from (5) git global excludes; (5) `core.excludesFile`. Built-in suffix filters (binaries, archives, media, lockfiles, `.min.js` in fast modes) and the file-size cap (512 MiB default, `CBM_MAX_FILE_BYTES`) are not overridable. Symlinks are always skipped. Parent-pruning caveat: a negation cannot resurrect files inside an excluded directory — negate the directory itself.

Verify: skipped subtrees appear under `excluded` in the `index_repository` response (`{"dirs": [≤25 paths], "count": total, "truncated": bool}`).

### Custom file extensions

Map framework-specific extensions onto built-in grammars:

```json
// <repo>/.memory-for-ai.json  (per-project — wins over global)
{ "extra_extensions": { ".blade.php": "php", ".mjs": "javascript" } }
```

```json
// ~/.config/memory-for-ai/config.json  (global; honors $XDG_CONFIG_HOME)
{ "extra_extensions": { ".twig": "html", ".phtml": "php" } }
```

Keys must start with `.`; language names are case-insensitive; unknown names are skipped with a stderr warning. Accepted names include bash, c, c++, c#, clojure, cmake, cobol, common lisp, css, cuda, dart, dockerfile, elixir, elm, erlang, f#, fortran, glsl, go, graphql, groovy, haskell, hcl, html, ini, java, javascript, json, julia, kotlin, lua, makefile, markdown, matlab, nix, objective-c, ocaml, perl, php, protobuf, python, r, ruby, rust, scala, scss, sql, svelte, swift, toml, tsx, typescript, verilog, vimscript, vue, xml, yaml, zig (plus aliases). Full table: [CONFIGURATION.md](CONFIGURATION.md#1-custom-file-extension-mapping).

### Resolver knobs (rare)

- `CBM_DISABLE_LSP_CROSS=1` — skip the LSP cross-pass (the most expensive pipeline phase); documented workaround if a language resolver misbehaves on your code.
- `CBM_LSP_DISABLED=1` — drop Hybrid-LSP entirely (diagnostic: isolates tree-sitter-only behavior).
- `CBM_SEMANTIC_ENABLED=1` — opt in to the semantic **edge** pass (`SEMANTICALLY_RELATED`, threshold `CBM_SEMANTIC_THRESHOLD`, default 0.75) at index time. (Distinct from `semantic_query` search, which needs moderate/full mode.)

## 9. Project isolation & team sharing

### Scoped sessions (`--scope`)

`memory-for-ai --scope=/path/to/repo` pins an MCP session to exactly one repository: any `project`/`base_project`/`target_project` argument naming a different project is **refused** before the tool runs (folder-name aliases still resolve), `list_projects` shows only the pinned project, and indexing is confined inside the scope path. This is the mechanism behind per-project installs (below) — see also [CONFIGURATION.md §3b](CONFIGURATION.md#3b-per-project-scoped-sessions---scope-install---project).

### Per-project install (`install --project`)

Run from the repo root (the shell/PowerShell installers forward the flag):

```bash
curl -fsSL https://raw.githubusercontent.com/LonelyTraderBay/memory-for-ai/main/install.sh | bash -s -- --project
```

Installs/refreshes the shared binary **without touching global agent configs**, writes a repo-local `.mcp.json` entry named `memory-for-ai-<repo-directory>` (`--name=` to override) pinned with `--scope`, and indexes immediately. Agents in other repos see their own servers; `uninstall` never touches the repo's own `.mcp.json`. Full walkthrough: [INSTALL.md](INSTALL.md#per-project-install).

### Team-shared graph artifact

`index_repository(persistence=true)` writes `.memory-for-ai/graph.db.zst` — a zstd-compressed, `VACUUM INTO`-compacted snapshot (8–13:1 typical) next to your source. Commit it and teammates bootstrap from it on first index (import + incremental diff) instead of a full re-index. A `.gitattributes` line with `merge=ours` is auto-created on first export, so concurrent updates never conflict the binary file. Two tiers: **best** (explicit index, `zstd -9`) and **fast** (watcher refreshes, `zstd -3`). Purely optional — add `.memory-for-ai/` to `.gitignore` to make everyone index from scratch.

### Cross-repo intelligence

Index each repository as its own project, then `index_repository(repo_path=<any>, mode="cross-repo-intelligence", target_projects=["*"])` matches Routes/Channels across projects into `CROSS_HTTP_CALLS` / `CROSS_ASYNC_CALLS` / `CROSS_CHANNEL` edges. Re-run after any target re-indexes. Explore with `trace_path(mode="cross_service")`.

## 10. CI and containers

| Setting | Effect |
|---|---|
| `CBM_WORKERS=<n>` | Override parallel-indexing worker count — use in containers where `sysconf(_SC_NPROCESSORS_ONLN)` reports host CPUs instead of the cgroup quota. |
| `CBM_MEM_BUDGET_MB=<n>` | Pin the in-memory graph budget below the cgroup limit to leave headroom for siblings. |
| `MFA_CACHE_DIR=<path>` | Move indexes/config (e.g. onto a CI cache volume). All CBM processes on the account must agree on one canonical root. |
| `config set auto_index true` + `auto_index_limit <n>` | Auto-index new projects on first session connect (default limit 50,000 files). |
| `config set watcher_enabled false` / `auto_watch false` | Suppress background sync in ephemeral environments; re-index explicitly. Read once at daemon start — run `memory-for-ai daemon stop` after changing `watcher_enabled`. |

**Always-refused indexing roots** (independent of any configuration): filesystem/drive/UNC roots; top-level system trees (`/etc`, `/var`, `/usr`, `/home`, `/Users`, `C:\Windows`, `C:\Users`, `C:\ProgramData`, `C:\Program Files`); your home directory itself; credential directories at any depth (`.ssh`, `.aws`, `.gnupg`, `.kube`, `.docker`, …). Index project directories, not the world. Full environment reference: [CONFIGURATION.md](CONFIGURATION.md).

## 11. Failure modes & recovery

| Symptom | Cause → fix |
|---|---|
| `trace_path` returns 0 results | Name mismatch. `search_graph(name_pattern=".*PartialName.*")` first, use the exact qn. |
| Results mention the wrong repo | Missing/incorrect `project`. Run `list_projects`; the bare folder name works as an alias. |
| `stale_cursor` error | The project re-indexed under you. Re-run the original query from page one. |
| "session is scoped" refusal | The server is pinned with `--scope` — by design for per-project installs. Use the pinned project only. |
| Tool call refused: repo path outside allowed root | `CBM_ALLOWED_ROOT` or a `--scope` boundary confines indexing. Index inside the boundary. |
| `index_repository` returns `status:"degraded"` | Persisted-node verification fell below `CBM_DUMP_VERIFY_MIN_RATIO` (default 0.5) — the index exists but is not trustworthy; re-index and keep the response `logfile`. |
| Graph seems to miss brand-new code | Watcher lag or watcher disabled. `index_status` → then `index_repository` (incremental). |
| `/mcp` doesn't list the server | Restart the agent after install; verify the config path is absolute; test `echo '{}' \| /path/to/binary` returns JSON. |
| Graph UI not loading | Start with `--ui=true --port=9749`, open `http://localhost:9749`. Owned by the shared daemon — concurrent sessions don't duplicate it. |
| "secure daemon endpoint could not be created" | The default rendezvous ancestry failed the privacy walk. Set `MFA_RUNTIME_DIR` to a directory you own (see [CONFIGURATION.md](CONFIGURATION.md#relocating-the-daemon-rendezvous-directory)). |
| Version/build conflict on start | All CBM processes must share one exact build + cache root. Close other sessions or update via the install script (it coordinates a safe activation window). |
