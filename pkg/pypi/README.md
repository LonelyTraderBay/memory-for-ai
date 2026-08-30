# memory-for-ai

mcp-name: io.github.LonelyTraderBay/memory-for-ai

**Fast code intelligence engine for AI coding agents.** Indexes an average repository in milliseconds, the Linux kernel (28M LOC) in 3 minutes. Answers structural queries in under 1ms.

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
memory-for-ai install   # configure your coding agents
memory-for-ai --help
```

## Supported platforms

| OS      | Architecture |
|---------|-------------|
| macOS   | arm64, amd64 |
| Linux   | arm64, amd64 |
| Windows | arm64, amd64 |

## Full documentation

See [github.com/LonelyTraderBay/memory-for-ai](https://github.com/LonelyTraderBay/memory-for-ai)
