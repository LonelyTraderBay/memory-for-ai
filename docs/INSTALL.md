# Installation reference

How to get memory-for-ai running on your machine, inside one specific project, or in CI — and how to verify, update, and remove it.

## Choose your path

| Situation | Path |
|---|---|
| My machine, many repos, configure all my coding agents at once | [Standard install](#standard-install) (one line) |
| One repo deserves its own fenced MCP server named after it | [Per-project install](#per-project-install) |
| Headless container / CI runner | [Standard install](#standard-install) with `--skip-config`, then the [CI settings](#containers-and-ci); or a [package manager](#package-managers) |
| I manage packages via npm / pip / brew / scoop / winget / choco / AUR / Nix / Go | [Package managers](#package-managers) |
| I want to inspect exactly what will be written before anything is | `memory-for-ai install --dry-run` |
| I can't run installers at all | [Manual MCP configuration](#manual-mcp-configuration) |

## Standard install

Downloads the verified release archive for your platform, checks its SHA-256 against the release `checksums.txt`, installs the binary (default `~/.local/bin`), configures every detected coding agent (see [What install writes](#what-install-writes)), strips macOS quarantine and ad-hoc signs the binary.

**macOS / Linux:**

```bash
curl -fsSL https://raw.githubusercontent.com/LonelyTraderBay/memory-for-ai/main/install.sh | bash
```

**Windows (PowerShell):**

```powershell
Invoke-WebRequest -Uri https://raw.githubusercontent.com/LonelyTraderBay/memory-for-ai/main/install.ps1 -OutFile install.ps1
Unblock-File .\install.ps1     # removes Mark-of-the-Web
.\install.ps1
```

If script execution is blocked: `Set-ExecutionPolicy -Scope Process Bypass`, or `PowerShell -ExecutionPolicy Bypass -File .\install.ps1`.

**Options** (both scripts):

| Flag | Meaning |
|---|---|
| `--dir=<path>` | Install directory (default `~/.local/bin`; make sure it is on `PATH`) |
| `--clients=<list>` | Configure only the comma-separated clients (e.g. `--clients=claude,cursor`) |
| `--skip-config` | Install the binary only — no agent configuration |
| `--project` | Per-project mode — see below |
| `--name=<name>` | Override the derived server name (per-project mode) |

After install: **restart your coding agent** and say **"Index this project"**. First index of an average repo finishes in seconds; the Linux kernel takes ~3 minutes.

Prefer manual download? Grab `memory-for-ai-<os>-<arch>.tar.gz` (`.zip` on Windows) from the [latest release](https://github.com/LonelyTraderBay/memory-for-ai/releases/latest), extract, run the bundled `install.sh` / `install.ps1`. Linux `-portable` archives are fully static builds.

## Per-project install

For when a repository should carry its own memory: an MCP server named after the repo, an index no other repo can see, zero global configuration changes. One command from the repository root:

```bash
curl -fsSL https://raw.githubusercontent.com/LonelyTraderBay/memory-for-ai/main/install.sh | bash -s -- --project
```

```powershell
# Windows, from the repo root, with install.ps1 downloaded
powershell -ExecutionPolicy Bypass -File .\install.ps1 --project
```

What it does, exactly:

1. Installs or refreshes the **shared binary** (one binary still serves all repos) — **without touching any global agent config**.
2. Writes a repo-local `.mcp.json` entry named `memory-for-ai-<repo-directory>` (invalid characters collapse to `-`; `--name=` overrides) whose command is the installed binary run with `--scope=<repository>`.
3. Indexes the repository immediately.

Guarantees while pinned with `--scope`: any tool argument naming a different project is refused before the tool runs; `list_projects` shows only the pinned project; indexing is confined to paths inside the repository. An agent opened in a different repo sees its own server. `uninstall` removes the shared binary and global config but never touches a repo's own `.mcp.json` (the repository owns that file).

**Which clients see the per-project server.** The entry lands in the standard repo-root `.mcp.json`, so every client that documents project-scope `.mcp.json` support picks it up — Claude Code and VS Code among them. Clients that keep project MCP config under their own filename (Cursor: `.cursor/mcp.json`; Gemini CLI: `.gemini/settings.json`) or read only global config will not adopt the entry on their own. For those, either run the regular global `install` as well (it configures all detected clients; the one shared binary then serves both the global unscooped server and this repo's scoped one), or copy the entry into that client's own project config — `install --project --dry-run` previews exactly what would be written.

Semantics and test coverage for scoped sessions: [CONFIGURATION.md §3b](CONFIGURATION.md#3b-per-project-scoped-sessions---scope-install---project). Agent-facing usage: [AGENT_GUIDE.md §9](AGENT_GUIDE.md#9-project-isolation--team-sharing).

## Package managers

```bash
npm install -g memory-for-ai        # npm — downloads the verified runtime set at install time
pip install memory-for-ai           # PyPI
brew install memory-for-ai          # Homebrew
scoop install memory-for-ai         # Windows Scoop
winget install memory-for-ai        # Windows Winget
choco install memory-for-ai         # Chocolatey
yay -S codebase-memory-mcp-bin     # Arch AUR — community package under the previous product name (or paru)
go install github.com/LonelyTraderBay/memory-for-ai/pkg/go/cmd/memory-for-ai@latest
```

The npm/PyPI/Go wrappers verify and publish a coherent cached runtime set (executable + authenticated assets) without disturbing a running native install, then the usual `memory-for-ai install` configures agents. Update with the same package manager (`npm install -g memory-for-ai@latest`, `pip install -U memory-for-ai`, …).

Nix (flake) — run without installing:

```bash
nix run github:LonelyTraderBay/memory-for-ai                     # standard server
nix run github:LonelyTraderBay/memory-for-ai#memory-for-ai-ui -- --ui=true --port=9749
```

Packages: `default` (standard server), `memory-for-ai-ui` (graph UI embedded), `graph-ui` (frontend assets only). Note: launched by hand, the server exits when `stdin` closes (normal MCP behavior) — keep stdin open while testing the UI: `sleep infinity | memory-for-ai --ui=true`.

## What install writes

`install` configures **45 client surfaces** — 39 detected automatically and 6 conditional/explicit ("conditional" = written only when the documented platform or an explicit existing config path proves the target is active). It writes:

- a documented **MCP server entry** in each detected client's native config (`~/.claude.json`, `$CODEX_HOME/config.toml`, `.gemini/settings.json`, `~/.codeium/windsurf/mcp_config.json`, VS Code `mcp.json`, Cursor `.cursor/mcp.json`, and so on);
- **durable context** where the client documents a safe contract: skills, instruction files (`AGENTS.md` / `GEMINI.md` / `QWEN.md` / rules), and fail-open lifecycle hooks (`SessionStart`, `SubagentStart`, post-`Read` coverage context) — context-only, never gating or denying;
- **three tiered graph profiles** (Scout / Verify / Auditor) for clients with custom-agent formats, each checking its evidence with `check_index_coverage` before making claims.

It never enables experimental feature flags, plugins, YOLO modes, global permission bypasses, or third-party instruction trust. Updates migrate only byte-identical prior definitions and never overwrite user-modified agents.

Preview the exact writes for **your** machine before running anything:

```bash
memory-for-ai install --dry-run
```

The full per-client matrix (which surface gets MCP entry / skills / hooks / agents, and why some hooks are deliberately withheld per client) is defined in the installer source (`src/cli/`); `--dry-run` output is authoritative for your machine.

<details>
<summary>All 45 configured surfaces</summary>

Claude Code, Codex CLI, Gemini CLI, Zed, OpenCode, Antigravity, Aider, KiloCode, VS Code, Cursor, Windsurf, Augment / Auggie, OpenClaw, Kiro, Junie, Hermes, OpenHands, Cline, Warp, Qwen Code, GitHub Copilot CLI, Factory Droid, Crush, Goose, Mistral Vibe, Grok Build, Qoder CLI, Kimi Code CLI, GitLab Duo CLI, Rovo Dev CLI, Amp, Devin CLI / Local, Tabnine, Continue / cn (conditional), Visual Studio (conditional, Windows), TRAE (conditional), Roo Code (conditional), Amazon Q Developer IDE, CodeBuddy Code CLI, IBM Bob IDE (conditional), IBM Bob Shell, Pochi, Pi, Sourcegraph Cody (explicit opt-in), Oh My Pi (omp).
</details>

## Manual MCP configuration

If you prefer not to use `install` at all:

```json
{
  "mcpServers": {
    "memory-for-ai": {
      "command": "/absolute/path/to/memory-for-ai",
      "args": []
    }
  }
}
```

Add to `~/.claude.json` (user scope) or the project `.mcp.json`; for a per-project pinned server, append `"--scope=<repo>"` to `args`. Restart the agent and verify with `/mcp` — you should see `memory-for-ai` with 18 tools. Quick transport check: `echo '{}' | /path/to/binary` must print JSON.

## Containers and CI

```bash
curl -fsSL https://raw.githubusercontent.com/LonelyTraderBay/memory-for-ai/main/install.sh \
  | bash -s -- --skip-config --dir=/usr/local/bin
memory-for-ai config set auto_index true
memory-for-ai config set auto_index_limit 20000
```

Environment that matters in constrained runtimes — full table in [CONFIGURATION.md](CONFIGURATION.md#4-environment-variables):

| Variable | Why in CI |
|---|---|
| `CBM_WORKERS=<n>` | Container CPU quota is invisible to `sysconf`; set the worker count to the cgroup limit. |
| `CBM_MEM_BUDGET_MB=<n>` | Cap the indexing budget below the container memory limit. |
| `MFA_CACHE_DIR=<path>` | Keep indexes on a cache volume across CI steps. One canonical root per account. |
| `CBM_ALLOWED_ROOT=<dir>` | Confine `index_repository` when the server may be driven by untrusted callers. |

Agent-facing CI notes (auto-index, watcher suppression, always-refused roots): [AGENT_GUIDE.md §10](AGENT_GUIDE.md#10-ci-and-containers).

## Update

Updates run **from the install script, not from inside the running binary** (`memory-for-ai update` prints the exact command). The script beside the binary is the updater — re-running it *is* the update: it stops the daemon, retires the running binary, installs the new one, and cleans up.

```bash
bash "<install-dir>/install.sh"          # macOS / Linux
powershell -ExecutionPolicy Bypass -File "<install-dir>\install.ps1"   # Windows
```

Why: on Windows a running executable cannot replace its own image; on macOS/Linux it is a deliberate choice — the binary makes **no network request of its own accord** (no background version checks, nothing phones home). You learn about releases from the install script, your package manager, or GitHub. npm/PyPI/Go installs update through their package manager on every platform.

## Uninstall

```bash
memory-for-ai uninstall
```

Removes owned agent config entries, skills, hooks, instructions, and the binary. Indexed graphs are listed and deleted only after confirmation. The install script beside the binary is **reported, not deleted** — it may be your own copy, a symlink, or package-manager owned; the uninstaller prints its path and the `rm` command instead of deleting a file it cannot prove it owns. A repo-local `.mcp.json` from `install --project` belongs to that repository and is never touched.

## Build from source

Prerequisites: C and C++ compilers (gcc/clang), zlib, git. Then:

```bash
git clone https://github.com/LonelyTraderBay/memory-for-ai.git
cd memory-for-ai
scripts/build.sh --with-ui        # shipped composition (graph UI embedded)
scripts/build.sh                  # without UI (development)
# → build/c/memory-for-ai  (.exe on Windows)
```

Test suite (the same entry CI gates run):

```bash
scripts/test.sh                   # full lane: sanitizer build + all suites + guards
scripts/test.sh --suites <name>   # one suite, incremental
build/c/test-runner --list-suites
```

Artifact-flow check (build candidates, package, extract, smoke): `scripts/ci/smoke-artifact.sh <linux|darwin|windows> <amd64|arm64>`.

## Verifying release artifacts

Every release ships `checksums.txt` (SHA-256, verified by both install scripts before extraction), SLSA Level 3 build provenance, and Sigstore cosign bundles:

```bash
gh attestation verify ./memory-for-ai \
  --repo LonelyTraderBay/memory-for-ai \
  --signer-workflow LonelyTraderBay/memory-for-ai/.github/workflows/_build.yml
```

All executable candidates are VirusTotal-scanned before release; the selected bytes ship unchanged and release notes link the exact verdicts. Policy, evidence TSVs, and the single documented Microsoft `!ml` false-positive tolerance: [SECURITY.md](SECURITY.md). Windows SmartScreen may warn on unsigned software — "More info" → "Run anyway" after checking the checksum.

## Install troubleshooting

| Problem | Fix |
|---|---|
| `curl \| bash` blocked by policy | Download `install.sh`, inspect, run it locally (`bash install.sh`) |
| Binary not on `PATH` | `export PATH="$HOME/.local/bin:$PATH"` (or the `--dir` you chose) |
| Antivirus quarantines the binary | Known Defender `Wacatac.B!ml` false positive — evidence & verification: [SECURITY.md](SECURITY.md#antivirus-false-positives) |
| macOS "cannot be opened" | Handled automatically by `install`; manually: `xattr -d com.apple.quarantine <binary>` |
| Agent doesn't show the server after install | Restart the agent; check `/mcp`; confirm the config path is absolute; `echo '{}' \| <binary>` should print JSON |
| Hook trust prompts (Codex) | Review/trust via `/hooks`; changing a hook definition changes its trust hash |
| "secure daemon endpoint could not be created" | Set `MFA_RUNTIME_DIR` to a directory you own — [CONFIGURATION.md](CONFIGURATION.md#relocating-the-daemon-rendezvous-directory) |
