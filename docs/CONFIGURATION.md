# Configuration Reference

This page documents the configuration files that `memory-for-ai` reads or writes today.

## At a Glance

| Purpose | Path | Format | Notes |
|---|---|---|---|
| Global custom extension mapping | `$XDG_CONFIG_HOME/memory-for-ai/config.json` | JSON | Falls back to `~/.config/memory-for-ai/config.json` when `XDG_CONFIG_HOME` is unset. |
| Per-project custom extension mapping | `{repo_root}/.memory-for-ai.json` | JSON | Overrides conflicting global `extra_extensions` entries. |
| CLI-managed runtime settings | `${MFA_CACHE_DIR:-~/.cache/memory-for-ai}/_config.db` | SQLite | Written by `memory-for-ai config set/reset`. |
| UI settings | `${MFA_CACHE_DIR:-~/.cache/memory-for-ai}/config.json` | JSON | Stores `ui_enabled` and `ui_port`. |
| Daemon operation log | `${MFA_CACHE_DIR:-~/.cache/memory-for-ai}/logs/memory-for-ai-daemon.log` | Structured log | Durable daemon lifecycle, watcher/indexing, UI, resource, and error events. |
| Admission conflict log | `${MFA_CACHE_DIR:-~/.cache/memory-for-ai}/logs/daemon-conflicts.ndjson` | NDJSON | Exact-build, ABI, and canonical-cache conflicts. |
| Activation log | `${MFA_CACHE_DIR:-~/.cache/memory-for-ai}/logs/activation-events.ndjson` | NDJSON | Install/update/uninstall activation progress and outcomes. |

CBM resolves `MFA_CACHE_DIR` to a canonical per-account path before using any of these locations. The log directory and files are private to the account.

## 1. Custom File Extension Mapping

Two optional JSON files let you map additional file extensions to built-in languages.

### Global config

Default path:

```text
$XDG_CONFIG_HOME/memory-for-ai/config.json
```

Fallback when `XDG_CONFIG_HOME` is unset:

```text
~/.config/memory-for-ai/config.json
```

### Per-project config

Place this file in the repository root:

```text
.memory-for-ai.json
```

### Format

```json
{
  "extra_extensions": {
    ".blade.php": "php",
    ".mjs": "javascript",
    ".twig": "html"
  }
}
```

Notes:

- Extension keys must include the leading dot.
- Language names are case-insensitive.
- Unknown language names are skipped.
- Missing files are ignored.
- If the same extension appears in both files, the per-project file wins.

## 2. CLI-Managed Runtime Settings

The `config` subcommand stores runtime settings in a small SQLite database:

```text
${MFA_CACHE_DIR:-~/.cache/memory-for-ai}/_config.db
```

Inspect or change values with the CLI:

```bash
memory-for-ai config list
memory-for-ai config get auto_index
memory-for-ai config set auto_index true
memory-for-ai config set auto_index_limit 50000
memory-for-ai config set watcher_enabled false
memory-for-ai config reset auto_index
```

Current keys:

| Key | Default | Meaning |
|---|---|---|
| `auto_index` | `false` | Automatically index new projects when an MCP session starts. |
| `auto_index_limit` | `50000` | Maximum file count allowed for automatic indexing of a new project. |
| `auto_watch` | `true` | Register the session's project with the background git watcher on connect. Set `false` to keep a session from registering its project (the watcher still runs for other projects). |
| `watcher_enabled` | `true` | Master switch for the background watcher subsystem. Set `false` to stop the watcher from starting at all — no poll thread and no project registration. Reindex manually with `index_repository` when disabled. |

> **`watcher_enabled` vs `auto_watch`.** `watcher_enabled` controls whether the
> watcher *subsystem* starts at all (the background poll thread). `auto_watch` is
> narrower: it only controls whether a connecting session registers *its own*
> project with an already-running watcher. When `watcher_enabled=false`,
> `auto_watch` has no effect — there is no watcher to register with.
>
> They also differ in **when they are read**, which matters because the watcher
> lives in the background daemon, not in your MCP client:
>
> - `auto_watch` is consulted each time a session would register its project, so
>   a change applies to sessions that connect afterwards.
> - `watcher_enabled` is read **once, when the daemon starts**, because it decides
>   whether the watcher is built at all. The daemon is long-lived and outlives
>   individual MCP sessions, so **reconnecting your client is not enough** — retire
>   the daemon so the next one picks the new value up:
>
> ```bash
> memory-for-ai config set watcher_enabled false
> memory-for-ai daemon stop     # next session starts a daemon without the watcher
> memory-for-ai daemon status   # confirm
> ```
>
> Disabling the watcher does not disable anything else: the daemon still starts,
> `auto_index` still runs, and `index_repository` stays available for manual
> reindexing.

## 3. UI Settings

The optional built-in graph UI stores its settings in:

```text
${MFA_CACHE_DIR:-~/.cache/memory-for-ai}/config.json
```

Current format:

```json
{
  "ui_enabled": false,
  "ui_port": 9749
}
```

Notes:

- If a UI-enabled binary finds its verified external asset pack and no UI config file exists yet, the UI auto-enables on first run. Missing or invalid assets leave the MCP/daemon service available and keep the UI disabled.
- `MFA_CACHE_DIR` changes both the UI config location and the runtime settings database location.
- CBM resolves `MFA_CACHE_DIR` to one canonical per-account cache root. A process configured with a different root fails while any CBM session or command is active; close them before switching roots.

## 3b. Per-Project Scoped Sessions (`--scope`, `install --project`)

One binary still serves any number of repositories, but a **scoped session**
serves exactly one:

```bash
memory-for-ai --scope=/path/to/repo        # MCP server pinned to that repo
```

While pinned:

- Every tool argument that names a project (`project`, `base_project`,
  `target_project`) must name the session's derived project or be omitted;
  any other name is refused with an explicit scope error before the tool
  runs. The repository FOLDER name still resolves to the full derived
  project name (the `list_projects` alias rule), so an agent naturally
  spelling "my-repo" stays inside the scope.
- `list_projects` reports only the pinned project — other indexed projects
  are not even visible.
- Indexing is confined to paths inside the scoped repository (the scope path
  is also the session's `CBM_ALLOWED_ROOT` boundary).

`install --project` (run from the repository root) wires this up end to end:
it installs/refreshes the shared binary WITHOUT touching global agent
configs, writes a project-local `.mcp.json` entry named
`memory-for-ai-<repo-directory>` (invalid characters collapse to `-`; pass
`--name=<suffix>` to override) whose command is the installed binary with
`--scope=<repository>`, and indexes the repository immediately. The shell and
PowerShell installers forward the same flag:

```bash
curl -fsSL https://raw.githubusercontent.com/LonelyTraderBay/memory-for-ai/main/install.sh | bash -s -- --project
```

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1 --project
```

Result: an agent opened in that repository sees one MCP server named after
it, serving only its graph; an agent opened in a different repository sees
its own server. `uninstall` removes the shared binary and global config as
before; a project-local `.mcp.json` entry is the repository's own file and
is not touched by it.

## 4. Environment Variables

These environment variables affect runtime behavior.

### General

| Variable | Default | Description |
|---|---|---|
| `CBM_ALLOWED_ROOT` | *(unset)* | Confine `index_repository` to paths within this directory. When set, a `repo_path` that resolves (after symlink / `..` resolution) outside this root is refused, and the same check now applies to the graph UI's `POST /api/index` route rather than only to the MCP tool. Unset imposes no *containment* restriction — but see the always-on limits below, which apply whether or not this is set. Useful when the server may be driven by an untrusted caller, e.g. agentic or multi-tenant deployments. |
| `MFA_CACHE_DIR` | `~/.cache/memory-for-ai` | Override the cache directory used for indexes, `_config.db`, and UI `config.json`. |
| `CBM_DIAGNOSTICS` | `false` | Enable periodic `snapshot.json` and retained `trajectory.ndjson` below a fresh owner-private directory in the system temp directory. The daemon records the randomized paths in the `diagnostics.start` discovery record (a single JSON line) in `${MFA_CACHE_DIR}/logs/memory-for-ai-daemon.log`; that one record is emitted even when `CBM_LOG_LEVEL` suppresses ordinary logging, so the paths always remain discoverable. |
| `CBM_DOWNLOAD_URL` | GitHub releases | Override the update download URL. |
| `CBM_LOG_LEVEL` | `info` | Set the log level to `debug`, `info`, `warn`, `error`, or `none` (or `0`-`4`). Thin-frontend messages use that session's stderr; detached daemon events use `${MFA_CACHE_DIR}/logs/memory-for-ai-daemon.log`. |
| `CBM_LOG_FORMAT` | `text` | Log output format: `text` or `json`. Unrecognized values are ignored. |
| `MFA_RUNTIME_DIR` | `%LOCALAPPDATA%` (Windows), `/private/tmp` (macOS), `/tmp` (other) | Parent directory for the daemon/CLI rendezvous directory, which CBM creates inside it as `memory-for-ai-daemon-<uid>` (`memory-for-ai-daemon-<key>` on Windows). Set it when the default ancestry cannot pass the private-directory check — see below. `MFA_CACHE_DIR` does **not** move the rendezvous. |

### Indexing pipeline

| Variable | Default | Description |
|---|---|---|
| `CBM_WORKERS` | auto-detected | Override the indexing worker count. |
| `CBM_MEM_BUDGET_MB` | auto (fraction of system RAM) | Override the indexing memory budget in MiB. Oversized requests clamp to total RAM; invalid values fall back to the auto-detected budget. |
| `CBM_MAX_FILE_BYTES` | `536870912` (512 MiB) | Per-file read cap in bytes. Files over the cap are reported as oversized and skipped, never silently dropped or read unbounded. Non-positive or unparseable values fall back to the default. |
| `CBM_DISABLE_LSP_CROSS` | *(unset)* | Set to `1` to skip the LSP cross-pass phase — the most expensive pipeline phase. Also the documented workaround if a language resolver misbehaves on your codebase (see `docs/RUNTIME_TRACE_MODEL.md`). |
| `CBM_SEMANTIC_ENABLED` | *(unset)* | Set to `1` to opt in to the semantic embedding edges pass (nomic-embed-code vectors, shipped in the binary). Off by default. |
| `CBM_SEMANTIC_THRESHOLD` | `0.75` | Similarity threshold for semantic edges, in `(0, 1]`. Values outside the range or non-numeric are ignored and the default applies. |
| `CBM_DUMP_VERIFY_MIN_RATIO` | `0.5` | Minimum verification ratio for store dump verification, in `[0, 1]`. Set `0` to disable the check. |
| `CBM_SQLITE_MMAP_SIZE` | `67108864` (64 MiB) | `PRAGMA mmap_size` override (bytes) for the graph store's SQLite connection. |

### Index supervision and hooks

| Variable | Default | Description |
|---|---|---|
| `CBM_INDEX_SUPERVISOR` | *(enabled)* | Set to `0` to request in-process indexing instead of a supervised worker. Under the mandatory coordination daemon this is a fail-closed refusal — the supervisor is a safety boundary, not a preference. |
| `CBM_INDEX_WORKER_TIMEOUT_S` | `900` (15 min) | Quiet-timeout for a supervised indexing worker, in seconds. This is a **no-progress** timeout: any new log line (per-batch progress, pass boundaries) resets it, so a large repository that keeps making progress is never killed. A genuinely stuck file emits nothing and is killed and reported as a hang. |
| `CBM_INDEX_MAX_RESTARTS` | `100` | Maximum restarts of a failed indexing job by the daemon application. Invalid values fall back to the default. |
| `CBM_HOOK_DEADLINE_MS` | `2000` | In-process budget (ms) for the agent `PreToolUse` hook augmenter. Values below the internal minimum (50 ms) are clamped. |
| `CBM_HOOK_TIMEOUT_LOG` | *(built-in path)* | Override the path of the hook timeout crumb log (tests and power users). |

### LSP debugging

| Variable | Default | Description |
|---|---|---|
| `CBM_LSP_DISABLED` | *(unset)* | Set to `1` to skip the Hybrid-LSP resolvers entirely — a diagnostic/benchmarking knob that isolates tree-sitter-only behavior. |
| `CBM_LSP_DEBUG` | *(unset)* | Enable debug mode in the language resolvers (shared across all language LSPs). |
| `CBM_LSP_KOTLIN_AST` | *(unset)* | Set to `1` to enable the Kotlin debug AST dumper. |

### Memory diagnostics

All of these are off by default and exist for troubleshooting memory behaviour;
they add instrumentation, not behavior changes.

| Variable | Default | Description |
|---|---|---|
| `CBM_MEM_CENSUS` | *(unset)* | Set to `1` to log a per-phase allocation census readable from the daemon log. |
| `CBM_MEM_PHASES` | *(unset)* | Set to `1` to include per-phase memory records in diagnostics output. |
| `CBM_MEM_STATS` | *(unset)* | Set to `1` to record allocator statistics that committed-byte counters cannot distinguish. |
| `CBM_MEM_STATS_OUT` | *(unset)* | File path for the mimalloc allocator stats dump written at session exit. |
| `CBM_MEM_PROFILE` | *(unset)* | Set to `1` to enable fixed-capacity allocation profiling tables. |
| `CBM_MEM_PROFILE_MIN` | *(unset)* | Minimum allocation size (bytes) recorded by `CBM_MEM_PROFILE`. |
| `CBM_PROFILE` | *(unset)* | Any non-empty value other than `0` marks a profiling run (e.g. keeps the supervised worker log). |

### Internal and test-only variables

`CBM_INDEX_MARKER_FILE`, `CBM_INDEX_QUARANTINE_FILE`, and
`CBM_INDEX_SINGLE_THREAD` are set **by the index supervisor into its worker**
—they are plumbing between the two processes, not user configuration. The
`CBM_TEST_*` family and `CBM_MI_THREAD_DONE` exist for the test harness and
demonstration builds; release binaries must not honour them (enforced by the
binary composition gate).

### Relocating the daemon rendezvous directory

Before it is used, the rendezvous directory and every ancestor of it are checked:
each ancestor must be owned by you or by root, must not be world-writable (unless
it is the standard root-owned sticky directory such as `/tmp`), and must carry no
allow-ACL — on Windows, no ACE granting mutation rights to another identity. The
rendezvous directory itself is then forced to owner-only (`0700`, no extended ACL
/ an owner-only DACL).

That ancestry is not always acceptable in the default location. A Windows profile
that has acquired a capability-SID ACE with `WRITE_DAC` / `WRITE_OWNER` / `DELETE`
on `%LOCALAPPDATA%` — something an installed packaged app can add — fails the walk,
and so can an unusual `/tmp` or home directory on POSIX. When that happens *every*
command fails, `config list` included, so the settings surface cannot be reached
either:

```text
memory-for-ai: secure daemon endpoint could not be created
```

`MFA_RUNTIME_DIR` points the rendezvous at an ancestry you choose:

```bash
export MFA_RUNTIME_DIR="$HOME/cbm-runtime"   # any directory you own
```

```powershell
$env:MFA_RUNTIME_DIR = "D:\cbm-runtime"
```

The check is not relaxed for the directory you name: it goes through exactly the
same validation as the default, and a value that fails it is refused rather than
silently ignored. Because the rendezvous is how sessions find each other, every
process that should share one daemon must see the same value — set it in the
environment of your MCP client and your shell alike, or a CLI invocation without
it will coordinate through the default location instead.

Environment used by daemon-owned components—such as diagnostics, daemon logging, and process-wide indexing resource limits—is captured from the first daemon-backed session that starts the daemon. Later sessions join the existing process and cannot replace those values. To change them, close every daemon-backed session, update the relevant agent configurations consistently, and restart a session. `CBM_ALLOWED_ROOT` remains session-specific, a conflicting `MFA_CACHE_DIR` is rejected, and one-shot CLI commands use their own current environment without starting the daemon.


### Roots that are always refused

Independently of `CBM_ALLOWED_ROOT`, some directories are refused as an indexing
root because they are too broad or too sensitive to index as a unit:

- a filesystem root, a Windows drive root, or a UNC share root;
- a top-level system tree — `/etc`, `/var`, `/usr`, `/home`, `/Users`, and on
  Windows `C:\Windows`, `C:\Users`, `C:\ProgramData`, `C:\Program Files`;
- your home directory itself (directories *below* it are fine);
- a credential directory at any depth — `.ssh`, `.aws`, `.gnupg`, `.kube`,
  `.docker`, `.netrc`, `.git-credentials`, `.password-store`, macOS `Keychains`.

Two limits are worth stating plainly. This constrains *scope*, not
*sensitivity*: inside a root that is allowed, every file the process can read may
be indexed and later returned. And the credential list is a denylist, so it
raises the cost of a mistake rather than closing the class — a directory it does
not name is permitted.

## 5. Agent and Editor Integration Files

The `install` command can also write MCP entries and instruction blocks into agent/editor config files such as Claude Code, Codex, Gemini, VS Code, Cursor, Zed, and others.

Those target paths vary by tool and platform, so the easiest way to inspect the exact files for your machine is:

```bash
memory-for-ai install --dry-run
```

That prints the specific config files the installer would modify without writing anything.
