# PHÂN TÍCH TOÀN DIỆN DỰ ÁN `memory-for-ai` (CBM — Codebase Memory)

> Ngày phân tích: 2026-09-08 · Phương pháp: 6 luồng review chuyên sâu song song + xác minh chéo thủ công · Mọi phát hiện đều đã đối chiếu trực tiếp với code thật (file:dòng).

---

## 1. Tổng quan dự án

**memory-for-ai** là một MCP server viết bằng C thuần (zero-dependency, single static binary) biến toàn bộ codebase thành knowledge graph lưu trong SQLite, giúp AI coding agent trả lời câu hỏi cấu trúc bằng graph query thay vì đọc từng file.

| Thành phần | Quy mô | Vai trò |
|---|---|---|
| `src/` | 199 file, ~162K dòng C | MCP server, CLI, daemon, store (SQLite), Cypher engine, pipeline indexing, watcher, foundation |
| `internal/cbm/` | ~89K dòng code tự viết + 162 grammar tree-sitter vendored | Extraction engine AST + Hybrid LSP type resolution cho 10 ngôn ngữ |
| `tests/` | 125 file test C + 85 repro case + fuzz harness | Test framework tự viết, policy cấm skip |
| `graph-ui/` | ~6.4K dòng React/TS + three.js | UI 3D trực quan hóa graph |
| `scripts/` | 68 script Python/shell | License gate, benchmark, audit, release |
| `.github/workflows/` | 20 workflow | CI hardening: CodeQL, fuzzing, SLSA 3, cosign, VirusTotal |

**Bối cảnh pháp lý:** dự án là fork/derivative của `DeusData/codebase-memory-mcp` (paper arXiv:2603.27277 đã xác minh là có thật, tác giả Vogel et al., Charité Berlin). LICENSE giữ nguyên "Copyright (c) 2025 DeusData" — **tuân thủ MIT đúng luật**. Fork đã mở rộng đáng kể so với bản gốc trong paper (66 ngôn ngữ / 14 tools → 162 ngôn ngữ / 18 tools).

---

## 2. Xác minh các tuyên bố trong README

| Claim | Kết quả kiểm chứng |
|---|---|
| 18 MCP tools | ✅ Đúng — bảng `TOOLS` trong `src/mcp/mcp.c` có đúng 18 entries |
| 162 ngôn ngữ | ✅ Đúng — 162 file grammar shim `internal/cbm/grammar_*.c` |
| 45 client surfaces | ✅ Hợp lệ — registry 19-enum trong `agent_clients.h` + registry data-driven thứ hai trong `cli.c` (chứa aider, cline, warp, goose, zed, crush...) |
| Paper arXiv:2603.27277 | ✅ Có thật trên arXiv (submit 2026-03-28), các con số 83%/92%, 10× token, 2.1× tool calls khớp abstract |
| SLSA 3 / cosign / checksums / VirusTotal | ✅ Tồn tại thật trong release workflow |
| Benchmark claims | ✅ Có `scripts/benchmark-index.sh`, `benchmark-search-graph.sh`, dữ liệu đo thật trong `private/benchmarks/` |

---

## 3. Điểm mạnh nổi bật (hiếm gặp ở quy mô repo này)

1. **Độ cứng bảo mật cao bất thường:** SQL gần như 100% parameterized; installer bắt buộc verify checksum + chống HTTPS downgrade + validate archive closed-set; config write atomic (mkstemp + O_EXCL + O_NOFOLLOW + 0600 + rename); `activation_transaction.c` dùng dirfd-relative ops chống TOCTOU; 27/27 GitHub Actions pin bằng full SHA.
2. **Publish path SQLite thiết kế bài bản:** staging → seal → quarantine → atomic rename; crash giữa chừng không làm corrupt DB đang sống; WAL + busy_timeout đầy đủ.
3. **Không có SQL injection reachable:** Cypher engine evaluate in-memory, không dịch sang SQL string interpolation.
4. **MCP message reader phòng thủ tốt:** bounded line, Content-Length cap, reject NUL byte.
5. **Kỷ luật test:** policy cấm skip được CI enforce (`check-no-test-skips.sh`), 85 regression repro cases, fuzz harness, comment trong code ghi lại lịch sử bug đã fix (dạng "fixed in #1537...").
6. **XSS frontend được chủ động phòng và có test thật**; không `dangerouslySetInnerHTML`.
7. **Frontend TS strict đầy đủ**, không `any` trong production code.

---

## 4. PHÁT HIỆN THEO SEVERITY

### 🔴 High (9) — nên fix trước release kế tiếp

| # | Vị trí | Vấn đề |
|---|---|---|
| H1 | `internal/cbm/cbm.c:1242-1247` | Đường return skip file Perl (nesting quá sâu) **thiếu `cbm_index_mark_done()`** → file vô tội bị quarantine sau 2 lần worker crash. Fix 1 dòng. |
| H2 | `internal/cbm/cs_lsp.c:2208` | C# walker đệ quy **không có walk-depth guard** (7/10 ngôn ngữ khác đều chặn ở 512) → stack overflow crash worker trên file C# nested sâu, đặc biệt Windows (stack 1MB). |
| H3 | `internal/cbm/ts_lsp.c:3268` | TS/JS `process_node` đệ quy không depth cap — JS/TS là ngôn ngữ dễ gặp nesting cực sâu nhất (JSX, codegen). |
| H4 | `internal/cbm/rust_lsp.c:4632` | Rust có step cap 200000 nhưng **không có depth cap** — 200K frame vượt xa stack 8MB Linux; mutual recursion qua macro tokens. |
| H5 | `src/store/store.c:7795` | Stack buffer `binds[32]` không bound-check; `exclude_labels` > ~22 phần tử → stack smash. Hiện chưa có production caller (latent bomb ở public API). |
| H6 | `src/cypher/cypher.c:3191,3138,2343` | Cypher executor N+1 triệt để: load toàn bộ nodes vào RAM rồi filter, 1 query/edge, 2 query COUNT/node cho `in_degree` (trong khi `batch_count_degrees` tồn tại mà không dùng), `regcomp` lại mỗi row. |
| H7 | `src/cypher/cypher.c:3468,3547,3818,4042` | Bubble sort + distinct/aggregate O(n²) **không check deadline 30s** → query runaway treo hàng phút/giờ; cơ chế chống runaway bị hổng đúng chỗ quan trọng nhất. |
| H8 | `server.json:15` | MCP Registry vẫn trỏ npm identifier cũ `memory-for-ai` sau khi gói đổi tên `memory-for-ai-mcp` → publish registry sai/ownership validation fail ở release tới. Sót từ đợt đổi tên gần đây. |
| H9 | `.github/workflows/_security.yml:70-86` | CodeQL alert gate **fail-open**: thiếu permission `security-events: read` + nhánh `|| echo "0"` nuốt mọi lỗi API → gate luôn PASS dù có alert. Gate đứng trong đường publish. |

### 🟠 Medium (đáng chú ý nhất)

**Engine (internal/cbm):**
- `internal/cbm/arena.c/h` là **dead code + trùng include guard `CBM_ARENA_H`** với `src/foundation/arena.h` — bom hẹn giờ ODR/ABI, nên xóa hẳn.
- QN computation nhân bản giữa `extract_unified.c` và `extract_defs.c` — nguồn gốc loạt bug rớt edge #554/#621; cần một resolver duy nhất.
- `cbm_node_text` (`helpers.c:56`) không clamp byte range theo `source_len` — OOB read nếu trộn tree/source.
- Parse lần 2 (preprocessor, `cbm.c:1423`) không có deadline.
- YAML scan lặp subtree O(n·d), có thể emit string_refs trùng.
- Python attribute lookup có depth cap nhưng **không visited set** → phân rã mũ trên diamond inheritance (PHP/Kotlin đã có visited set).

**Store/Cypher/Pipeline:**
- `graph_buffer` flush/merge (`graph_buffer.c:1762,1836`) **không check lỗi, không rollback** — lệch chuẩn với `cbm_delta_patch` vốn làm đúng; graph có thể ghi nửa vời visible cho reader.
- `cbm_gbuf_delete_by_label` (`graph_buffer.c:820`) quên gỡ node khỏi `nodes_by_name` → node "ma".
- JSON parse bằng `strstr` (`cypher.c:2398`, `graph_buffer.c:182`) → sai giá trị khi key xuất hiện trong string value; có thể đụng dedup key → **mất edge IMPORTS**. Đã vendor yyjson nhưng không dùng thống nhất.
- Leiden input O(E·N) + dedup O(E²) (`store.c:10213`) — thuật toán đúng chuẩn paper nhưng chuẩn bị input chậm.
- `batch_count_degrees` (`store.c:6992`) silently drop id > ~4090, bỏ qua lỗi bind → kết quả sai âm thầm.
- `where_append` (`store.c:7599`) snprintf-truncation → negative-size OOB write (latent, chưa reachable).
- Full table scan ở `find_node_by_qn_any` (thiếu index không theo project-prefix).

**Daemon/Watcher/Foundation:**
- Race shutdown log sink (`daemon/host.c:115-121`): check ngoài mutex trong khi `fclose` + destroy mutex → UB cửa sổ shutdown.
- 6 điểm `stat()` trần trên Windows với path UTF-8 (`watcher.c:476,613,1244`, `git_context.c:258`...) → mất phát hiện thay đổi trên file/thư mục tên non-ASCII.
- IPC probe POSIX coi ECONNREFUSED là "active" → livelock startup với socket mồ côi (cần kiểm chứng thêm).

**Frontend (graph-ui):**
- Race condition `NodeDetailPanel.loadCode` (`NodeDetailPanel.tsx:64-88`): click nhanh 2 node → code của node cũ hiển thị dưới node mới.
- **GPU memory leak**: `EdgeLines` geometry không dispose khi re-compute (`EdgeLines.tsx:62`) — mỗi lần click/hover leak ~4MB buffer GPU với graph 80K edges.
- `selectedNode` sống sót khi đổi project → panel chi tiết hiển thị connections của node khác.
- Filter bị reset mỗi lần data reload; search sidebar không debounce (O(n) mỗi keystroke trên 250K nodes).

**Build/CI/Release:**
- `install.ps1:316-346` không rollback khi install fail giữa chừng → user Windows mất binary đang chạy.
- Manifests homebrew/scoop/winget/aur **lạc hậu vĩnh viễn** (0.8.1/0.10.3 vs release 0.10.8) — không có release job nào cập nhật; smoke test brew cài bản cũ rồi pass xanh, tự che giấu drift.
- `release.yml` thiếu `concurrency` guard → hai dispatch cùng version race trên cùng tag.
- License-gate dùng `scancode-toolkit` bản latest không pin hash — mâu thuẫn chính sách hash-pin của chính repo.

**MCP/CLI:**
- Race/UAF tiềm ẩn `active_request_id_str` (`mcp.c:14318`) đọc/ghi không qua mutex — severity thật phụ thuộc threading model của daemon session (cần kiểm chứng).

### 🟡 Low (~60 mục) — tổng hợp theo nhóm

- **JSON-RPC spec drift:** id lạ echo thành `-1` thay vì `null` + lỗi `-32600`; parse error dùng `id: 0`; unknown tool trả `isError` thay vì `-32602` (`mcp.c:199,14263,13957`).
- **Narrowing int64→int** ngầm trong `cbm_mcp_get_int_arg` (`mcp.c:1633`) — nên clamp tại helper.
- **JSON injection nhẹ** qua error message build bằng snprintf thủ công (`mcp.c:7541,1857`).
- **`manage_adr`**: mode lạ/update thiếu content thành silent no-op — trái chính sách "teaching error" của chính codebase.
- **Framing desync** khi client gửi `Content-Type` trước `Content-Length` (`mcp.c:14518`).
- **Tar/zip parser** (chỉ trong TEST_API build): `file_size` âm không validate, prefix-match tên binary, tràn `int header_end` với archive ~2GB.
- **Makefile**: 157/159 grammar shim thiếu explicit deps → incremental build có thể relink object cũ khi sửa scanner vendored.
- **Repo hygiene:** file `Formula` 20-byte rác ở root; thư mục `graph-ui/@/` 7 file trùng y hệt `src/components/ui/` đang bị git track; `three-stdlib` dùng mà không khai báo dependency.
- **OOM paths:** `safe_realloc` free con trỏ cũ rồi caller ghi vào NULL (~40 sites); `cli_slurp_stream` không giới hạn kích thước stdin.
- **Cypher numeric compare:** float bị coi là chuỗi khi ORDER BY; `(int)strtol` tràn với giá trị lớn.
- **WHERE với biến chưa khai báo** trả toàn bộ node thay vì báo lỗi.
- LSH `seen_set` calloc 128KB mỗi query (50K hàm → ~6.4GB allocator churn).

---

## 5. TOP 10 KHUYẾN NGHỊ ƯU TIÊN (theo thứ tự nên làm)

| # | Việc | Effort | Tác động |
|---|---|---|---|
| 1 | Thêm `cbm_index_mark_done()` vào đường return Perl (`cbm.c:1246`) | 1 dòng | Hết quarantine oan file |
| 2 | Sửa `server.json` identifier → `memory-for-ai-mcp` + thêm assert vào `check-product-metadata.py` | 30 phút | Tránh release registry hỏng |
| 3 | Đóng CodeQL gate fail-open: thêm `security-events: read`, bỏ `|| echo "0"` | 15 phút | Security gate có ý nghĩa thật |
| 4 | Chuẩn hóa walk-depth guard 512 cho C#/TS/Rust LSP | Vài giờ | Chặn 3 lỗ hổng crash stack-overflow |
| 5 | Xóa `internal/cbm/arena.c/h`, thống nhất include `foundation/arena.h` | 1 giờ | Gỡ bom ODR/ABI |
| 6 | Cypher: thay bubble sort/distinct bằng qsort + hash set, cắm deadline-check vào mọi vòng O(n²); dùng `batch_count_degrees`; cache regex | 1-2 ngày | Query lớn từ "treo" → chạy được |
| 7 | Sửa error contract `gbuf_flush_to_store`/`merge_into_store` (check return + rollback) theo mẫu `delta_patch` | Nửa ngày | Atomicity đường full-index |
| 8 | Frontend: dispose geometry EdgeLines + chống race loadCode + clear selectedNode khi đổi project | Nửa ngày | Hết leak GPU + hiển thị sai dữ liệu |
| 9 | Bound-check `binds[32]` + clamp `where_append` trong store search | 1 giờ | Dứt điểm lớp stack-overflow tiềm ẩn |
| 10 | Quyết định số phận pkg manifests (homebrew/scoop/winget/aur): viết release job cập nhật hoặc xóa khỏi repo | Nửa ngày | Chấm dứt drift version |

### Nợ kỹ thuật dài hạn đáng cân nhắc
- Hợp nhất QN resolution giữa def walk và unified walk (triệt gốc bug rớt edge kiểu #554/#621).
- Chuẩn hóa mọi JSON access qua yyjson (bỏ strstr-parse).
- Thêm bảng `schema_migrations` có thứ tự thay migration ad-hoc.
- Xác minh threading model daemon session để kết luận race `active_request_id`.
- Hardening tar/zip parser phòng khi update-in-binary được bật lại trong release.

---

## 6. Đánh giá tổng kết

| Tiêu chí | Đánh giá |
|---|---|
| Chất lượng code C | ★★★★☆ — cao bất thường; lỗi còn lại tập trung ở memory-safety tiềm ẩn và hiệu năng |
| Bảo mật & supply chain | ★★★★★ — một trong những pipeline hardening kỹ nhất ở quy mô này; chỉ còn CodeQL gate fail-open + server.json drift |
| Kiến trúc | ★★★★☆ — module hóa rõ ràng; điểm trừ: QN resolver nhân bản, JSON access không nhất quán |
| Test | ★★★★☆ — 125 file test, policy cấm skip, fuzz, 85 repro cases; thiếu test race cho hooks frontend và 3D layer |
| Docs/marketing | ★★★★★ — mọi claim kiểm chứng được, không phóng đại |
| Frontend | ★★★★☆ — strict TS, XSS sạch; cần sửa vòng đời three.js object và race condition |

**Kết luận:** Đây là dự án chất lượng cao với văn hóa engineering nghiêm túc (comment ghi lịch sử bug, policy no-skip, fail-closed mặc định). Không có lỗi Critical. 9 lỗi High chủ yếu là: crash stack-overflow trên 3 ngôn ngữ LSP chưa có depth guard, 1 bug quarantine oan file (fix 1 dòng), 2 vấn đề hiệu năng Cypher trên graph lớn, và 2 điểm drift trong release pipeline. Tất cả đều fix được với effort nhỏ — ưu tiên theo Top 10 ở mục 5.

---

## 7. Giới hạn của đợt phân tích

- `src/pipeline/` (~36K dòng) được đọc kỹ phần chạm SQLite/concurrency, một số pass thuần in-memory mới lướt qua.
- `extract_calls.c` (3.6K), `extract_usages.c` (2.7K), `sqlite_writer.c` (2.4K), `lang_specs.c` (2.7K) mới đọc chọn lọc theo điểm nóng.
- Không chạy thử build/test/CI thật; các phát hiện concurrency đều ghi rõ khi "cần kiểm chứng thêm".
- Báo cáo chi tiết từng mảng: xem thêm `REVIEW-CBM.md` (daemon/watcher) và `REVIEW-CBM-report.md` (internal/cbm engine) trong repo.
