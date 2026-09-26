# Installing memory-for-ai on Windows x64

Only native Windows x64 is supported. Linux, macOS, Windows ARM64, x64
emulation on ARM64 and WSL are outside the product support matrix. Historical
releases may contain other platforms; current releases contain only
`memory-for-ai-windows-amd64.zip` and its MCPB bundle.

## Preflight: check your machine first

- Windows x64 with PowerShell and Git available on PATH.
- `powershell -Command "$PSVersionTable.PSVersion"` and `git --version` work.
- Allow roughly 2 GB free disk; large repositories require additional cache space.
- Use an installation/cache directory owned by your account. ACL checks fail
  closed on directories another account can modify.

## Standard install

```powershell
Invoke-WebRequest -Uri https://raw.githubusercontent.com/LonelyTraderBay/memory-for-ai/main/install.ps1 -OutFile install.ps1
Unblock-File .\install.ps1
.\install.ps1
```

The installer downloads the x64 archive, verifies SHA-256 before extracting,
and configures detected clients. Restart the client afterward. It does not
require WSL, Docker, Node.js or Go for the native binary. Git supports watcher
freshness; `search_code` uses PowerShell at runtime.

## Per-project install

```powershell
.\install.ps1 --project
```

Creates a project-local MCP entry scoped to the current repository. Optional
`--name=<name>` selects the server name; `--skip-config` installs the binary
without changing client configuration. See [configuration](CONFIGURATION.md)
for scope, cache locations and daemon rendezvous ownership.

## Package managers

These wrappers also target Windows x64 only:

```powershell
npm install -g memory-for-ai-mcp
pip install memory-for-ai
go install github.com/LonelyTraderBay/memory-for-ai/pkg/go/cmd/memory-for-ai@latest
```

Scoop, Chocolatey and winget definitions remain under `pkg/`; their publication
is separate from building the release. Homebrew, AUR and Nix are not current
supported installation routes. Wrappers verify the runtime set before publishing
it and retain the current installation if validation fails.

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

Use an absolute Windows executable path in the client's MCP configuration:

```json
{
  "mcpServers": {
    "memory-for-ai": {
      "command": "C:/Users/you/AppData/Local/Programs/memory-for-ai/memory-for-ai.exe",
      "args": []
    }
  }
}
```

For project isolation, add `--scope=C:/path/to/repository` to `args`. Restart
the client; the server exposes 23 MCP tools. Use `index_status` and
`check_index_coverage` before trusting graph completeness or freshness.

## Update

Re-run `install.ps1`, or update through the package manager used to install.
The Windows installer retires the prior executable before publishing the new
one; a running process cannot overwrite its own image. Existing indexes are
preserved. On failure, inspect the error and retained backup before retrying.

## Uninstall

Run `memory-for-ai uninstall` to remove managed client configuration, then
remove the native installation through its original installer/package manager.
Do not delete the cache unless you explicitly want to discard saved indexes.

## Build from source

Use native Windows x64 with MSYS2 CLANG64, Node.js for graph UI, and the Go
version in `pkg/go/go.mod` for package-wrapper tests:

```powershell
./scripts/setup-windows-toolchain.ps1
./scripts/verify-windows.ps1
```

The verification command runs native tests, UI checks, package wrappers,
Windows product guards and lint. Use `-Suites "mcp,edit,edit_integration"` for
an explicitly limited iteration. `-NoSanitizer` is functional-only verification;
ASan/UBSan are enabled by default. Full-product TSan/MSan are outside this
matrix; see [Windows x64](WINDOWS-X64.md) for measured results and limits.

## Verifying release artifacts

Verify the downloaded archive against `checksums.txt` using `Get-FileHash
-Algorithm SHA256`. The release pipeline retains candidate selection evidence,
provenance and signatures. See [security policy](../SECURITY.md) for verification
and antivirus handling. A local build or passing unit tests do not certify a
published artifact.

## Install troubleshooting

| Problem | Check |
|---|---|
| Unsupported platform | Native Windows x64 is required; ARM64/emulation/WSL are excluded. |
| `search_code` cannot complete | Ensure Windows PowerShell is on PATH, then restart the client. |
| Secure daemon endpoint refused | Use an owned runtime directory; see `MFA_RUNTIME_DIR` in CONFIGURATION.md. |
| Server missing in the client | Restart the client and verify its absolute executable path. |
| Antivirus quarantine | Inspect the exact artifact and release evidence; do not disable protection. |
