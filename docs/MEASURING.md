# Measuring real effectiveness

This guide gives you three instruments, in increasing rigor:

1. **A 15-minute spot check** — measure one real question from your own backlog, graph vs. file-by-file, with exact commands and formulas. Enough to justify adopting or skipping on your codebase.
2. **Built-in measurement surfaces** — what the tool itself records (index time, query counters, resource trajectory) and what it deliberately cannot see (your agent's model tokens).
3. **A rigorous A/B protocol** — frozen inputs, isolated sessions, paired metrics, honest aggregation — for publishable or decision-grade numbers.

Measure three questions separately, always: **answer quality**, **latency/stability**, and **cost (tokens/tool calls)**. A fast query does not prove a correct answer; a token reduction is only useful when the answer still meets your quality bar.

---

## 1. The 15-minute spot check

Pick one real structural question you actually need answered this week — the kind you'd normally grep for. Example used below: *"who calls `cbm_fopen`, directly or transitively?"*

### Setup

```bash
# One-time: index the repo at the exact commit you'll test
git -C /path/to/repo rev-parse HEAD          # record the SHA
memory-for-ai cli index_repository --repo-path /path/to/repo
memory-for-ai cli list_projects              # note the project name → $PROJ
```

### Condition A — graph

In a **fresh** agent session whose only exploration tool is memory-for-ai:

```
trace_path(function_name="cbm_fopen", direction="inbound", depth=3)
```

Record from your client's usage report (Claude Code: `/cost` and the tool-call log; other clients: their usage panels; API users: the `usage` field of the response): **input tokens, output tokens, number of tool calls** for the question-answering window, and whether the answer was correct (check a few callers in source).

Reference point from the maintainer's own dogfood run (this exact question class, 2026-09): one `trace_path` call returned all 66 callers complete in ~250 tokens; the grep equivalent was 75 matches across 38 files, ~33,000 tokens to read, and still missed indirect callers.

### Condition B — file-by-file

Fresh session, same model, same question; exploration restricted to text search / file listing / file reads only (no memory-for-ai tools). Record the same numbers.

### Compute

```
token reduction (%)     = 100 × (B_tokens − A_tokens) / B_tokens
tool-call reduction (%) = 100 × (B_calls  − A_calls ) / B_calls
ratio                   = B_tokens / A_tokens
```

If either count is zero, report "N/A" for that line — do not add pseudocounts. Publish the raw pair beside every ratio.

### Interpret honestly

- One question is a **smoke signal, not a benchmark**. Repeat with 5–10 questions of different shapes (discovery, callers, impact, architecture, cross-service) before making any claim about "your repo".
- Structural questions are where the graph wins by an order of magnitude. Pure text-lookup questions ("where is this error string printed?") may show little or no advantage — that's expected, and `search_code` exists for that class.
- **Count the fixed cost**: the tool manifest (`tools/list`) is roughly 7K tokens of per-session overhead in a client that lists all tools. A one-question session can lose to grep on pure tokens; the win amortizes over a real working session. For short sessions, scoped tool profiles (Scout/Verify/Auditor) or CLI mode shrink this cost.

## 2. Built-in measurement surfaces

| Surface | What it gives you | How to read it |
|---|---|---|
| `index_repository` response | Duration, node/edge counts, coverage report (`skipped` / `parse_partial` / `excluded`), per-run log file path | Keep it with your run artifacts; index-run class (full / artifact-assisted / incremental) determines comparability |
| `index_status` | Counts, git context, coverage summary, `confidence_score` / `confidence_reasons` | Freshness cross-check before measuring; not proof by itself |
| Diagnostics (`CBM_DIAGNOSTICS=1`) | Daemon writes `snapshot.json` + retained `trajectory.ndjson` (RSS, commit charge, page faults, fds, `query_count`, `query_errors`, `query_total_us` / `avg` / `max`) every 5 s | Discover exact paths from the `diagnostics.start` record in `${MFA_CACHE_DIR}/logs/memory-for-ai-daemon.log`. These are **CBM-side** query counters — they cannot see your agent's model tokens or non-CBM tool calls |
| Soak workload | Per-call latency + resource trend under sustained load | `scripts/soak-legs.sh build/c/memory-for-ai 10` from a source checkout → `soak-results*/{latency.csv,metrics.csv,summary.txt}`. Stability evidence, not answer-quality evidence |
| `compare_graphs` | Deterministic two-snapshot diffs | Verifying a refactor didn't silently drop nodes/edges |

To attribute daemon counters to one workload: use an otherwise idle daemon and record before/after values (or start a dedicated run).

## 3. Rigorous A/B protocol

Use when the numbers will drive a real decision or be published.

### Freeze the experiment

Record, before either condition runs: repository URL + exact commit SHA; the question set with expected scope / ground truth per question; CBM version, index mode, OS, machine; agent model + version, system prompt, context limits, per-question budget; condition order; the client/harness that records tokens and tool calls. Use the **same** repository SHA, questions, model, prompt, and budget for both conditions, and never let the second condition see the first's answers or tool results.

Do not run in a "clean-looking" working copy — untracked files count. Create detached worktrees per condition:

```bash
set -eu
SOURCE_REPO=/absolute/path/to/source-repository
SHA=$(git -C "$SOURCE_REPO" rev-parse "main^{commit}") || exit 1
RUN_ROOT=$(mktemp -d) || exit 1
git -C "$SOURCE_REPO" worktree add --detach "$RUN_ROOT/graph" "$SHA" || exit 1
git -C "$SOURCE_REPO" worktree add --detach "$RUN_ROOT/baseline" "$SHA" || exit 1
mkdir "$RUN_ROOT/artifacts"
```

Repeat the SHA + `git status --porcelain --untracked-files=all`-is-empty assertions immediately before indexing and before each condition; on failure, make a new worktree rather than "cleaning" a reused one. Keep artifacts outside both worktrees.

### Question set

Mix of: definition discovery, relationship/call-path questions, targeted source retrieval, architecture questions, cross-cutting questions. Record ground truth independently (derive it from the source at the frozen SHA, not from either condition's answer).

### Graph-condition preflight

Immediately before measurement: re-assert clean worktree, then a fresh successful index of that exact worktree and mode (`memory-for-ai cli index_repository --repo-path "$RUN_ROOT/graph"`), then cross-check `index_status` (`root_path`, `git.head_sha`, detached), then 1–2 representative queries returning symbols you verified at that SHA. If a `.memory-for-ai/graph.db.zst` artifact is present, classify the run artifact-assisted **only** if the index log records a successful `artifact.import` with `db` and `size_mb`. Save all outputs with the run.

### Capture usage honestly

Your MCP client or evaluation harness must capture agent usage — CBM cannot know your model's token consumption or your total tool-call count.

One row per measured window, per isolated session (each `(run_id, condition)` = one session answering one question):

```text
run_id,condition,repo_sha,question_id,window,input_tokens,output_tokens,total_tokens,tool_calls,wall_time_ms,answer_artifact,quality_score
```

- `tool_calls` counts **every** client tool invocation in the window — orchestration, retries, errors, zero-result calls included — not just CBM calls. Never substitute CBM's `query_count`.
- Useful windows: **answering** (markers around the answering phase) and **full-session** (orientation + dead ends + formatting). Full-session best represents an adopter's total cost; answering explains where a difference arose. Compare only windows both clients report directly; record N/A otherwise — never infer, partition, or duplicate session totals.
- Grade answers against source at the frozen SHA: PASS (1.0) / PARTIAL (0.5) / FAIL (0.0), N/A excluded from the denominator. For finer comparison: score correctness/completeness/specificity separately, blind the grader to condition, randomize order.

### Aggregate and report

For each window, over paired runs:

```
token reduction (%)     = 100 × (baseline − graph) / baseline      # N/A if baseline = 0
tool-call reduction (%) = 100 × (baseline − graph) / baseline      # N/A if baseline = 0
token ratio             = baseline / graph                         # N/A if graph = 0
```

Publish: raw paired counts, per-pair quality scores, run count, aggregation method (chosen **before** looking at results), failures and zero-result calls, and the frozen controls (SHA, model, prompts, machine, order). Retain every run. **Never generalize one repository / question set / model / machine into a universal claim.**

## 4. Published baselines

| Source | Result | Scope |
|---|---|---|
| [arXiv:2603.27277](https://arxiv.org/abs/2603.27277) | 10× fewer tokens, 2.1× fewer tool calls vs. file-by-file exploration, at 83% answer quality (92% for the file-by-file baseline) | 31 real repositories, blinded grading |
| README performance table | Linux kernel full index 3 min; Cypher <1 ms; five structural queries ~3,400 vs ~412,000 tokens | Single machine (Apple M3 Pro); exact reproduction needs the original inputs |
| Maintainer dogfood (2026-09) | Callers query: ~250 tokens / 1 call vs ~33,000 tokens of reading, 66 callers complete vs grep's incomplete picture | One repository, one question class |

Treat these as calibration points for your own measurement, not as expectations: your repo, language mix, question mix, and session shape move the numbers in both directions.
