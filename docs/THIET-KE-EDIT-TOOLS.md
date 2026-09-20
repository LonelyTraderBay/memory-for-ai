# Thiết kế kỹ thuật: Bộ edit tools (phương án D — clean-room)

> Spec ngày 2026-09-16. Phạm vi: thiết kế 3 tool mới `edit_symbol` → `delete_symbol` → `rename_symbol` cho memory-for-ai, clean-room (không copy code Serena), giữ nguyên MIT + single-binary.
> Tài liệu này **chỉ là thiết kế** — chưa sửa code. Mọi tham chiếu kiến trúc đều đối chiếu với codebase thật tại thời điểm viết.

---

## 1. Mục tiêu & phạm vi

**Mục tiêu**: cho agent khả năng sửa code ở mức symbol (thay body, chèn trước/sau, xóa an toàn, đổi tên) với độ tin cậy *cao hơn* các tool LSP-based nhờ kèm bằng chứng coverage/confidence trong mọi response.

**Non-goals** (cố ý không làm):
- Diagnostics từ compiler thật (cần LSP — phá zero-dependency).
- Debug tương tác, file utilities, shell (trùng agent harness).
- Edit các file không nằm trong project đã index.

**Ba nguyên tắc xuyên suốt** (kế thừa văn hóa correctness protocol của dự án):
1. **Evidence-first**: mọi response của edit tool phải kèm `coverage` + `confidence` + trạng thái re-index. Agent không bao giờ phải "tin mù" một edit.
2. **Dry-run mặc định**: lần gọi đầu luôn trả về *plan* (diff preview); apply chỉ khi `dry_run=false`. Tương tự triết lý `install --dry-run` hiện có.
3. **Atomic + revertible**: ghi file qua temp-file + rename; mọi edit tạo backup để `undo` trong cùng session.

## 2. Điểm tựa kiến trúc hiện có (đã kiểm chứng trong code)

| Tài sản sẵn có | Vai trò trong thiết kế |
|---|---|
| `cbm_node_t` (`src/store/store.h:37-47`): `file_path` (relative), `start_line`, `end_line`, `label`, `qualified_name` | Xác định chính xác vùng source của symbol — nền của mọi edit |
| `cbm_store_find_nodes_in_range()` (`src/store/store.h:166-168`) | Tìm node chồng lấn một line range — dùng cho kiểm tra nested symbol trước khi edit |
| `trace_path(direction="inbound")` + `include_evidence` (strategy `lsp`/`language_rule`/`heuristic`/`unresolved`) | Usage check cho `delete_symbol`; confidence tiers cho `rename_symbol` |
| `check_index_coverage` (paths ≤128, scopes ≤32) | **Gate bắt buộc** trước `rename_symbol` và trước mọi edit trên file có `coverage_note` |
| `pipeline_incremental.c` (disk-based incremental re-index, inbound cross-file edge preservation) | Re-index sau edit — không cần full re-index |
| `watcher/` (auto-sync theo git/filesystem) | Đồng bộ hóa sau edit; cần cơ chế tránh race (mục 7) |
| Tool registration trong `src/mcp/mcp.c`: bảng schema JSON tĩnh + `TOOL_ANNOTATIONS` (`mcp.c:789-808`) + dispatch strcmp chain (`mcp.c:~13936`) | Pattern đăng ký tool mới — 3 điểm chạm duy nhất vào mcp.c |
| Cơ chế `stale_cursor` (cursor không sống qua re-index) | Sẵn có để vô hiệu hóa pagination cũ sau edit |
| Node `body` column trong store (backfill, `store.h:~1194`) | Cache source của node — có thể dùng để verify trước khi ghi |

## 3. Kiến trúc tổng thể

### 3.1. Module mới: `src/edit/`

Không nhồi thêm vào `mcp.c` (đã ~14.7k dòng). Tạo module riêng:

```
src/edit/
  edit.h            Public API cho tầng MCP
  edit_resolve.c    qn → node → file range (dùng store)
  edit_surgery.c    Line-range text surgery (replace/insert/delete) trên buffer
  edit_write.c      Atomic write (temp + rename), backup/undo, mtime guard
  edit_plan.c       Diff preview (unified-ish, bounded) cho dry_run
  edit_rename.c     Occurrence collection + confidence partitioning cho rename
```

### 3.2. Luồng chung của một edit call

```
1. RESOLVE    qualified_name → cbm_node_t (file_path, start_line, end_line)
              → ambiguous → trả suggestions (tái dùng pattern của get_code_snippet)
2. GATE       check_index_coverage(paths=[file]) → nếu file có parse_partial/
              excluded → từ chối edit vùng đó, trả coverage_note (trừ khi force)
3. READ       Đọc file từ disk + ghi nhận mtime/size (optimistic concurrency)
4. VERIFY     Node range còn khớp source thật? (so signature dòng đầu của range
              với node.properties; lệch → báo stale, yêu cầu re-index)
5. PLAN       edit_surgery trên buffer → sinh diff preview
              → nếu dry_run=true (mặc định): trả plan + evidence, KẾT THÚC
6. APPLY      edit_write: temp file cùng thư mục → fsync → rename (atomic trên
              cả POSIX lẫn Windows); lưu backup vào ~/.cache/memory-for-ai/backups/
7. REINDEX    Gọi incremental re-index cho đúng 1 file (route của
              pipeline_incremental) — đồng bộ, blocking trong call
8. RESPOND    Trả: plan đã apply + evidence (coverage, confidence, reindex
              status, số node/edge thay đổi) + backup_id cho undo
```

### 3.3. Đăng ký tool (3 điểm chạm vào mcp.c)

1. Thêm entry vào bảng tool tĩnh (schema JSON inline như các tool hiện có).
2. Thêm vào `TOOL_ANNOTATIONS`: edit tools là những tool đầu tiên ghi vào source — cần cờ `read_only=false`, `destructive=true` (với `delete_symbol`), `idempotent=false`.
3. Thêm nhánh dispatch `handle_edit_symbol` / `handle_delete_symbol` / `handle_rename_symbol` vào chain.

## 4. Tool 1: `edit_symbol`

### Input schema

```json
{
  "project": "string (required)",
  "qualified_name": "string (required — qn đầy đủ từ search_graph)",
  "action": "replace_body | insert_before | insert_after (required)",
  "content": "string (required — source mới; replace_body: thân symbol mới,
             KHÔNG gồm signature; insert_*: block chèn, tự giữ indentation)",
  "dry_run": "boolean, default true",
  "force": "boolean, default false — bỏ qua coverage gate (response vẫn ghi
            'coverage_gate': 'bypassed')",
  "expected_mtime_ns": "integer, optional — optimistic concurrency từ plan call"
}
```

### Quy tắc surgery

- `replace_body`: thay các dòng `(signature_end+1 .. end_line)` của node. Signature line giữ nguyên — đây là khác biệt cốt lõi so với text-replace: agent không bao giờ vô tình đổi tên/tham số khi chỉ định sửa thân.
- `insert_before` / `insert_after`: chèn tại `start_line` / `end_line+1`; tự suy ra indentation từ dòng signature của node láng giềng.
- Từ chối nếu range chứa **nested symbol** mà action sẽ xóa mất (dùng `cbm_store_find_nodes_in_range`) — trừ khi `content` đã chứa lại định nghĩa đó (so khớp qn sau re-parse).

### Response (apply mode)

```
edit_symbol: APPLIED  myproj.src.handlers.ProcessOrder  (Method)
  action: replace_body · src/handlers/order.c:88-134 → 88-141
  diff: +19 / -12 lines
  coverage: file fully indexed (no gaps)
  confidence: HIGH (node range verified against source pre-write)
  reindex: incremental OK — 1 file, 23 nodes, 41 edges updated (184 ms)
  backup: bk_20260916_161203_order.c  (undo: edit_symbol với action='restore')
```

## 5. Tool 2: `delete_symbol`

Khác biệt then chốt so với safe-delete của LSP: usage check dùng **graph bắc cầu**, thấy cả caller qua Route/cross-service mà LSP không thấy.

### Luồng

```
1. RESOLVE + GATE như edit_symbol
2. USAGE CHECK  trace_path(qn, direction="inbound", depth=5, include_evidence=true)
   → callers_total == 0  → safe, confidence HIGH
   → callers chỉ trong test files → safe-with-note (đề xuất xóa test kèm theo)
   → callers thật → danh sách callers (prefix-grouped, giới hạn 20 + total)
3. Nếu có callers thật và force=false → TỪ CHỐI, trả caller list + gợi ý
   ("xóa/cập nhật N caller trước, hoặc force=true")
4. Surgery: xóa range start_line..end_line + dòng trống thừa liền kề
5. Write + reindex + respond (kèm danh sách node/edge đã gỡ khỏi graph)
```

### Input bổ sung

- `include_tests` (default false): cho phép xóa cả khi chỉ còn test callers — response liệt kê các test bị mồ côi để agent quyết định dọn tiếp.
- `recursive_orphans` (default false): sau khi xóa, quét callee của symbol vừa xóa — callee nào mất hết caller thì liệt kê làm orphan gợi ý xóa tiếp (dead-code cascade, chính là pattern "propagate deletions" nhưng chủ động hơn nhờ graph). Cascade resolve theo **node ID** (không phải qualified name), conservative: caller list bị cap hoặc resolve ambiguous thì không liệt kê; tối đa 10 orphan/response. ✅ Đã implement.

## 6. Tool 3: `rename_symbol`

Tool khó nhất — và cũng là nơi memory-for-ai có thể **vượt** Serena nhờ evidence.

### Nguyên tắc an toàn: coverage gate là điều kiện tiên quyết

LSP rename "tin mù" rằng language server thấy hết references. memory-for-ai làm tường minh:

```
1. RESOLVE qn → node
2. COLLECT occurrences:
   a. Graph: mọi edge USAGE / CALLS / CALL_REFERENCE / IMPORTS / IMPLEMENTS
      trỏ tới node, kèm file_path + line + resolution strategy
      (lsp | language_rule | heuristic | unresolved)
   b. Vét text: search_code(name_pattern) trên toàn repo để bắt occurrences
      mà graph miss (string literal, reflection, comment, macro)
3. GATE: check_index_coverage(paths=[mọi file chứa occurrence])
   → file nào có gap → occurrences trong gap = "unknown-confidence"
4. PARTITION kết quả thành 3 tầng:
   HIGH     — edges strategy=lsp/language_rule + file fully covered → sửa tự động
   REVIEW   — heuristic + text-only matches trong code → liệt kê từng cái,
              agent duyệt (tham số approved_occurrences)
   SKIP     — comment/docstring/unresolved → mặc định không đụng
5. dry_run plan: bảng occurrences theo tầng, theo file; tổng số thay đổi
6. Apply: sửa theo thứ tự bottom-up theo line trong từng file (tránh lệch
   offset); mỗi file một atomic write; re-index tất cả file đã chạm
7. Verify sau re-index: node mới với tên mới tồn tại; query kiểm tra không còn
   edge nào trỏ tên cũ → báo cáo vòng kiểm chứng khép kín
```

### Input schema bổ sung

```json
{
  "new_name": "string (required)",
  "scope": "definition_only | project (default project)",
  "approved_occurrences": "array of {file, line} — duyệt tầng REVIEW",
  "include_comments": "boolean, default false",
  "expected_counts": "integer, optional — agent assert số occurrence trước khi
                      apply; lệch → abort (chống apply nhầm plan cũ)"
}
```

### Giới hạn công khai (phải ghi trong tool description)

- Dynamic dispatch (getattr/reflection/string-based lookup) nằm ở tầng REVIEW/SKIP — tool **không** tự sửa, chỉ liệt kê.
- `expected_counts` + coverage gate là hai cơ chế chống "rename mò" — đây là câu chuyện khác biệt hóa chính so với Serena.

## 7. Concurrency & tương tác watcher

- **Race agent-edit vs watcher**: watcher poll có thể bắt đầu re-index giữa lúc write. Giải pháp: `edit_write` đi qua daemon (per-account session coordination đã có) — daemon giữ per-project write-lock; watcher và edit tool cùng acquire. Edit là thao tác ngắn (<1s) nên lock không ảnh hưởng throughput.
- **Optimistic concurrency**: mtime/size đọc ở bước 3 phải khớp lúc ghi; lệch → abort với lỗi `file_changed_externally`, kèm gợi ý re-run plan. `expected_mtime_ns` cho phép agent ghim plan→apply.
- **Cursor invalidation**: sau re-index, mọi cursor `trace_path` cũ tự động stale (cơ chế `stale_cursor` đã có — không cần làm gì thêm).

## 8. Testing strategy

| Tầng | Nội dung | Trạng thái |
|---|---|---|
| Unit (`tests/`) | edit_surgery trên buffer: replace/insert/delete ở đầu/giữa/cuối file, CRLF vs LF, file không có trailing newline, nested symbols, unicode | ✅ suite `edit` 36 test |
| Integration | Fixture repos đa ngôn ngữ (Python + TS + Go + C — 4 ngôn ngữ có full Hybrid-LSP): edit → re-index đồng bộ → assert disk + graph mới (node range, edges) đúng qua production MCP flow | ✅ suite `edit_integration` 11 test (`tests/test_edit_integration.c`) |
| Concurrency | Fault-injection theo pattern sẵn có của `pipeline_incremental.c` (`*_test_fail_*_once`): ép write thất bại (`CBM_EDIT_TEST_API=1` + `cbm_edit_write_test_fail_once`) → assert không file nào corrupt, journal/undo vẫn nhất quán | ✅ 2 test fault trong suite `edit_integration` |
| Rename adversarial | Repo có reflection/string-lookup → assert occurrences rơi đúng tầng REVIEW/SKIP, không bị sửa mò | ✅ (suite `edit`) |
| A/B đo giá trị | Theo `docs/MEASURING.md`: so token/tool-call của "rename bằng edit tools" vs "rename bằng grep + read + write thủ công" trên 5 task chuẩn × 2 kịch bản kích thước file | ✅ `scripts/ab-edit-tools.py` → kết quả tại `docs/AB-RESULTS.md` §"A/B — edit tools": giảm 90.5% token ở file ~300 dòng, thua trên file ~10 dòng (break-even là kích thước file, không phải fan-out) |

## 9. Roadmap & effort ước lượng

| Phase | Nội dung | Effort | Phụ thuộc | Trạng thái |
|---|---|---|---|---|
| 1 | `src/edit/` skeleton + `edit_symbol` (3 actions) + dry-run + atomic write + single-file re-index | ~2–3 tuần | Không | ✅ Xong |
| 2 | `delete_symbol` + usage check + orphan cascade (gợi ý) | ~1–2 tuần | Phase 1 | ✅ Xong |
| 3 | `rename_symbol` 2-phase (plan/apply) + coverage gate + 3 tầng confidence | ~3–4 tuần | Phase 1, 2 | ✅ Xong |
| 4 | `undo_edit` (restore từ backup) + `expected_counts` + polish docs/AGENT_GUIDE | ~1 tuần | Phase 3 | ✅ Xong |
| 5 | `move_symbol` (scope chặt theo §11: cùng ngôn ngữ, Python+TS trước, không re-export/circular) | ~3–4 tuần | Phase 3 + số liệu A/B | 🔜 GO — chưa bắt đầu |
| — | `inline_symbol` | — | Demand evidence | ⏸️ NO-GO, revisit theo §11 |

Sau mỗi phase: cập nhật `docs/AGENT_GUIDE.md` (tool catalog + playbook), `docs/llms.txt`, và số "18 tools" → tăng tương ứng ở README (hiện tại: 22 tools). Unit test C cho cả 4 tool nằm trong `tests/test_edit.c` (suite `edit`, 36 test); integration + fault-injection trong `tests/test_edit_integration.c` (suite `edit_integration`, 11 test); logic parity test (không cần compiler) ở `build/edit_surgery_logic_test.py` (27 ca).

## 10. Rủi ro chính & giảm thiểu

| Rủi ro | Xác suất | Giảm thiểu |
|---|---|---|
| Edit làm hỏng file user (mất dữ liệu) | Thấp nhưng nghiêm trọng | Atomic write + backup mọi edit + dry-run mặc định + `undo` |
| Graph stale sau edit → agent đọc thông tin cũ | Trung bình | Re-index đồng bộ *trong* call, không async; response kèm reindex status |
| Rename sai trên dynamic language | Trung bình | 3 tầng confidence + coverage gate + `expected_counts`; không bao giờ sửa occurrence `unresolved` |
| Phình scope (agent muốn move symbol, inline…) | Cao | Đã chốt (§11): move = GO scope chặt Phase 5; inline = NO-GO tới khi có demand evidence |
| Lock contention với watcher trên repo lớn | Thấp | Per-file lock granularity; edit <1s; watcher backoff đã adaptive |

## 11. Quyết định move/inline sau số liệu A/B (2026-09-19)

Điều kiện §10 đặt ra đã đủ: có số liệu A/B ([AB-RESULTS.md](AB-RESULTS.md) §"A/B — edit tools"). Phân tích và quyết định:

**Suy luận từ số liệu rename:** chi phí edit tools gần như phẳng (~1.0–1.2K token/rename) bất kể fan-out hay kích thước file; chi phí thủ công tăng tuyến tính theo tổng bytes các file bị đụng. Break-even là **kích thước file** — vùng thắng của edit tools là *nhiều file + file lớn* (90.5% token reduction, 3 calls thay 12 ở fan-out 5 file ~300 dòng).

### `move_symbol` → **GO** (Phase 5, scope chặt)

- **Về mặt cấu trúc, move luôn nằm trong vùng thắng**: một move tối thiểu đụng file nguồn + file đích + mọi file import — tức fan-out ≥ 2–3 file ngay cả trong ca đơn giản nhất, đúng chế độ edit tools áp đảo.
- **Giá trị vượt xa token**: phần khó của move không phải cắt/dán thân hàm mà là **viết lại import cho đúng** — đây cũng là chỗ agent sửa tay hay sai nhất (thiếu import, thừa import, sai đường dẫn tương đối). Graph đã có sẵn IMPORTS edges để liệt kê chính xác ai import symbol, và máy móc 3 tầng confidence + coverage gate của rename tái dùng trực tiếp.
- **Scope chặt để giữ rủi ro ở mức rename**: cùng ngôn ngữ; Python + TS trước (đã có fixture trong `edit_integration`); KHÔNG theo re-export chain, KHÔNG tự gỡ circular import (đưa vào REVIEW); Go/C (include path, header/impl split) để phase sau khi Python/TS ổn định.
- Effort ước lượng ~3–4 tuần, ngang rename (import generation là phần khó tương đương coverage gate).

### `inline_symbol` → **NO-GO hiện tại**, revisit khi có demand

- **Rủi ro đúng cao nhất trong các ứng viên**: parameter substitution, name capture, recursion, early return, evaluation order, closure — IDE truyền thống cũng chỉ inline được ca tầm thường. Một inline sai trên 20 call-site là thiệt hại lớn dù có `undo_edit`.
- **Số liệu A/B không ủng hộ**: inline thường áp dụng cho helper nhỏ nằm trong file nhỏ — đúng vùng edit tools *thua* (small scenario: −310%). Đây là ca sửa tay rẻ hơn cả về token lẫn rủi ro.
- Hoãn không tốn chi phí kỹ thuật: máy móc REVIEW/SKIP + coverage gate + journal/undo đều tái dùng được nếu sau này làm.
- **Điều kiện revisit**: có demand thực (issue/feedback người dùng), hoặc sau khi `move_symbol` hoàn thiện và ổn định.

### Dữ liệu còn thiếu

A/B đo được *giá trị mỗi lần dùng*, không đo được *tần suất nhu cầu* — không có telemetry về việc agent thật sự cần move/inline bao nhiêu lần trong session. Hành động kèm theo: đã mở GitHub issue pinned [#12 — RFC use-case move/inline](https://github.com/LonelyTraderBay/memory-for-ai/issues/12) thu thập use-case từ người dùng thật; nếu inline không có demand sau 1–2 tháng (tính từ 2026-09-19) thì xóa hẳn khỏi roadmap thay vì treo NO-GO vĩnh viễn.

## 12. Thiết kế chi tiết `move_symbol` (Phase 5, 2026-09-19)

Quyết định GO tại §11. Mục tiêu: chuyển một symbol (Function/Class top-level) từ module nguồn sang module đích **cùng project, cùng ngôn ngữ**, viết lại import ở mọi nơi import nó — với evidence tiers, dry-run mặc định, atomic write + backup + undo như 3 tool trước.

### 12.1 Scope chặt (vi phạm → REVIEW hoặc từ chối, KHÔNG làm mò)

| Được hỗ trợ (auto-apply khi có evidence) | REVIEW (chỉ apply với force=true) | Từ chối hẳn |
|---|---|---|
| Symbol label Function/Class **top-level** (không lồng trong symbol khác — check bằng node cha trong graph) | Import có alias (`from m import f as g` — giữ alias, chỉ đổi module) | Method/nested function (parent là Class/Function khác) |
| Python `from mod import f` / TS `import {f} from "./mod"` | Re-export (`__all__` Python, `export {f} from` TS) — liệt kê, không tự sửa | Move sang module khác ngôn ngữ |
| TS relative path recomputation (`./` vs `../`, path mới tính từ vị trí file importer so với file đích) | Barrel file (index.ts re-export) — liệt kê làm REVIEW | Destination module chưa tồn tại (Phase 5 không tạo file mới — agent tự tạo rồi move sau) |
| | Circular import phát sinh sau move (dest import lại source) — liệt kê cả 2 chiều | Destination đã có symbol trùng tên (collision → báo lỗi, gợi ý rename_symbol trước) |

### 12.2 Luồng 2-phase (giữ nguyên pattern rename_symbol)

**Plan (dry_run=true, mặc định):**
1. Resolve symbol → node + file nguồn (dùng `cbm_edit_resolve_symbol`).
2. Resolve destination: arg `destination_module` (qn của Module node, vd `proj.pkg.utils`) → file đích; kiểm tra tồn tại + cùng extension + không collision.
3. Lấy toàn bộ IMPORTS edges vào symbol (`cbm_store_find_edges_by_target_type(store, node_id, "IMPORTS", ...)`) — đây là danh sách importer **graph-verified (HIGH tier)**, mỗi edge kèm `local_name` để phát hiện alias.
4. Sweep bổ sung bằng `cbm_edit_scan_identifier` trên các file import module nguồn nhưng không có edge → REVIEW tier (dynamic import, `importlib`, `require()` động).
5. Circular check: BFS 1 bước trên IMPORTS edges của module đích — nếu đích (hoặc module mà đích import) import lại nguồn → REVIEW cả move, response vẽ chu trình.
6. Trả plan: per-file tier table + import diff mẫu + warning re-export/barrel/circular. Không ghi gì.

**Apply (dry_run=false):**
1. Re-verify: định nghĩa tại nguồn còn khớp source (definition-drift), mọi file importer khớp mtime với lúc plan nếu agent truyền `expected_files` (số file sẽ bị sửa — pin plan→apply giống `expected_counts`).
2. **Thứ tự ghi (quan trọng cho undo)**:
   a. Đọc thân symbol từ file nguồn (line range từ graph).
   b. Ghi file đích: chèn thân symbol vào vị trí `position` (mặc định `end` — cuối file; tuỳ chọn `after_imports`).
   c. Ghi file nguồn: xoá thân symbol (`cbm_edit_surgery_delete` — đã hấp thụ blank line thừa).
   d. Ghi từng file importer: viết lại import theo ngôn ngữ (§12.3).
   Mỗi file: mtime guard + backup + atomic write (máy móc `edit_write.c` y nguyên). Một file fail → dừng, báo rõ file nào đã ghi/chưa ghi + backup paths (không tự rollback — undo_edit từng file, giống rename_symbol).
3. Reindex 1 lần duy nhất sau khi mọi file ghi xong (`handle_index_repository` nội bộ).
4. Post-verify trên graph mới: symbol resolve được ở qn đích; IMPORTS edges mới chỉ về module đích; không còn HIGH-tier import nào trỏ module nguồn cho symbol đó.

### 12.3 Import rewriting (phần khó nhất — module mới `src/edit/edit_move.c`)

**Python:**
- `from mod_a import f` → `from mod_b import f` (đổi module path, giữ nguyên tên/alias).
- `from mod_a import f, g` (nhiều symbol, chỉ move `f`) → tách thành 2 dòng: `from mod_a import g` + `from mod_b import f` (giữ thứ tự dòng để diff tối thiểu).
- `import mod_a` + dùng `mod_a.f` → REVIEW (attribute access cần đổi cả call-site, Phase 5 không chạm).
- Alias `from mod_a import f as f1` → `from mod_b import f as f1` (REVIEW tier vì call-site dùng `f1` không đổi — an toàn nhưng cần người đọc xác nhận ý đồ).
- `__all__ = [..., "f", ...]` trong file nguồn → REVIEW (liệt kê dòng, không tự sửa).

**TypeScript/JavaScript:**
- `import {f} from "./mod_a"` → `import {f} from "<relative-path-mới>"` — tính lại relative path từ thư mục của file importer tới file đích (không đuôi `.ts/.js`), chuẩn hoá `./` prefix.
- `import {f, g} from "./mod_a"` → tách giống Python.
- Default import / namespace import (`import * as m`) / side-effect import → REVIEW.
- `export {f} from "./mod_a"` (barrel) → REVIEW (đổi barrel là quyết định API surface, agent phải xác nhận).
- Path alias (`@/mod_a`, tsconfig paths) → REVIEW (Phase 5 không đọc tsconfig).

**Sau move, file nguồn có thể vẫn cần symbol** (symbol khác trong file nguồn gọi nó): nếu graph có CALLS edge từ trong file nguồn vào symbol → tự thêm `from mod_b import f` / `import {f} from "./mod_b"` vào file nguồn (HIGH tier, vì evidence là CALLS edge). Đây là ca rename_symbol không bao giờ gặp — điểm khác biệt lớn nhất của move.

### 12.4 Schema MCP (dự kiến)

```
move_symbol(qualified_name, project, destination_module,
            position="end"|"after_imports",  # default "end"
            dry_run=true,                    # default: plan only
            force=false,                     # apply REVIEW tier + bypass drift check
            expected_files=<int>)            # pin số file sẽ sửa từ plan trước
```

Response prefixes: `move_symbol: DRY-RUN` / `move_symbol: APPLIED` / `move_symbol: PARTIAL` (một số file fail — kèm danh sách backup để undo).

### 12.5 Test plan

| Tầng | Nội dung |
|---|---|
| Unit (suite `edit`) | Import rewriting Python/TS trên buffer: single/multi symbol, alias, tách dòng, relative path recomputation (cùng thư mục, lên 1-2 cấp, xuống cấp), file nguồn cần re-import |
| Integration (suite `edit_integration`) | Fixture Python + TS: move function 2 file → assert disk (thân ở đích, import đúng) + graph mới (node ở file đích, IMPORTS edges trỏ đích); move tạo circular → REVIEW không apply; destination collision → từ chối |
| Fault injection | `cbm_edit_write_test_fail_once` giữa chuỗi ghi nhiều file → PARTIAL + không file nào corrupt + backup đủ để undo |
| A/B | ✅ Mở rộng `scripts/ab-edit-tools.py` thêm task move (5 mức fan-out × 2 kịch bản, quality byte-identical 10/10) — kết quả tại `docs/AB-RESULTS.md` §"A/B — edit tools": giảm 93.2% token ở padded300 |

### 12.6 Chia nhỏ implement (mỗi bước 1 commit)

1. **5a** ✅ (`c7825aff`) — `src/edit/edit_move.c` core: extract/insert/delete body + Python import rewriting + unit test.
2. **5b** ✅ (`c011c1e8`) — TS import rewriting + relative path recomputation + unit test (suite `edit` 82 test).
3. **5c** ✅ (`54a56cda`) — `handle_move_symbol` trong mcp.c: plan/apply, circular check, re-export detection, schema + tool count 22→23.
4. **5d** ✅ (`98eab359`) — Integration + fault-injection tests (suite `edit_integration` 17 test). Phát hiện và sửa 2 bug thật: TS importer resolve về Module node (không phải symbol) nên sweep phải nhìn cả 2 target; và use-after-free con trỏ `irel` sau `cbm_edit_free_node`.
5. **5e** ✅ — Docs (AGENT_GUIDE, llms.txt 22→23 tools) + metadata đã regen ở 5c.
