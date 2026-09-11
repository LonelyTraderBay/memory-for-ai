# Code Review: internal/cbm (memory-for-ai / CBM)

Phạm vi: code tự viết trong `internal/cbm/` (arena, cbm, extract_*, lsp/*, ac, scope, type_registry...). Bỏ qua grammar vendored (`grammar_*.c`, `vendored/`). Mọi phát hiện đều đã đọc code thực; mục nào chưa chắc được ghi "cần kiểm chứng thêm".

---

## 1. Memory safety / Arena

### 1.1 `internal/cbm/arena.c` là code chết + trùng include guard — Medium
- `Makefile.cbm:258` build `src/foundation/arena.c`; `EXTRACTION_SRCS` (Makefile.cbm:286-303) **không** chứa `internal/cbm/arena.c`. Bản `internal/cbm` còn không tự compile được: dùng `SKIP_ONE`/`PAIR_LEN` (arena.c:12,20,52,61...) nhưng chỉ include `arena.h` + libc, không include `foundation/constants.h` nơi 2 hằng này định nghĩa.
- Cả hai `internal/cbm/arena.h` và `src/foundation/arena.h` dùng chung include guard `CBM_ARENA_H`. `cbm.c:2` include `"arena.h"` (resolve theo thư mục hiện tại → bản internal/cbm), trong khi implementation link từ `src/foundation/arena.c`. Hiện struct layout giống nhau nên "may mắn chạy đúng"; một TU include cả hai header theo thứ tự khác sẽ âm thầm dùng sai khai báo → nguy cơ ODR/ABI mismatch khó phát hiện.
- **Fix**: xóa hẳn `internal/cbm/arena.c/h` (hoặc đổi guard + đồng bộ), và đổi include trong cbm.c thành `"foundation/arena.h"` cho tường minh.

### 1.2 Tràn số trong growth path của arena — Low
`src/foundation/arena.c:42` (và bản chết internal/cbm/arena.c:20):
```c
size_t new_size = a->block_size * PAIR_LEN;   // không có overflow check
```
`block_size` có thể bị đẩy lên bất kỳ qua đường `min_size` (alloc một object khổng lồ); nhân đôi lặp có thể wrap `size_t`. Thực tế khó xảy ra (cần alloc ~2^63) nhưng là lỗ hổng hardening. Tương tự `cbm_arena_alloc` (arena.c:63): `n = (n + ARENA_ALIGN) & ~ARENA_ALIGN` wrap về 0 khi `n > SIZE_MAX-7` → trả về con trỏ hợp lệ cho yêu cầu khổng lồ.
- **Fix**: `if (n > SIZE_MAX - 8) return NULL;` và `if (a->block_size > SIZE_MAX / 2) new_size = min_size;`.

### 1.3 `cbm_node_text` không clamp byte range theo source_len — Medium (cần kiểm chứng thêm cho đường exploit cụ thể)
`internal/cbm/helpers.c:56-63`:
```c
uint32_t start = ts_node_start_byte(node);
uint32_t end   = ts_node_end_byte(node);
if (end <= start) return cbm_arena_strdup(a, "");
return cbm_arena_strndup(a, source + start, end - start);  // không check end <= source_len
```
An toàn chỉ khi node luôn thuộc tree parse từ đúng `source` đó. Toàn bộ extraction/LSP đều gọi hàm này; rủi ro là đường dùng node từ tree cũ với source khác (vd `result->cached_tree` được giữ lại cho "cross-file LSP reuse", cbm.c:1662). Tôi không verify được một call-site thực sự trộn tree/source, nhưng API không phòng thủ: một ngày có bug lifetime là thành OOB read.
- **Fix**: đổi signature thành `cbm_node_text(a, node, source, source_len)` và clamp `end = min(end, source_len)`; hoặc ít nhất assert trong bản debug.

### 1.4 ac.c — các đường OOM không kiểm tra — Low
- `ac.c:53-57` `queue_init` không check `malloc` NULL; `queue_push` deref NULL ngay khi OOM. `ac_build_failure` (ac.c:104) cũng không check `calloc`.
- `ac.c:202-203` `memset(go_table, CBM_AC_NO_STATE /* -1 */, ...)` — đúng trên two's complement nhưng là idiome mong manh; nên fill bằng vòng lặp hoặc dùng giá trị 0 cho "no state" và dời root.
- `ac.c:187` `max_states += lengths[i]` — tràn `int` nếu tổng pattern > 2GB (input nội bộ, ít rủi ro).
- `get_decomp_buf` (ac.c:258-267): `needed + 0xFFFF` có thể tràn int → cap âm → trả NULL (caller đã check, OK), nhưng `tls_decomp_cap` được gán kể cả khi malloc thất bại — không hỏng, chỉ khó đọc.
- **Fix**: check NULL sau mọi malloc/calloc trong ac.c; dùng `size_t` cho tổng.

### 1.5 ac.c — mất pattern khi >64 pattern chia sẻ terminal state — Medium
`ac.c:89`: `ac->output_list[state] = p;` — ghi đè: hai pattern ≥64 cùng kết thúc tại một state (vd pattern là hậu tố của nhau) thì chỉ pattern cuối được ghi nhận trong output chain; `cbm_ac_scan_batch` (ac.c:386-406) đi theo `output_next` (chỉ trỏ tới fail-state) nên pattern bị ghi đè không bao giờ được báo. Bitmask path (≤64 pattern) không bị.
- **Fix**: mỗi state giữ danh sách pattern thật (mảng nhỏ hoặc linked list của pattern ids), hoặc tài liệu hóa rõ ràng giới hạn "chỉ dùng batch mode khi không có hai pattern nào là suffix của nhau".
- Cần kiểm chứng thêm: liệu caller hiện tại (configlinker) có bao giờ >64 pattern không.

---

## 2. Extraction / tree-sitter traversal

### 2.1 Đường return Perl thiếu `cbm_index_mark_done` — High
`internal/cbm/cbm.c:1242-1247`:
```c
if (language == CBM_LANG_PERL && cbm_source_nesting_exceeds(...)) {
    result->has_error = true;
    result->error_msg = cbm_arena_strdup(a, "perl source nesting too deep; skipped");
    return result;            // <-- thiếu cbm_index_mark_done(rel_path);
}
```
Mọi đường return khác đều gọi `mark_done` (dòng 1225, 1234, 1254, 1287, 1664). File Perl bị skip sẽ có mark "S" không có "D" → rơi vào suspect set của crash supervisor mỗi lần worker crash sau đó; xuất hiện trong 2 lần chạy liên tiếp → **file vô tội bị quarantine** (chính loại bug mà comment ở dòng 563-577 mô tả là đã từng làm 4 fixture bị quarantine oan).
- **Fix**: thêm `cbm_index_mark_done(rel_path);` trước `return`.

### 2.2 Không timeout cho parse lần 2 (preprocessor) và các LSP pass — Medium
`cbm.c:1423`: `TSParseOptions pp_opts = {0};` — không gắn `progress_callback`/deadline như parse chính (1272-1278). Expanded source của file C/C++ bệnh lý được parse không giới hạn thời gian; các `cbm_run_*_lsp` cũng chạy ngoài deadline. Các LSP có step-budget riêng (vd c_lsp `C_EVAL_MAX_STEPS_PER_FILE`, rust `CBM_RUST_EVAL_STEP_CAP`) nên rủi ro hang chủ yếu nằm ở parse lần 2.
- **Fix**: truyền cùng deadline còn lại vào `pp_opts`.

### 2.3 `handle_yaml_nested` + `scan_infra_bindings`: quét lặp toàn bộ subtree → O(n·d), có thể duplicate — Medium
Main walk (`extract_unified.c:2407-2434`) duyệt mọi node; với **mỗi** `block_mapping`/`array`/`document` lại gọi `scan_yaml_for_infra_bindings(ctx, node)` (1613) — hàm này tự walk toàn bộ descendant. YAML lồng nhau dẫn đến quét chồng lấp O(n·d). Tương tự `handle_yaml_nested` (1804): guard "root-level" chỉ so parent kind `stream|document|block_node`, mà trong tree-sitter-yaml một mapping lồng dưới `block_mapping_pair → block_node → block_mapping` cũng có parent là `block_node` → nested mapping cũng trigger `walk_yaml_mapping`, trong khi walk từ root đã cover nó → **khả năng emit string_refs trùng** (cần kiểm chứng thêm bằng test trên YAML lồng; nếu pipeline dedupe thì còn lại vấn đề perf).
- **Fix**: chỉ scan khi node là mapping gốc (kiểm tra grandparent), hoặc đánh dấu node đã scan.

### 2.4 Stack cố định, silently drop subtree — Low
`extract_unified.c:1402` (`YAML_WALK_STACK_CAP 256`), `:1612` (`INFRA_SCAN_STACK_CAP 512`): đầy thì ngừng push → mất lặng lẽ nhánh sâu — đúng bug class mà `extract_node_stack.h` được viết ra để sửa (issue #199), nhưng 2 site này không dùng nó.
- **Fix**: dùng `TSNodeStack` (arena-backed, grow 2x).

### 2.5 Quét sibling lùi tuyến tính per-node → O(n²) trên file phẳng — Low/Medium
`nasm_preceding_label` (extract_unified.c:1997) và `objectscript_routine_preceding_tag` (2030): với mỗi instruction/statement quét `prev_named_sibling` lùi đến đầu → file NASM/ObjectScript phẳng N dòng thành O(N²). Có giới hạn 4 mức ancestor nhưng sibling scan không giới hạn.
- **Fix**: cache "label gần nhất" trong WalkState, cập nhật khi gặp label, O(1) per node.

### 2.6 Vòng lặp attribution O(defs × calls) với strcmp trong inner loop — Low
`cbm.c:1562-1617`: mỗi call quét mọi def, trong đó có `strcmp(d->label, "Function")` chạy defs×calls lần. File generated lớn (10⁴ defs × 10⁵ calls) → 10⁹ strcmp.
- **Fix**: pre-build mảng chỉ gồm Function/Method defs (đã sort theo start_line) rồi binary search theo `c->start_line`.

### 2.7 `r_collect_imports` đệ quy không depth guard + ts_node_named_child O(n²) — Low
`extract_imports.c:900-903`: đệ quy theo độ sâu AST của file R, không cap (khác với mọi import pass khác đều cursor-based); child access theo index (O(n²) trên node rộng). Thực tế file R ít lồng sâu nên rủi ro thấp.
- **Fix**: chuyển sang cursor walk như các pass khác.

### 2.8 GROW_ARRAY silently drop khi arena cạn — Low
`cbm.c:106-119`: OOM → `return` lặng, mất record không báo. Có chủ đích ("best-effort"), nhưng kết quả là graph thiếu cạnh không dấu vết.
- **Fix**: set cờ `result->extraction_degraded` khi drop để pipeline biết.

### 2.9 Encoding: giả định UTF-8 tuyệt đối — Low (cần kiểm chứng thêm)
`cbm.c:1268,1420` hardcode `TSInputEncodingUTF8`; không thấy xử lý BOM/UTF-16 trong internal/cbm. File UTF-16/UTF-8-BOM sẽ parse sai lệch (byte offset không khớp ký tự). Nếu tầng đọc file (src/pipeline) đã chuẩn hóa thì bỏ qua — cần kiểm chứng ở tầng đó (ngoài phạm vi review này).

---

## 3. Hybrid LSP / type resolution

### 3.1 C# walker không có walk-depth guard — High
`cs_lsp.c:2208` `cs_resolve_calls_in_node` đệ quy theo độ sâu AST (2221, 2274, 2454-2481, 2876) — **không** có `walk_depth` (`cs_lsp.h` chỉ có `eval_depth`). Trong khi py/php/go/kotlin/java/perl/c đều chặn ở 512 (`cbm_lsp_max_walk_depth()`, scope.h:45 — comment nói rõ mục đích chống "native stack overflow (SIGSEGV) that takes down the whole index"). File C# generated/deeply-nested → stack overflow → crash worker.
- **Fix**: thêm `int walk_depth` vào CSLSPContext và guard giống `go_lsp.c:19-23`.

### 3.2 TS/JS `process_node` không có walk-depth guard — High
`ts_lsp.c:3268` `process_node` đệ quy per-child (3282, 3318, 3342, 3627, 3690...) chỉ có `eval_depth` (type evaluation) và `member_depth` — không có cap cho độ sâu AST walk. JS/TS là ngôn ngữ dễ gặp nesting cực sâu nhất (JSX lồng, ternary chain, object literal khổng lồ do codegen — chính repo đã từng gặp `reallyLargeFile.ts` 583k dòng comment).
- **Fix**: thêm walk_depth guard như các ngôn ngữ khác.

### 3.3 Rust: step cap không thay thế được depth cap — High
`rust_lsp.c:4632-4640`: `CBM_RUST_EVAL_STEP_CAP 200000` đếm tổng node visit, nhưng recursion depth vẫn vô hạn — 200k frame × vài trăm byte/frame vượt xa stack 8MB Linux, gấp ~40 lần stack 1MB của Windows (comment tại cbm.c:735-754 ghi nhận Windows/ARM từng overflow ở 1MB). `rust_walk_macro_tokens` (2831-2846) đệ quy vào token tree không depth guard, và gọi lại `rust_resolve_calls_in_node` (2837) tạo mutual recursion.
- **Fix**: thêm `walk_depth` cap (512) vào `rust_resolve_calls_in_node` và `rust_walk_macro_tokens`.

### 3.4 Python attribute/field lookup: depth cap nhưng không visited set → phân rã mũ — Medium
`py_lsp.c:1041-1068` (`py_lookup_attribute_depth`) và 1129+ (`py_lookup_field_depth`): mỗi mức đệ quy rẽ qua `alias_of` + mọi `embedded_types[i]`; depth cap 16 nhưng không nhớ node đã thăm → diamond/multiple-inheritance bị duyệt lặp theo cấp số nhân (worst case k^16; thực tế 3^16 ≈ 4·10⁷ lookup cho hierarchy 3-base đủ sâu). So sánh: `php_lsp.c:364-384` và `kotlin_lsp.c:1311-1335` đều có visited set.
- **Fix**: thêm visited-set (như php/kotlin) hoặc memo (type_qn, member) → kết quả.

### 3.5 Scope lookup tuyến tính — Low (perf)
`scope.c:91-137`: lookup/contains/update đều quét tuyến tính chunk×binding×parent-chain. Hàm có K locals → mỗi identifier O(K) → O(N·K) per function. Chunk 16 giúp hằng số nhưng không đổi bậc.
- **Fix**: hash map per scope frame (hoặc intern string + so pointer) nếu profiling cho thấy hot.

### 3.6 Hash/collision handling — nhìn chung TỐT
- `type_registry.c`: chained hash + verify đầy đủ bằng `strcmp` trong `lookup_method_self` (483-495); post-finalize tail scan đúng (499+). `read_only` seal chống mutation race (439-443) — thiết kế tốt.
- `lsp_neg_memo.h`: `cbm_negmemo_key` đảm bảo key ≠ 0 (dòng 73); grow-by-rehash đúng.
- Lưu ý nhỏ: các iterator candidate (`cbm_type_embed_iter_next` 274-291, `cbm_type_short_iter_next` 349-363, `cbm_free_func_iter_next` 313-327) chỉ so **hash 64-bit**, không strcmp — collision lý thuyết sinh candidate sai; caller phải tự verify. Cần kiểm chứng thêm rằng mọi call-site đều verify lại.

### 3.7 Một số điểm tốt đã verify (không phải bug)
- Walk-depth guard 512 + env override đã có ở go/php/py/kotlin/java/perl/c (7/10 ngôn ngữ).
- `CBM_LSP_MAX_LOOKUP_DEPTH 16` cho alias/MRO chain (scope.h:36).
- c_lsp có eval step budget 10000/file (c_lsp.c:1486) và negative memo đúng chuẩn "direct lookup trước, memo sau" (2723-2750).
- Ý thức rõ về race với shared registry (c_lsp.c:4873-4895 — tránh ghi con trỏ arena per-file vào registry shared).
- `lsp_node_iter.h`, `wd_collect_children`, `ts_nstack_push_children` đã giải quyết đúng bẫy `ts_node_child(i)` O(n²).

---

## 4. Performance (hot path)

| # | Vị trí | Vấn đề | Mức |
|---|--------|--------|-----|
| P1 | extract_unified.c:1613/1804 | Quét chồng lấp subtree YAML/JSON mỗi node → O(n·d) | Medium |
| P2 | cbm.c:1562-1617 | O(defs×calls) + strcmp inner loop | Low/Medium |
| P3 | py_lsp.c:1041+ | Lookup thuộc tính phân rã mũ trên diamond | Medium |
| P4 | extract_unified.c:1997/2030 | Sibling scan O(n²) NASM/ObjectScript | Low |
| P5 | scope.c | Scope lookup tuyến tính | Low |
| P6 | ac.c:255-267 | TLS decomp buffer giữ RAM bằng file lớn nhất tới khi thread chết | Low (by design, nên có cap) |

---

## 5. Kiến trúc (coupling extraction ↔ grammars)

### A1. QN computation bị nhân bản giữa hai walk — Medium
`extract_unified.c:731-977` (`compute_func_qn`/`compute_class_qn`) phải **mirror** `extract_defs.c` cho từng ngôn ngữ; chính comment thừa nhận lệch 1 segment là rớt edge (dòng 875, 928-931: "these are two separate functions, and a one-segment disagreement between them drops every CALLS edge"). Các special-case (Wolfram/Lisp/Elixir/CFML/ObjC/Dart/Agda/Nix/Rust impl...) được hardcode song song ở 2 nơi.
- **Fix**: một resolver QN duy nhất dùng chung cho cả def walk và unified walk (đã có sẵn `cbm_resolve_func_name` dùng chung ở cuối `compute_func_qn` — nâng tất cả special-case vào đó).

### A2. Dispatch bằng chuỗi node-kind hardcode per-language rải khắp nơi
Mỗi ngôn ngữ mới cần sửa: `lang_specs.c` (bảng kind) + special-case trong `compute_func_qn`/`compute_class_qn` + import boundary (`is_actual_import_boundary`, extract_unified.c:1880-1971) + có khi cả LSP riêng. `is_actual_import_boundary` là switch khổng lồ per-language — nên đưa vào `CBMLangSpec` dưới dạng callback/bảng để giảm coupling.
- **Fix**: mở rộng `CBMLangSpec` với hook `resolve_qn(node, ctx)` và bảng import-predicate; di chuyển dần special-case vào per-language table.

### A3. Dead code: `internal/cbm/arena.c/h` (xem 1.1) — xóa để tránh drift.

---

## Bảng tổng kết theo severity

| Severity | Số lượng | Mục |
|----------|---------|-----|
| Critical | 0 | — |
| High | 4 | 2.1 (Perl mark_done), 3.1 (C# walk depth), 3.2 (TS walk depth), 3.3 (Rust depth vs step cap) |
| Medium | 7 | 1.1 (dead arena + guard), 1.3 (node_text clamp), 1.5 (AC output_list overwrite), 2.2 (pp timeout), 2.3 (YAML rescan), 3.4 (py lookup mũ), A1 (QN nhân bản) |
| Low | 9 | 1.2, 1.4, 2.4, 2.5, 2.6, 2.7, 2.8, 2.9, 3.5/P6 |

## 5 khuyến nghị quan trọng nhất

1. **Thêm `cbm_index_mark_done(rel_path)` vào đường return Perl** (cbm.c:1246) — một dòng, sửa bug vận hành thật có thể quarantine oan file.
2. **Chuẩn hóa walk-depth guard 512 cho C# / TS / Rust** (3 walker duy nhất còn thiếu) — đây là các lỗ hổng crash (SIGSEGV stack overflow) còn mở, đặc biệt nặng trên Windows stack 1MB. Với Rust: step cap 200000 phải đi kèm depth cap, không thay thế được.
3. **Xóa `internal/cbm/arena.c/h`** và đổi mọi include sang `"foundation/arena.h"` — loại bỏ cặp include guard trùng `CBM_ARENA_H` đang là bom hẹn giờ ODR.
4. **Hợp nhất QN resolution** giữa def walk và unified walk (A1) — nguồn gốc đã được ghi nhận của nhiều bug rớt edge (#554/#621); mỗi special-case thêm vào một bên mà quên bên kia là một bug mới.
5. **Thêm visited-set cho Python attribute/field lookup** (3.4) và **sửa YAML rescan** (2.3) — hai điểm nghẽn perf có khả năng kích hoạt cao nhất trên repo thật (multiple inheritance phổ biến; YAML lồng nhau là chuyện thường ở K8s/CI config).
