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
3. **Fixed cost not counted:** the 18-tool manifest costs ~7K tokens per session in clients that list all tools. A one-question session can lose to grep on pure tokens; the advantage amortizes over a real working session. (Mitigations: Scout/Verify/Auditor scoped tool profiles, or CLI mode as used here.)
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
