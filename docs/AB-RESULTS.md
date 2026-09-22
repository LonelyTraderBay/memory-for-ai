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
3. **Fixed cost not counted:** the 23-tool manifest costs ~9K tokens per session in clients that list all tools. A one-question session can lose to grep on pure tokens; the advantage amortizes over a real working session. (Mitigations: Scout/Verify/Auditor scoped tool profiles, or CLI mode as used here.)
4. **Question mix favors structure by design** — 6 of 8 questions are structural, matching the product’s target workload. Q4/Q6 are the honesty controls.
5. Published-paper comparison (arXiv:2603.27277, 31 repos, blinded): 10× fewer tokens, 2.1× fewer tool calls, 83% vs 92% answer quality for the file-by-file baseline. The graph **locates**; it does not replace reading code when you need deep understanding.

## The workflow this evidence supports

1. **Locate with the graph** — `trace_path` / `search_graph` for who-calls-whom and blast radius (cheap, exact, transitive).
2. **Read the right slice** — `get_code_snippet` for just the function you will touch, not the whole file.
3. **Edit** as usual.
4. **Verify with `detect_changes`** — confirm the diff’s blast radius matches what you intended to affect.
5. **Drop to text tools deliberately** — `search_code` / grep for string literals, comments, and error messages; `ls` for directory shape.
6. **Guard freshness** — `index_status` before trusting line numbers; re-index is seconds and incremental.

## Reproduce

Follow [MEASURING.md](MEASURING.md) §3 (rigorous A/B protocol) with your own repository, question set, and model. Keep artifacts outside the worktrees, record raw paired counts beside every ratio, and publish your caveats with your numbers — as above.

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
