# So sánh memory-for-ai với Serena (oraios/serena)

> Phân tích ngày 2026-09-16, dựa trên [README Serena](https://github.com/oraios/serena), [tài liệu tools chính thức của Serena](https://oraios.github.io/serena/01-about/035_tools.html) và [docs/AGENT_GUIDE.md](AGENT_GUIDE.md) của dự án này.

## 1. Kết luận nhanh

Hai dự án **cùng giải một bài toán** — cung cấp cho AI coding agent khả năng hiểu codebase ở mức symbol/cấu trúc qua MCP, thay vì đọc file từng dòng — nhưng **khác nhau căn bản về kiến trúc và phạm vi**:

- **Serena = "IDE-as-MCP"**: chạy language server (LSP) thật, truy vấn symbol **on-demand**, mạnh về **sửa code** (rename, move, replace symbol body, insert, safe delete, debugging).
- **memory-for-ai = "Knowledge-graph-as-MCP"**: index toàn bộ repo **một lần thành graph bền vững** trong SQLite, mọi câu hỏi là truy vấn graph mili-giây, mạnh về **phân tích cấu trúc** (call chain bắc cầu, blast radius, architecture, dead code, Cypher tự do).

Hai tool **bổ sung cho nhau hơn là trùng lặp**: một agent có thể dùng memory-for-ai để định vị + hiểu kiến trúc, rồi dùng Serena để refactor chính xác ở mức symbol.

## 2. Bảng so sánh tổng quan

| Khía cạnh | Serena | memory-for-ai |
|---|---|---|
| Định vị | "The IDE for your coding agent" | Persistent codebase knowledge graph qua MCP |
| Công nghệ lõi | Language server (LSP) thật qua SolidLSP; hoặc plugin JetBrains (trả phí) | Tree-sitter (162 grammar vendored) + Hybrid-LSP type resolution nhúng bằng C — không cần language server process |
| Mô hình dữ liệu | Truy vấn LSP on-demand, không lưu graph | Graph node/edge bền vững trong SQLite, watcher tự re-index |
| Số ngôn ngữ | ~40+ (phụ thuộc từng language server cài bên ngoài) | 162 (tất cả compile sẵn trong binary) |
| Triển khai | Python, cài qua `uv tool install serena-agent` | Một native executable C duy nhất, zero runtime dependency |
| Hướng sử dụng chính | **Đọc + SỬA** (edit/refactor/debug) | **Đọc + PHÂN TÍCH** (18 tool, gần như read-only) |
| Truy vấn tự do | Không có | Cypher subset read-only (`query_graph`) |
| "Memory" | File ghi chú markdown per-project (`write_memory`…) | Chính graph bền vững + ADR (`manage_adr`) + runtime traces |
| Cross-service | Không | `CROSS_*` edges, `trace_path(mode="cross_service")` |
| Giấy phép | GPL-3.0-or-later (SolidLSP: MIT) | MIT toàn bộ |
| LLM bên trong | Không | Không — client là intelligence layer |

## 3. Mapping chi tiết: 18 tool của memory-for-ai ↔ tool Serena

### 3.1. Indexing & quản lý project

| memory-for-ai | Serena tương đương | Nhận xét |
|---|---|---|
| `index_repository` | `activate_project` (+ LSP index ngầm) | Serena index "lười" qua language server khi cần; memory-for-ai index chủ động, đầy đủ, có 3 mode (`full`/`moderate`/`fast`) + `cross-repo-intelligence` |
| `list_projects` | `get_current_config`, `remove_project` | Tương đương về quản lý danh sách project |
| `index_status` | `get_current_config` (một phần) | memory-for-ai trả thêm node/edge counts, git context, coverage report |
| `delete_project` | `remove_project` | Tương đương |
| `check_index_coverage` | — (không có) | **Độc nhất**: oracle kiểm chứng độ phủ index trước khi agent kết luận "không tồn tại" |

### 3.2. Discovery & đọc code

| memory-for-ai | Serena tương đương | Nhận xét |
|---|---|---|
| `search_graph` | `find_symbol`, `get_symbols_overview` | Serena tìm symbol qua LSP; memory-for-ai có 3 mode: BM25 full-text, regex name, và **semantic vector search** — Serena không có semantic search |
| `get_code_snippet` | `find_symbol` (kèm body), `read_file` | Tương đương; memory-for-ai dùng qualified name từ graph |
| `search_code` | `search_for_pattern` | Cả hai là regex/text search; memory-for-ai xếp hạng kết quả theo độ quan trọng cấu trúc (definitions trước, tests sau) |
| `get_graph_schema` | — | Serena không có schema vì không có graph |
| `get_architecture` | `onboarding` (một phần) | Serena `onboarding` tạo tài liệu định hướng; memory-for-ai trả cấu trúc trực tiếp: languages, packages, entry points, routes, hotspots, **layers, clusters (community detection), cycles** |
| `get_code_actions` | `get_diagnostics_for_file`, `get_diagnostics_for_symbol` | Serena diagnostics đến từ LSP thật (linter/type errors); memory-for-ai gợi ý từ bằng chứng coverage/complexity/test/dependency-security. Cả hai đều read-only |

### 3.3. Phân tích quan hệ & tác động

| memory-for-ai | Serena tương đương | Nhận xét |
|---|---|---|
| `trace_path` | `find_referencing_symbols`, `jet_brains_type_hierarchy` | **Khác biệt lớn nhất**: Serena chỉ tìm reference trực tiếp (1 hop); memory-for-ai duyệt **call chain bắc cầu** đến depth 5 với tổng caller/callee chính xác, kèm data-flow mode và risk labels |
| `detect_changes` | — (không có) | Blast radius của git diff — Serena không có khái niệm tương đương |
| `query_graph` | — (không có) | Cypher read-only: dead code, complexity hotspots, dependency graph, security-evidence — không thể biểu diễn bằng LSP calls |
| `compare_graphs` | — (không có) | Diff hai snapshot đã index (refactor audit, vendor bump) |

### 3.4. Memory & runtime evidence

| memory-for-ai | Serena tương đương | Nhận xét |
|---|---|---|
| `manage_adr` | `write_memory` / `read_memory` / `edit_memory` / `list_memories` / `delete_memory` / `rename_memory` | Serena: file markdown đơn giản. memory-for-ai: ADR có cấu trúc section, byte-preserving update, safe retry |
| `ingest_traces` | — (không có) | Nhận runtime call traces vào sidecar cô lập |
| `get_runtime_traces` | — (không có) | Đối chiếu "code nói gì" (static graph) vs "runtime chạy gì" (traces) |

### 3.5. Tool Serena mà memory-for-ai KHÔNG có

| Nhóm | Tool Serena | Ghi chú |
|---|---|---|
| **Symbolic editing** | `replace_symbol_body`, `insert_after_symbol`, `insert_before_symbol`, `safe_delete_symbol` | Sửa code ở mức symbol, an toàn và tiết kiệm token hơn search-and-replace |
| **Refactoring** | `rename_symbol`, `jet_brains_move`, `jet_brains_rename`, `jet_brains_inline_symbol`, `jet_brains_safe_delete` | Rename/move/inline xuyên file qua LSP/JetBrains |
| **Debugging** | `jet_brains_debug` [BETA] | Breakpoint, inspect biến, REPL debug — chỉ qua plugin JetBrains trả phí |
| **File utilities** | `read_file`, `create_text_file`, `list_dir`, `find_file`, `replace_content`, `replace_in_files`, `insert_at_line`, `replace_lines`, `delete_lines` | Bộ tool file cơ bản (thường bị tắt khi chạy trong Claude Code/Codex vì trùng với built-in) |
| **Shell** | `execute_shell_command` | Chạy build/test/lint |
| **Workflow** | `onboarding`, `initial_instructions`, `serena_info`, `open_dashboard` | Onboarding + dashboard web |
| **Query cross-project** | `query_project`, `list_queryable_projects` | Truy vấn project khác — gần với ý tưởng `cross-repo-intelligence` của memory-for-ai nhưng thực thi theo cách "gọi tool read-only lên project kia" |

## 4. Phân tích điểm mạnh tương đối

### Serena mạnh hơn ở

1. **Edit/refactor an toàn**: rename/move/replace symbol xuyên codebase — memory-for-ai hoàn toàn read-only với source.
2. **Diagnostics thật từ compiler/linter** qua LSP (type errors, unused…).
3. **Debug tương tác** (JetBrains plugin).
4. **Không cần bước index** — mở project là dùng được (đổi lại phụ thuộc language server per-language).

### memory-for-ai mạnh hơn ở

1. **Truy vấn quan hệ bắc cầu**: call chain depth 5, blast radius của diff, cross-service impact — LSP không trả lời được ở bất kỳ giá nào (thông tin không nằm trong một file).
2. **Truy vấn tự do bằng Cypher**: dead code, complexity, cycles, community detection.
3. **Phủ ngôn ngữ rộng gấp ~4 lần** (162 vs ~40) và không cần cài language server.
4. **Bền vững xuyên session**: graph sống trong SQLite, watcher tự cập nhật; Serena phải re-query LSP mỗi session.
5. **Triển khai đơn giản**: một binary native, no Python/uv/Docker; license MIT (Serena GPL-3.0 — rào cản cho tích hợp thương mại).
6. **Semantic search** (vector cosine) và **runtime traces** — Serena không có.

## 5. Định vị cạnh tranh

- Serena **không phải đối thủ trực tiếp** theo nghĩa "thay thế": overlap chủ yếu ở 3–4 tool discovery (`find_symbol` ↔ `search_graph`, `find_referencing_symbols` ↔ `trace_path` 1-hop).
- Đối thủ trực tiếp của memory-for-ai theo trục "graph memory" là các dự án **code-graph / codebase-RAG** (ví dụ các MCP server dựa trên Neo4j/tree-sitter indexing), không phải Serena.
- Chiến lược khuyến nghị: **coi Serena là kênh bổ sung** — một agent có cả hai sẽ dùng graph của memory-for-ai để hiểu trước, rồi tool edit của Serena để sửa đúng. Tài liệu marketing nên nhấn mạnh các khả năng Serena không có: blast radius, cross-service, Cypher, persistent graph, MIT license, single-binary.
