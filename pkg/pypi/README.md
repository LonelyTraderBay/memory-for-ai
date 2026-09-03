# memory-for-ai

mcp-name: io.github.LonelyTraderBay/memory-for-ai

An MCP server that turns a codebase into a persistent knowledge graph — functions, classes, call chains, HTTP routes, cross-service links — so an AI coding agent answers structural questions with **graph queries instead of reading file after file**. Fully local: no API key, no Docker, no telemetry.

This Python wrapper downloads the selected `memory-for-ai` runtime set from [GitHub Releases](https://github.com/LonelyTraderBay/memory-for-ai/releases) on first run and verifies it before publishing it in your OS cache directory. The set contains the native executable and authenticated integration asset, with the graph UI always embedded.

## Installation

```bash
pip install memory-for-ai
# or
pipx install memory-for-ai
```

There is one composition per platform: the graph UI ships in every build, so no variant selection is needed.

## Usage

```bash
memory-for-ai install             # configure every detected coding agent (--dry-run previews)
memory-for-ai install --project   # repo-local fenced server named after the repo (no global changes)
memory-for-ai cli list_projects   # every MCP tool also runs as a one-shot CLI command
memory-for-ai --help
```

Restart your agent, say **"Index this project"** — done. Update with `pip install -U memory-for-ai`.

## Supported platforms

| OS      | Architecture |
|---------|-------------|
| macOS   | arm64, amd64 |
| Linux   | arm64, amd64 |
| Windows | arm64, amd64 |

## Documentation

- [Agent operating guide](https://github.com/LonelyTraderBay/memory-for-ai/blob/main/docs/AGENT_GUIDE.md) — tool catalog, task→tool playbooks, correctness protocol, per-project tuning
- [Installation reference](https://github.com/LonelyTraderBay/memory-for-ai/blob/main/docs/INSTALL.md) — every install path, CI/containers, build from source
- [Configuration reference](https://github.com/LonelyTraderBay/memory-for-ai/blob/main/docs/CONFIGURATION.md) — settings and environment variables
- [Measuring real effectiveness](https://github.com/LonelyTraderBay/memory-for-ai/blob/main/docs/MEASURING.md) — measure token and tool-call savings on your own repo

## License

MIT
