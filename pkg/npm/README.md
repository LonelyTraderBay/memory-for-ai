# memory-for-ai

[![npm](https://img.shields.io/npm/v/memory-for-ai?style=flat&color=blue)](https://www.npmjs.com/package/memory-for-ai)
[![GitHub Release](https://img.shields.io/github/v/release/LonelyTraderBay/memory-for-ai?style=flat&color=blue)](https://github.com/LonelyTraderBay/memory-for-ai/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-green)](https://github.com/LonelyTraderBay/memory-for-ai/blob/main/LICENSE)
[![Platform](https://img.shields.io/badge/macOS_%7C_Linux_%7C_Windows-supported-lightgrey)](https://github.com/LonelyTraderBay/memory-for-ai/releases/latest)

An MCP server that turns a codebase into a persistent knowledge graph — functions, classes, call chains, HTTP routes, cross-service links — so an AI coding agent answers structural questions with **graph queries instead of reading file after file**. This npm wrapper downloads, verifies, and caches the native runtime set for your platform; Node.js owns download, cache repair, and launch.

One self-contained native executable behind the wrapper. 162 languages via vendored tree-sitter grammars, refined by embedded Hybrid-LSP type resolution. 18 MCP tools. No Docker, no API key, no telemetry — everything runs locally.

- **AI agents:** read [docs/AGENT_GUIDE.md](https://github.com/LonelyTraderBay/memory-for-ai/blob/main/docs/AGENT_GUIDE.md) — the complete operating manual.
- **Evaluating effectiveness:** [docs/MEASURING.md](https://github.com/LonelyTraderBay/memory-for-ai/blob/main/docs/MEASURING.md) — a 15-minute spot check and a full A/B protocol.

## Installation

```bash
npm install -g memory-for-ai-mcp
```

The runtime set for your platform is downloaded and verified automatically at install time. One composition per platform; the graph UI is always included (the former `CBM_VARIANT=ui` opt-in is obsolete).

Then configure your coding agents and index:

```bash
memory-for-ai install      # configures all 45 supported automatic/conditional client surfaces (--dry-run previews)
```

<details>
<summary>All 45 configured surfaces</summary>

Claude Code, Codex CLI, Gemini CLI, Zed, OpenCode, Antigravity, Aider, KiloCode, VS Code, Cursor, Windsurf, Augment / Auggie, OpenClaw, Kiro, Junie, Hermes, OpenHands, Cline, Warp, Qwen Code, GitHub Copilot CLI, Factory Droid, Crush, Goose, Mistral Vibe, Grok Build, Qoder CLI, Kimi Code CLI, GitLab Duo CLI, Rovo Dev CLI, Amp, Devin CLI / Local, Tabnine, Continue / cn (conditional), Visual Studio (conditional, Windows), TRAE (conditional), Roo Code (conditional), Amazon Q Developer IDE, CodeBuddy Code CLI, IBM Bob IDE (conditional), IBM Bob Shell, Pochi, Pi, Sourcegraph Cody (explicit opt-in), Oh My Pi (omp).
</details>

Restart your agent. Say **"Index this project"** — done.

## Usage

```bash
memory-for-ai install          # configure all detected coding agents
memory-for-ai install --project  # repo-local fenced server named after the repo (no global changes)
memory-for-ai cli list_projects   # every MCP tool also runs as a one-shot CLI command
memory-for-ai --version
memory-for-ai --help
memory-for-ai uninstall        # remove owned agent configs and the binary
```

Updates go through npm on every platform: `npm install -g memory-for-ai-mcp-mcp@latest`. The command stays `memory-for-ai` (the package name differs because `memory-for-ai` on npm is held by an unrelated third party).

## Supported Platforms

| OS      | Architecture |
|---------|-------------|
| macOS   | arm64, amd64 |
| Linux   | arm64, amd64 |
| Windows | arm64, amd64 |

## Learn more

- [Agent operating guide](https://github.com/LonelyTraderBay/memory-for-ai/blob/main/docs/AGENT_GUIDE.md) — tool catalog, task→tool playbooks, correctness protocol, per-project tuning
- [Installation reference](https://github.com/LonelyTraderBay/memory-for-ai/blob/main/docs/INSTALL.md) — every install path, CI/containers, build from source
- [Configuration reference](https://github.com/LonelyTraderBay/memory-for-ai/blob/main/docs/CONFIGURATION.md) — settings and environment variables
- [Measuring real effectiveness](https://github.com/LonelyTraderBay/memory-for-ai/blob/main/docs/MEASURING.md) — tokens, tool calls, answer quality on your repo

## License

MIT
