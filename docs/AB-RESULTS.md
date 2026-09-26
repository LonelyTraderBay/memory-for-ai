# A/B results — memory-for-ai measured on its own repository

This is a worked example of the protocol in [MEASURING.md](MEASURING.md), run on this repository itself. It exists so adopters can see what a real measurement looks like end to end — including the parts that went wrong — before running their own.

**Read this first:** this is a single-repo, single-machine, single-day case study. The grader was not blind, and tokens are estimated from tool-output bytes (see [Limitations](#limitations)). Per the rule in MEASURING.md: never generalize one repository / question set / model / machine into a universal claim. Use this as a calibration point, then measure your own workload.

## Frozen setup

| Control | Value |
|---|---|
| Repository | `LonelyTraderBay/memory-for-ai` @ `f866cd3f` (clean worktree verified) |
| CBM version | v0.10.8 (installed release binary, Windows amd64) |
| Date | 2026-09-14 |
| Index after re-index | 26,204 nodes · 143,016 edges · full mode · re-index wall time 18.5 s |
| Conditions | A = graph tools only (same binary, CLI mode) · B = text search + file listing + file reads only |
| Token estimate | tool-output bytes ÷ 4 (client usage meter unavailable in this harness) |
| Question set | 8 questions: callers, callees, definition, architecture, snippet, text lookup, imports, transitive impact |
| Grading | PASS / PARTIAL / FAIL against source at the frozen SHA, with spot-checks opened in the real files |

## The freshness incident (read this before trusting any line number)

The measurement almost shipped wrong numbers. The pre-existing index was **12 days stale** (database mtime 2026-09-02). Relationship *names* it returned were still correct, but line-level output had drifted:

| Symbol | Stale index said | Source at `f866cd3f` says |
|---|---|---|
| `cbm_mcp_handle_tool` | line 13844 | line 13984 |
| `where_bind_text` | line 7609 | line 7649 |

One `index_repository` re-index (18.5 s) brought every line number and snippet back to exact agreement with source. **Lesson, now baked into [AGENT_GUIDE.md §7](AGENT_GUIDE.md): check `index_status` freshness before trusting line-level output.** Relationship topology degrades gracefully; coordinates do not.

## Results

Bytes = total tool-output bytes consumed by the agent for that question (token ≈ bytes ÷ 4). "page 1" marks paginated results where the first page already answered the question; full-fetch estimates in parentheses.

| # | Question shape | A (graph) calls / bytes | B (grep + reads) calls / bytes | A grade | B grade |
|---|---|---|---|---|---|
| Q1 | Direct callers of `cbm_store_open_path` | 1 / 557 (page 1; full ≈ 2–3 pages) | 8 / 1,173,210 | **PASS** — spot-check `cbm_gbuf_load_from_db` @ graph_buffer.c:923 ✓ | PASS, at 1.1 MB of reading |
| Q2 | Callees of `run_cli` | 2 / 2,707 | 2 / 3,979 | **PASS** — 33 callees; spot-check `cbm_cli_build_args_json` @ main.c:763 ✓ | **PARTIAL** — an 80-line window showed 3 of 33 |
| Q3 | Definition of `cbm_mcp_handle_tool` | 1 / 236 | 2 / 654,086 | **PASS** — exact lines 13984–14011 after re-index | PASS (had to open a 654 KB file) |
| Q4 | Architecture: list subsystems | 1 / 4,720 | 1 / 184 | PASS | PASS — **B cheaper** (control question: a directory listing wins) |
| Q5 | Source snippet of `where_bind_text` | 1 / 1,292 | 2 / 3,456 | **PASS** — exact lines 7649–7661 | PASS |
| Q6 | Text lookup: "conflicting CBM process" | 1 / 1,335 | 1 / 243 | PASS | PASS — **B cheaper** (raw text lookup, as expected) |
| Q7 | Imports of cypher.c | 1 / 251 | 1 / 212 | PASS (14 rows incl. system-header noise) | PASS — near tie |
| Q8 | Depth-2 impact of `cbm_store_close` | 1 / 349 (page 1; full ≈ 1 KB) | 9 / 1,220,578 | **PASS** — 69 transitive callers | **PARTIAL** — indirect callers not feasible by reading |

One behavior worth calling out: in Q2 the tool **refused to guess** when the bare name was ambiguous, returned an `ambiguous` error with qualified-name suggestions, and answered correctly once given the qualified name. That is the designed accuracy guardrail, and it cost one extra call (939 bytes) instead of a wrong answer.

## Totals

| | A (graph) | B (file-by-file) |
|---|---|---|
| Questions answered fully | **8 / 8 PASS** | 6 PASS + 2 PARTIAL |
| Tool-output volume | ≈ 11,447 bytes (+ ~2 KB if paginated to completion) ≈ **~3.4K tokens** | ≈ 3,059,948 bytes ≈ **~765K tokens** |
| Tool calls | ~10–12 | 19 |

- **Token reduction ≈ 99.6%** under the MEASURING.md read-whole-file rule.
- **Charity bound:** even if B is credited with reading *only* tight windows around each match (~60 KB total instead of whole files), the graph is still **~4–5× cheaper** — and B's two PARTIAL answers (transitive/callee completeness) do not improve, because the missing information lives outside any single window.
- The two questions B won on cost (Q4 directory listing, Q6 raw text lookup) are exactly the classes MEASURING.md predicts the graph will not win. Use `search_code` / grep / `ls` for those.

### Earlier same-repo dogfood point (2026-09, same binary)

Full transitive inbound trace of `cbm_fopen`: **11 calls / 17,290 bytes ≈ 4.3K tokens for all 218 callers**, 4/4 spot-checked callers confirmed in source. Consistent with the maintainer reference point in MEASURING.md (~250 tokens for a first-page callers answer).

## Limitations

1. **Tokens are estimated** as bytes ÷ 4 of tool output — not the client’s real usage meter. Direction and rough magnitude are trustworthy; the second significant digit is not.
2. **Not blind:** the same agent ran both conditions and graded them. Ground truth was re-verified against source at the frozen SHA, but grader independence is the strongest reason to distrust the 8/8 vs 6/8 gap.
3. **Fixed cost not counted in this 2026-09-14 case study:** the latest 2026-09-24 Windows `v0.12.0-rc.2` candidate measured 8,347 `o200k_base` tokens (38,128 wire bytes) with all 23 tools; `analysis` measured 5,156 tokens/23,331 bytes with 14 tools, and `scout` 3,254 tokens/14,855 bytes with 8 tools. The earlier 2026-09-23 spot check measured 8,404 tokens. A one-question session can lose to grep on pure tokens; client caching/accounting changes the actual session cost. These are payload measurements, not model usage-meter totals.
4. **Question mix favors structure by design** — 6 of 8 questions are structural, matching the product’s target workload. Q4/Q6 are the honesty controls.
5. Published-paper comparison (arXiv:2603.27277, 31 repos, blinded): 10× fewer tokens, 2.1× fewer tool calls, 83% vs 92% answer quality for the file-by-file baseline. The graph **locates**; it does not replace reading code when you need deep understanding.

## The workflow this evidence supports

1. **Locate with the graph** — `trace_path` / `search_graph` for relationships represented in the indexed graph. Coverage depends on language constructs; verify empty/exhaustive caller claims against coverage and source.
2. **Read the right slice** — `get_code_snippet` for just the function you will touch, not the whole file.
3. **Edit** as usual.
4. **Verify with `detect_changes`** — confirm the diff’s blast radius matches what you intended to affect.
5. **Drop to text tools deliberately** — `search_code` / grep for string literals, comments, and error messages; `ls` for directory shape.
6. **Guard freshness and coverage** — check `index_status` before trusting line numbers and `check_index_coverage` before negative/exhaustive claims; re-index when stale.

## Reproduce

Follow [MEASURING.md](MEASURING.md) §3 (rigorous A/B protocol) with your own repository, question set, and model. Keep artifacts outside the worktrees, record raw paired counts beside every ratio, and publish your caveats with your numbers — as above.

## External-adopter spot check: VitTrade React (2026-09-23)

This is a **single retrieval smoke check**, not a rigorous A/B benchmark and not evidence of repo-wide token savings. It records a useful negative result before adoption: a React callback is present in source but absent from the current `CALLS` trace.

| Control | Value |
|---|---|
| Repository | `LonelyTraderBay/vittrade-react` @ `1eab2d7a25a270f965c03bc0adeadcbdd2abc548` (detached clean temporary checkout; the user's working copy was not changed) |
| Tool build | Windows native working-tree build based on `memory-for-ai` HEAD `284d48a1`; reports `dev`, not a release artifact |
| Index | Full mode · 9,400 nodes · 41,892 edges · 853 indexed file hashes · 0 skipped · 69 parse-partial files/ranges · medium confidence (0.85) · coverage ratio unavailable |
| Question | What does `TradePage.handleConfirmOrder` do, and what invokes it? |
| Tokenizer | `o200k_base` via `tiktoken` 0.14.0; counts below are tool-response/output proxies, not client usage-meter totals |

| Condition | Calls | Output-token proxy | Result |
|---|---:|---:|---|
| memory-for-ai: `search_graph` + `get_code_snippet` + `trace_path` + `search_code` fallback | 4 | 1,410 | Correct after the text-search fallback; `trace_path` alone returned `callers_total: 0` |
| Baseline: `rg -n -C 2` + read the handler and JSX binding slice | 2 | 936 | Correct; source shows `onClick={handleConfirmOrder}` at `TradePage.tsx:411` |

For this narrow question, the graph workflow used **50.6% more measured output tokens and twice as many calls** than the targeted baseline, before any fixed manifest cost. The current MCP `tools/list` raw stdio response contains 23 tools and measures **8,404 `o200k_base` tokens** (38,362 UTF-8 bytes; `tiktoken` 0.14.0); the scoped `analysis` and `scout` responses measure 5,190 and 3,275 tokens respectively. A client that sends the whole manifest would add that full-profile session overhead, though client caching/accounting can differ. These counts tokenize the exact JSON-RPC response plus its line ending, not a client usage meter. The handler snippet itself matched source, but the graph does not currently model this JSX callback as an inbound `CALLS` edge. Do not interpret zero callers as proof that a React handler is unused; verify callback bindings in source.

### Supplementary fixed-query probes (same VitTrade snapshot)

These four additional spot checks use the same tokenizer and clean checkout, but were not run as isolated, blinded model sessions. “memory-for-ai” is the actual shortest tool path tried for each question; where the graph could not answer the relation, the path uses `search_code` as an explicit fallback. Baseline is targeted `rg -n -C 2`. Counts are tool-response output only; they exclude prompts, reasoning, generated answers, and any cached/client-specific accounting.

| Question | memory-for-ai path | Calls A/B | Output-token proxy A/B | Observed result |
|---|---|---:|---:|---|
| Which file calls `createProtectedRoutes`? | inbound `trace_path` / targeted `rg` | 1/1 | 90/475 | Graph finds the caller file, but folds four concrete call sites into one file node; partial if call-site count matters. |
| Where is route literal `/trade/:pairId`? | `search_code` / targeted `rg` | 1/1 | 116/91 | Both locate the source line; grep is cheaper. |
| What binds `handleConfirmOrder` in JSX? | `search_code` fallback / targeted `rg` | 1/1 | 121/55 | Text fallback finds `onClick={handleConfirmOrder}`; graph `CALLS` trace does not represent this callback edge. |
| Where does `ORDER_PLACED` occur? | `search_code` / targeted `rg` | 1/1 | 158/137 | Both locate the two occurrences; grep is cheaper. |

Across the original handler question and these four probes, the observed paths total **8 calls / 1,895 output-token proxy** for memory-for-ai versus **6 calls / 1,694 tokens** for targeted `rg` (**11.9% more output tokens and 33.3% more calls** for memory-for-ai, before the manifest). This is only a descriptive sum across five hand-picked questions—not a benchmark estimate or a general savings claim. The 8,404-token full manifest alone is larger than either side's measured query output if a client sends it for this session; scoped profiles lower that fixed cost but expose fewer tools. The useful positive signal is the caller-file lookup; narrow literal and callback-binding questions favor grep, and callback completeness needs source verification.

**Decision:** the 2026-09-23 spot checks do not establish savings for VitTrade. The follow-up below adds an exact-tokenizer retrieval measurement, but still is not a blinded model-session benchmark or an actual client usage-meter reading. Use memory-for-ai as a locator, then verify JSX callbacks and any code you plan to change with source search/snippets. The 69 partial parses and unavailable coverage ratio also mean the index is not proven complete for this snapshot.

### RC2 same-SHA retrieval run (2026-09-24)

This is a deterministic, scripted retrieval comparison for eight fixed question shapes—not an isolated same-model A/B session. It improves on the earlier bytes÷4 proxy by tokenizing the exact MCP JSON-RPC responses and baseline text outputs with `tiktoken`, while still excluding prompts, reasoning, generated answers, client caching and the model's actual usage-meter accounting. The same agent selected and graded both conditions; the source was then checked directly, so grading was not blind.

| Control | Value |
|---|---|
| Target | `LonelyTraderBay/vittrade-react` @ `1eab2d7a25a270f965c03bc0adeadcbdd2abc548`; two clean detached clones, original working copy untouched |
| Tool | `memory-for-ai v0.12.0-rc.2`, local candidate SHA-256 `6366400071b5d0412403fd40e84c2656edd5f24fff06542fbb640dede9e54e30`; not a published release |
| Index | Full · 9,400 nodes · 41,892 edges · 853 indexed-file hashes · 69 parse-partial files · 0 skipped · medium confidence; coverage-ratio denominator unavailable |
| Freshness | All nine queried source paths reported `metadata_match`; `RiskManagementDemoPage.tsx` has an unrelated partial-parse range at line 213, while the measured `ORDER_PLACED` use is at line 99 |
| Tokenizer / capture | `tiktoken` 0.14.0 / `o200k_base`; graph counts include each complete JSON-RPC tool response plus its line ending; baseline counts include only the corresponding `rg`/source-read outputs |

| # | Question | Graph calls / tokens | Targeted `rg`/read calls / tokens | Source-checked result |
|---|---|---:|---:|---|
| Q1 | What does `TradePage.handleConfirmOrder` do, and what invokes it? | 4 / 1,448 | 2 / 473 | PASS only with fallback: `trace_path` says 0 callers; `search_code` finds the JSX binding at line 411. Graph-only caller answer is incomplete. |
| Q2 | Which file calls `createProtectedRoutes`? | 1 / 90 | 1 / 475 | PASS at file level: graph identifies `routes.ts`, but collapses three call expressions (and an import) into one file node. |
| Q3 | Where is `/trade/:pairId` referenced? | 1 / 198 | 1 / 371 | PARTIAL: both locate references, but hits include comments; actual web handling uses `/w/trade/` prefix logic rather than this exact route literal. |
| Q4 | What binds `handleConfirmOrder` in JSX? | 1 / 172 | 1 / 112 | PASS; targeted grep is cheaper for this narrow text question. |
| Q5 | Find `ORDER_PLACED` declarations and uses. | 1 / 306 | 1 / 372 | PASS for locating; graph also returns enclosing symbols beyond the literal occurrences. |
| Q6 | Which components call `useActionToast`? | 1 / 1,432 | 1 / 5,604 | PASS on count/scope: 68 graph caller rows match 68 direct source call expressions after excluding the hook's documentation/declaration and test files. |
| Q7 | What outgoing function relationship does `useRefresh` expose? | 1 / 98 | 2 / 810 | PARTIAL: graph exposes the local `update` callback but omits React hooks/state setters and other behavior visible in source. |
| Q8 | How does `fmtFee` select precision and which formatter does it call? | 1 / 380 | 1 / 123 | PASS; the tiny function is cheaper to inspect with targeted grep. |
| **Total** | **8 fixed questions** | **11 / 4,124** | **10 / 8,340** | Query-output-only graph payload is **50.6% lower**; graph needed one more tool call. |

The fixed MCP manifest changes the session-level conclusion. Exact `tools/list` measurements were:

| Profile | Tools | Manifest bytes / tokens | Initialize tokens |
|---|---:|---:|---:|
| Full (default) | 23 | 38,128 / 8,347 | 272 |
| `analysis` | 14 | 23,331 / 5,156 | 232 |
| `scout` | 8 | 14,855 / 3,254 | 189 |

Adding just the manifest and initialize response gives **12,743 tokens** for the eight-question full-profile graph session (**52.8% more** than the 8,340 baseline-output tokens). The `analysis` profile gives **9,512 tokens** (**14.1% more**); it includes `search_code`, which is needed for the JSX callback fallback. `scout` omits `search_code`, so it cannot run this full workflow. These comparisons do not include the agent's separate shell-tool schema or usage accounting, and should not be treated as a client-meter result.

**Practical reading:** structural fan-out lookup (`useActionToast`) is a clear per-query win; literal search and tiny snippets often favor grep. The callback question demonstrates why zero graph callers must not be treated as proof of no use. For this eight-question mix, however, neither full MCP nor `analysis` beats the targeted baseline after schema startup cost. Prefer CLI mode for isolated lookups, keep `search_code`/source inspection as a required fallback, and measure a real agent session before claiming token savings. The coverage ratio remains unknown, so do not make repo-wide completeness claims.

The local raw paired payloads are retained outside the worktrees at `build/vittrade-benchmark-20260924-v2/artifacts/vittrade-benchmark-20260924-final.json`; the artifact is ignored build output, not part of a published release.

### Source-matched `0.12.0-rc.2` release-build recheck (2026-09-24)

The Windows release-profile binary was rebuilt from the same source diff used by the regression suite (`scripts/build.sh --with-ui --version 0.12.0-rc.2 CC=clang CXX=clang++`), then passed through `prepare-release-candidates.sh` and `package-release.sh`. Linker-output SHA-256: `3506ec17c58277a3ccda514e1c5f3456dcffbcb4264da9a9673ae2bd8fa70693`; selected stripped payload SHA-256: `4f411231844ba51feb43239705b255708879624bfc57c7dab2576858c55ee7ea`. The ZIP and MCPB contain that selected payload. This is a local Windows/amd64 candidate, not a published release.

The same 11 MCP requests were replayed with that exact extracted candidate against a fresh full index of the unchanged VitTrade commit above: 9,400 nodes, 41,892 edges, 853 indexed-file hashes, 69 parse-partial files, 0 skipped; the coverage denominator remains unknown. All calls returned `isError=false`; their result text matched the earlier capture after removing only the nondeterministic `elapsed_ms` field. The replay therefore reproduces the earlier query-only result: **4,124 `o200k_base` tokens / 11 graph calls** versus the unchanged targeted baseline **8,340 tokens / 10 calls** (50.6% lower query output, before manifest cost). The source-matched binary's manifests also remained **8,347 / 5,156 / 3,254 tokens** for full / `analysis` / `scout`, so manifest-inclusive totals remain **12,743** (52.8% more than baseline) and **9,512** (14.1% more) for full and `analysis`. These are still response-token proxies, not a client usage-meter measurement.

The extracted ZIP artifact passed the native Windows amd64 artifact smoke, including installer checksum/extraction, MCP stdio and Content-Length framing, install/uninstall, UI HTTP, shutdown, and daemon retirement. This verifies the local candidate path only; other OS/architecture release lanes, release attestations, and public publication are not covered by this run.

## A/B — edit tools: `rename_symbol` / `move_symbol` vs grep + full-file rewrite (2026-09-19/20)

Second measurement family, this time for the **edit** path (THIET-KE-EDIT-TOOLS.md §8). Mechanical execution by `scripts/ab-edit-tools.py` against the dev build at `beecc49b` — no model in the loop, both conditions executed by the same script, quality proven by diffing the resulting trees (must be byte-identical after CRLF normalization).

**Rename task:** rename one Python function, fan-out rising from 2 to 12 occurrences across 1–5 files. **Move task (2026-09-20):** move one Python function to an existing destination module, fan-out rising from 0 to 4 importer files (2–6 files touched). Two file-size scenarios: `small` (bare fixture, ~10 lines/file) and `padded300` (~300 lines of inert code per file, simulating realistic module size).

- **Condition A (edit tools):** `rename_symbol` dry-run plan → apply (`force=true`) → `search_graph` verify = **3 calls**, plus one up-front `index_repository` (amortized per project, not per rename).
- **Condition B (manual):** `grep -rnw` → read every matched file **in full** → rewrite every file **in full** → verify grep (MEASURING.md read-whole-file rule).
- Token estimate = tool-output bytes ÷ 4, same convention as the read-path table above.

| task | files | calls A/B | tokens A (est) | tokens B (est) | token reduction | quality |
|---|---|---|---|---|---|---|
| small/t1_fanout_1 | 1 | 3/4 | 935 | 86 | −987.2% | PASS |
| small/t2_fanout_2 | 2 | 3/6 | 1,035 | 167 | −519.8% | PASS |
| small/t3_fanout_3 | 3 | 3/8 | 1,084 | 249 | −335.3% | PASS |
| small/t4_fanout_4 | 4 | 3/10 | 1,133 | 330 | −243.3% | PASS |
| small/t5_fanout_5 | 5 | 3/12 | 1,186 | 477 | −148.6% | PASS |
| **total small** | 15 | 15/40 | 5,373 | 1,309 | **−310.5%** | 5/5 PASS |
| padded300/t1_fanout_1 | 1 | 3/4 | 938 | 3,802 | 75.3% | PASS |
| padded300/t2_fanout_2 | 2 | 3/6 | 1,039 | 7,599 | 86.3% | PASS |
| padded300/t3_fanout_3 | 3 | 3/8 | 1,089 | 11,397 | 90.4% | PASS |
| padded300/t4_fanout_4 | 4 | 3/10 | 1,139 | 15,194 | 92.5% | PASS |
| padded300/t5_fanout_5 | 5 | 3/12 | 1,193 | 19,057 | 93.7% | PASS |
| **total padded300** | 15 | 15/40 | 5,398 | 57,049 | **90.5%** | 5/5 PASS |

**Move results** (same protocol; condition A = `move_symbol` dry-run plan → apply with `expected_files` parsed from the plan → `search_graph` verify = 3 calls; condition B additionally reads the destination module in full, which grep alone never surfaces):

| task | files | calls A/B | tokens A (est) | tokens B (est) | token reduction | quality |
|---|---|---|---|---|---|---|
| small/m1_fanout_0 | 2 | 3/6 | 962 | 95 | −912.6% | PASS |
| small/m2_fanout_1 | 3 | 3/8 | 991 | 176 | −463.1% | PASS |
| small/m3_fanout_2 | 4 | 3/10 | 1,019 | 257 | −296.5% | PASS |
| small/m4_fanout_3 | 5 | 3/12 | 1,047 | 338 | −209.8% | PASS |
| small/m5_fanout_4 | 6 | 3/14 | 1,075 | 420 | −156.0% | PASS |
| **total small** | 20 | 15/50 | 5,094 | 1,286 | **−296.1%** | 5/5 PASS |
| padded300/m1_fanout_0 | 2 | 3/6 | 970 | 7,526 | 87.1% | PASS |
| padded300/m2_fanout_1 | 3 | 3/8 | 999 | 11,323 | 91.2% | PASS |
| padded300/m3_fanout_2 | 4 | 3/10 | 1,027 | 15,120 | 93.2% | PASS |
| padded300/m4_fanout_3 | 5 | 3/12 | 1,055 | 18,917 | 94.4% | PASS |
| padded300/m5_fanout_4 | 6 | 3/14 | 1,083 | 22,715 | 95.2% | PASS |
| **total padded300** | 20 | 15/50 | 5,134 | 75,601 | **93.2%** | 5/5 PASS |

Move mirrors rename almost exactly on the A side (≈1.0–1.1K tokens flat, the plan is a per-file table plus a REVIEW section) and is slightly *more* favourable than rename at padded300 (93.2% vs 90.5% total reduction) because a manual move must also read the destination module in full — content that never mentions the symbol and that grep alone cannot surface.

Raw paired counts: `build/ab-edit-tools/ab_edit_results.csv` (regenerate with `python scripts/ab-edit-tools.py`).

### What this actually shows

1. **Break-even is file size, not fan-out.** A's cost is ~flat (≈1.0–1.2K tokens per rename, dominated by the plan/diff preview) regardless of occurrences or file size. B's cost grows linearly with total bytes of the files touched. On ~10-line files the edit tools **lose** (fixed overhead exceeds rewriting a tiny file); at ~300 lines/file they cut tokens **75–94%**. Real modules are usually closer to the second scenario.
2. **Call-count win is constant:** 3 calls vs 2·files + 2 (grep + verify). At 5 files that is 3 vs 12.
3. **Quality is equal by construction** — all 10 tree diffs byte-identical, and A's edits go through syntax-gate + atomic write + journal (crash-safe, undoable), which the mechanical full-file rewrite does not provide.
4. The honest negative result (`small` scenario) sets the usage rule: **for a rename inside one small file, plain read/edit is cheaper; use `rename_symbol` when the change fans out across files or the files are large** — exactly the workloads where manual rewriting is also the most error-prone.

### Limitations (edit-path specific)

5. **Synthetic fixture, mechanical executor.** No model session: real sessions add reasoning tokens to both conditions, and a model doing condition B might edit only matched line ranges instead of rewriting whole files (cheaper than our B, but then it must locate every safe edit boundary itself — the accuracy risk the graph removes).
6. **Padding is inert code**, not realistic surrounding logic; it measures the pure "pay for unrelated content" effect, which is the dominant term but not the only one.
7. **A's fixed cost includes the CLI/daemon response envelope** (~1K tokens/rename); MCP clients with cached tool manifests may see slightly different per-call overhead. Direction and rough magnitude are trustworthy; the exact break-even line count is not.
8. Same token-estimation caveat as the read path (bytes ÷ 4, not the client's meter).
