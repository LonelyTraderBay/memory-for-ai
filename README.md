# memory-for-ai

[![GitHub Release](https://img.shields.io/github/v/release/LonelyTraderBay/memory-for-ai?style=flat&color=blue)](https://github.com/LonelyTraderBay/memory-for-ai/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![CI](https://img.shields.io/github/actions/workflow/status/LonelyTraderBay/memory-for-ai/dry-run.yml?label=CI)](https://github.com/LonelyTraderBay/memory-for-ai/actions/workflows/dry-run.yml)
[![Languages](https://img.shields.io/badge/languages-162-orange)](#language-support)
[![Platform](https://img.shields.io/badge/macOS_%7C_Linux_%7C_Windows-supported-lightgrey)](https://github.com/LonelyTraderBay/memory-for-ai/releases/latest)
[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/LonelyTraderBay/memory-for-ai/badge)](https://scorecard.dev/viewer/?uri=github.com/LonelyTraderBay/memory-for-ai)
[![arXiv](https://img.shields.io/badge/arXiv-2603.27277-b31b1b?logo=arxiv)](https://arxiv.org/abs/2603.27277)

An MCP server that turns a codebase into a persistent knowledge graph — functions, classes, call chains, HTTP routes, cross-service links — so an AI coding agent answers structural questions with **graph queries instead of reading file after file**.

One self-contained native executable. 162 languages via vendored tree-sitter grammars, refined by embedded Hybrid-LSP type resolution. 18 MCP tools. No language runtime, no Docker, no API key, no telemetry — everything runs locally.

- **Are you an AI agent?** Read [docs/AGENT_GUIDE.md](docs/AGENT_GUIDE.md) — the complete operating manual (tool catalog, task→tool playbooks, correctness protocol, per-project tuning). [docs/llms.txt](docs/llms.txt) is the machine-readable index.
- **Installing for a specific project?** Jump to [Per-project install](#per-project-install) — one command, zero global config, one isolated graph named after the repo.
- **Want proof it pays off before adopting?** [docs/MEASURING.md](docs/MEASURING.md) — a 15-minute spot check and a full A/B protocol to measure token and tool-call savings on your own repository.

> **Research** — design and evaluation are described in [*Codebase-Memory: Tree-Sitter-Based Knowledge Graphs for LLM Code Exploration via MCP*](https://arxiv.org/abs/2603.27277) (arXiv:2603.27277): across 31 real repositories, 10× fewer tokens and 2.1× fewer tool calls vs. file-by-file exploration, at 83% answer quality (92% for the file-by-file baseline).

## Quick start

**macOS / Linux** (one line):

```bash
curl -fsSL https://raw.githubusercontent.com/LonelyTraderBay/memory-for-ai/main/install.sh | bash
```

**Windows** (PowerShell):

```powershell
Invoke-WebRequest -Uri https://raw.githubusercontent.com/LonelyTraderBay/memory-for-ai/main/install.ps1 -OutFile install.ps1
Unblock-File .\install.ps1        # remove Mark-of-the-Web
.\install.ps1
```

Then **restart your coding agent** and say **"Index this project"**. Done.

The installer downloads the verified release archive for your platform, verifies its SHA-256 against `checksums.txt`, installs the binary, and configures every coding agent it detects (Claude Code, Codex, Gemini CLI, Cursor, VS Code, Windsurf, and ~40 more — see [Multi-agent support](#multi-agent-support)). Options: `--skip-config` (binary only), `--dir=<path>`, `--clients=<list>`, `--project` / `--name=<name>` (per-project mode). Full reference, including all package managers, manual MCP config, containers/CI, and uninstall: [docs/INSTALL.md](docs/INSTALL.md).

> **Antivirus note:** Microsoft Defender may flag a release binary as `Trojan:Script/Wacatac.B!ml` — a known false positive (typically 61 of ~62 engines clean; the same family flags `gh`, llama.cpp, and Microsoft's own Go toolchain). Evidence and self-verification steps: [Antivirus False Positives](SECURITY.md#antivirus-false-positives).

## Per-project install

One binary can serve any number of repositories, but sometimes a project deserves its own fenced memory: an MCP server named after the repo, an index no other repo can see, and zero edits to global agent config. That is `--project`:

```bash
# From the repository root, after downloading install.sh (any OS)
bash install.sh --project
```

What it does: installs/refreshes the shared binary, writes a project-local `.mcp.json` entry named `memory-for-ai-<repo-directory>` pinned with `--scope=<repo>`, and indexes the repository immediately. An agent opened in that repo sees exactly one server serving exactly that graph; opening a different repo sees its own. The same flag works through the one-line pipes (`... | bash -s -- --project`) and PowerShell. Details and guarantees: [docs/INSTALL.md](docs/INSTALL.md#per-project-install) and [docs/CONFIGURATION.md](docs/CONFIGURATION.md#3b-per-project-scoped-sessions---scope-install---project).

## What it does

`index_repository` parses the whole tree (tree-sitter syntax pass + Hybrid-LSP type resolution), builds a graph of nodes (`Function`, `Class`, `Route`, `Package`, …) and edges (`CALLS`, `IMPORTS`, `IMPLEMENTS`, `DATA_FLOWS`, `HTTP_CALLS`, `CROSS_*`, …), and persists it to SQLite under `~/.cache/memory-for-ai/`. A background watcher re-indexes on Git/filesystem changes. After that, the agent's questions become millisecond graph queries:

```
You: "what calls ProcessOrder?"

Agent calls: trace_path(function_name="ProcessOrder", direction="inbound")
             → complete caller tree, one call, ~250 tokens

File-by-file alternative: grep 38 files, read ~33,000 tokens, still miss indirect callers.
```

There is **no built-in LLM**: your MCP client is the intelligence layer; this tool is the structural memory. Typical wins:

- **Callers / callees / blast radius** — `trace_path`, `detect_changes` answer in one call what grep cannot answer at any cost (transitive chains live in no single file).
- **Architecture in one call** — `get_architecture`: languages, packages, entry points, routes, hotspots, layers, community-detection clusters.
- **Dead code, complexity hotspots, dependency graph, security-evidence graph** — via `query_graph` (read-only Cypher subset).
- **Memory across sessions** — the graph persists; `manage_adr` persists architecture decisions beside it.

## Performance

Measured on Apple M3 Pro (see [docs/MEASURING.md](docs/MEASURING.md) to reproduce on your own workload):

| Operation | Time | Notes |
|-----------|------|-------|
| Linux kernel full index | 3 min | 28M LOC, 75K files → 4.81M nodes, 7.72M edges |
| Django full index | ~6 s | 49K nodes, 196K edges |
| Cypher query | <1 ms | Relationship traversal |
| Trace call path (depth 5) | <10 ms | BFS traversal |
| Dead-code detection | ~150 ms | Full graph scan |

**Token efficiency** (five structural queries on the same repo): ~3,400 tokens via the graph vs ~412,000 tokens via file-by-file exploration. A single-cause measurement recipe and the honest cost model (including the fixed per-session tool-list overhead) are documented in [docs/MEASURING.md](docs/MEASURING.md).

## Documentation map

| Document | What it covers | Primary audience |
|---|---|---|
| [docs/AGENT_GUIDE.md](docs/AGENT_GUIDE.md) | Operating manual: mental model, all 18 tools, task→tool playbooks, correctness protocol, per-project tuning | AI coding agents (and their humans) |
| [docs/INSTALL.md](docs/INSTALL.md) | Every install path: one-liners, per-project, package managers, containers/CI, update/uninstall, build from source, artifact verification | Whoever installs |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | Config files, `config set` keys, environment variables, scoped sessions | Operators, CI authors |
| [docs/MEASURING.md](docs/MEASURING.md) | Measuring answer quality, latency/stability, and token/tool-call savings on your repo | Evaluators |
| [docs/llms.txt](docs/llms.txt) | Machine-readable index of the above | AI agents |
| [SECURITY.md](SECURITY.md) | Reporting, release policy, antivirus false positives, supply chain | Everyone |

## CLI mode

Every MCP tool also runs as a local one-shot command (no daemon, no standing process; stdout stays machine-clean):

```bash
memory-for-ai cli index_repository --repo-path /path/to/repo
memory-for-ai cli list_projects
# use the "name" from list_projects as --project
memory-for-ai cli search_graph --project my-project --name-pattern '.*Handler.*' --label Function
memory-for-ai cli trace_path --project my-project --function-name Search --direction both
memory-for-ai cli query_graph --project my-project \
  --query 'MATCH (f:Function) RETURN f.name LIMIT 5'
memory-for-ai cli search_graph --project my-project --label Function | jq '.results[].name'
```

`cli <tool> --help` prints the flags generated from that tool's input schema. Arguments can also be piped as JSON on stdin.

## Hybrid LSP

Tree-sitter gives a syntactic AST; it cannot tell that `user.profile.display_name()` resolves to `Profile.display_name` three modules away. memory-for-ai embeds a lightweight C implementation of language type-resolution algorithms — structurally inspired by tsserver/typescript-go, pyright, gopls, Roslyn, Eclipse JDT, and rust-analyzer — that refines call/usage edges on every parse. No language-server process, no per-project setup.

Full type-aware resolution for **Python, TypeScript/JavaScript/JSX/TSX, PHP, C#, Go, C/C++, Java, Kotlin, Rust, Perl**: imports, generics, inheritance, JSX dispatch, traits/late static binding (PHP), LINQ + records (C#), embedded structs (Go), templates/namespaces (C++), overload + lambda resolution (Java), extension + scope functions (Kotlin), trait methods + UFCS (Rust), MRO + Exporter (Perl). All other grammars fall back to tree-sitter-textual resolution, so every file still produces a graph.

## Language support

162 languages, all parsed by vendored tree-sitter grammars compiled into the binary. Benchmarked against 64 real open-source repositories (78–49K nodes each):

| Tier | Score | Languages |
|------|-------|-----------|
| **Excellent** (≥90%) | | Lua, Kotlin, C++, Perl, Objective-C, Groovy, C, Bash, Zig, Swift, CSS, YAML, TOML, HTML, SCSS, HCL, Dockerfile |
| **Good** (75–89%) | | Python, TypeScript, TSX, Go, Rust, Java, R, Dart, JavaScript, Erlang, Elixir, Scala, Ruby, PHP, C#, SQL |
| **Functional** (<75%) | | OCaml, Haskell |

Also parsed (not yet benchmarked): Ada, Agda, Apex, ArkTS, Assembly, Astro, AWK, Beancount, BibTeX, Bicep, Bitbake, Blade, Cairo, Cap'n Proto, CFML, CFScript, Chialisp, Clojure, CMake, COBOL, Common Lisp, Crystal, CSV, CUDA, D, Devicetree, Diff, Dotenv, Elm, Emacs Lisp, F#, Fennel, Fish, Form, Fortran, Func, GDScript, Git Attributes, Gitignore, Gleam, GLSL, GN, Go Module, Go Template, GraphQL, Hare, HLSL, Hyprlang, INI, ISPC, Janet, Jinja2, JSDoc, JSON, JSON5, Jsonnet, Julia, Just, Kconfig, KDL, Lean 4, Linker Script, Liquid, LLVM IR, Luau, Magma, Makefile, Markdown, MATLAB, Mermaid, Meson, Mojo, Move, NASM, Nickel, Nix, ObjectScript Routine, ObjectScript UDL, Odin, Pascal, Pine Script, Pkl, PL/SQL, PO, Pony, PowerShell, Prisma, Properties, Protobuf, Puppet, PureScript, QML, Racket, Regex, Requirements, ReScript, RON, reStructuredText, Scheme, Slang, Smali, Smithy, Solidity, SOQL, SOSL, Squirrel, SSH config, Starlark, Svelte, Sway, SystemVerilog, TableGen, Tcl, Teal, Templ, Thrift, TLA+, Typst, Verilog, VHDL, Vim script, Vue, WGSL, WIT, Wolfram, XML, Zsh.

## Multi-agent support

`install` auto-detects and configures **45 supported automatic/conditional client surfaces** (39 automatic + 6 conditional/explicit) — Claude Code, Codex CLI, Gemini CLI, Zed, OpenCode, Cursor, VS Code, Windsurf, Kiro, Qwen Code, GitHub Copilot CLI, Junie, Factory Droid, Grok Build, Amp, Devin, and the rest of the matrix in [docs/INSTALL.md](docs/INSTALL.md#what-install-writes). It writes only documented MCP entries plus durable instructions, skills, and lifecycle hooks where the client documents a safe contract; it never enables experimental flags, plugins, or permission bypasses. Custom-agent formats receive three tiered graph profiles — **Scout** (fast provisional discovery), **Verify** (default, evidence-checked), **Auditor** (bounded exhaustive verification) — each biased to prove graph evidence against source via `check_index_coverage`. Preview exactly what would be written on your machine with `memory-for-ai install --dry-run`.

<details>
<summary>All 45 configured surfaces</summary>

Claude Code, Codex CLI, Gemini CLI, Zed, OpenCode, Antigravity, Aider, KiloCode, VS Code, Cursor, Windsurf, Augment / Auggie, OpenClaw, Kiro, Junie, Hermes, OpenHands, Cline, Warp, Qwen Code, GitHub Copilot CLI, Factory Droid, Crush, Goose, Mistral Vibe, Grok Build, Qoder CLI, Kimi Code CLI, GitLab Duo CLI, Rovo Dev CLI, Amp, Devin CLI / Local, Tabnine, Continue / cn (conditional), Visual Studio (conditional, Windows), TRAE (conditional), Roo Code (conditional), Amazon Q Developer IDE, CodeBuddy Code CLI, IBM Bob IDE (conditional), IBM Bob Shell, Pochi, Pi, Sourcegraph Cody (explicit opt-in), Oh My Pi (omp).
</details>

## Architecture

```
src/
  main.c              Entry point (MCP stdio server + CLI + install/update/config)
  daemon/             Per-account session coordination, IPC, lifecycle, shared jobs/watchers
  mcp/                MCP server (18 tools, JSON-RPC 2.0, session detection, auto-index)
  cli/                Install/uninstall/update/config (45 client surfaces, hooks, instructions)
  store/              SQLite graph storage (nodes, edges, traversal, search, Leiden/Louvain)
  pipeline/           Multi-pass indexing (structure → definitions → calls → HTTP links → config → tests)
  cypher/             Cypher query lexer, parser, planner, executor
  discover/           File discovery (.gitignore, .cbmignore, symlink handling)
  watcher/            Background auto-sync (Git/filesystem polling, adaptive intervals)
  traces/             Runtime trace ingestion
  ui/                 Local HTTP server + verified external 3D-UI asset pack
  foundation/         Platform abstractions (threads, filesystem, logging, memory)
  git/                Git context (worktree/detached state, HEAD for freshness checks)
  graph_buffer/       In-memory graph assembly during indexing, dumped to SQLite
  simhash/            MinHash/LSH near-clone fingerprints (SIMILAR_TO edges)
internal/cbm/         Vendored tree-sitter grammars (162 languages) + AST extraction engine
```

One session-coordination daemon is shared per account across clients: it owns watchers, shared indexing, and the optional graph UI (`memory-for-ai --ui=true --port=9749`, then open `http://localhost:9749`), so concurrent sessions never start duplicate services. All CBM processes must run the same exact build; native `install`/`update`/`uninstall` coordinate a safe account-wide activation window.

## Security & trust

This tool reads your codebase and writes your agent configuration — that is its job. All processing is 100% local; there is no telemetry and nothing phones home (`cbm` makes no network request of its own accord). Every release is verified before publication: VirusTotal scan of all executable candidates (single documented Microsoft `!ml` tolerance), SLSA Level 3 build provenance (`gh attestation verify …`), Sigstore cosign keyless signatures, SHA-256 `checksums.txt`, and CodeQL SAST gating. Full policy and audit trail: [SECURITY.md](SECURITY.md).

## License

MIT — see [LICENSE](LICENSE).
