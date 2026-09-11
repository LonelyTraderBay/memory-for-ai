# Báo cáo Review Code C — dự án CBM (memory-for-ai)

Hình thức: review tĩnh thủ công, không sửa code.

## 1. Phạm vi & phương pháp

**Đã review (đọc toàn bộ hoặc phần quyết định):**
- `src/watcher/` — watcher.c (toàn bộ)
- `src/discover/` — discover.c (toàn bộ)
- `src/git/` — git_context.c (toàn bộ)
- `src/daemon/` — http_server.c, httpd.c, daemon.c, service.c, host.c, project_lock.c, application.c (3549 dòng), runtime.c (3199 dòng), version_cohort.c (976 dòng), frontend.c, bootstrap.c, ipc.c (~80%, gồm POSIX publication/stale/accept/connect/probe + Windows security/pipe accept/connect/frame), subprocess.c (toàn bộ)
- `src/foundation/` — lock_registry.c, private_file_lock.c (toàn bộ), mem.c, arena.c, hash_table.c, secure_random.c, compat_thread.c, log.c, str_util.c (validate shell), platform.c (file API)
- `src/traces/` — traces.c
- `src/semantic/` — semantic.c (toàn bộ)

**Nằm ngoài phạm vi theo chốt ban đầu:** vendored/, internal/cbm/, src/mcp, src/cli, src/store, src/cypher, src/pipeline.

**Chưa đọc (được phép bỏ qua):** src/ui/ (config.c, layout3d.c, gitignore.c, language.c, userconfig.c, ast_profile.c, rotsq.c), compat_fs.c, workspace.c, vmem.c, slab_alloc.c, str_intern.c, bootstrap.c đoạn cuối (1001-1031), ipc.c đoạn 4004-4727 và 4911-5505.

**Phương pháp:** đọc trực tiếp mã nguồn, đối chiếu chéo caller/callee, kiểm tra lock ordering, vòng đời đồng bộ hóa, error path, ranh giới OS (POSIX/Windows). Mọi phát hiện liệt kê dưới đây đều đã đọc code xác minh; mục nào chưa chắc chắn được ghi rõ "cần kiểm chứng thêm".

## 2. Nhận định tổng quan

- **Không phát hiện lỗi mức Critical hoặc High.** Codebase đã ở trạng thái rất cứng: nhiều vị trí có comment ghi lại issue đã fix (DNS-rebinding guard, tombstone watcher, fork guard trong lock registry, owner-only ACL trên Windows pipe...), cho thấy đã qua nhiều vòng audit.
- Còn lại **3 Medium** (race khi shutdown log, stat() trần trên Windows với path UTF-8, probe IPC POSIX với ECONNREFUSED) và **17 Low** (chủ yếu error path hiếm gặp, edge case, hoặc vấn đề hiệu năng nhỏ).
- Chủ đề lặp lại đáng chú ý nhất: **sự không nhất quán trong việc dùng wide API trên Windows** — platform.c đã có `cbm_file_exists`/`cbm_file_size` dùng `GetFileAttributesW`, nhưng ít nhất 6 điểm gọi khác dùng `stat()` trần với path UTF-8.

## 3. Phát hiện chi tiết theo nhóm

### 3.1 Concurrency

**[Medium] src/daemon/host.c:115-121 so với 150-165 — race shutdown của log sink**

`host_log_sink` kiểm tra `!g_host_log_file || !g_host_log_mutex_initialized` **trước** khi lock mutex. Trong khi đó `host_log_close` set sink về NULL → lock mutex → `g_host_log_file = NULL` (dòng 156) → `fclose` (158) → unlock → `cbm_mutex_destroy(&g_host_log_mutex)` (162) → `g_host_log_mutex_initialized = false` (163). Một thread đang log vừa vượt qua bước check (115) sẽ lock một mutex đã bị destroy và `fprintf` vào `FILE*` đã đóng.

```c
// host.c:115 — check ngoài lock
if (!g_host_log_file || !g_host_log_mutex_initialized) { return; }
```

Cửa sổ race hẹp và chỉ xảy ra lúc shutdown, nhưng hệ quả là UB thật. **Fix:** lock mutex trước rồi mới check `g_host_log_file` bên trong lock; hoặc đơn giản không destroy mutex / không fclose (để sống cùng process lifetime).

**[Low] src/watcher/watcher.c:1452-1458 — mutation callbacks chạy khi đang giữ coordination_lock**

`project_pruned` / `mutation_end` được gọi trước khi unlock `coordination_lock` (unlock ở 1458). Nếu một callback gọi ngược lại watcher API sẽ deadlock. Lock order coordination→projects đã kiểm tra là nhất quán, nên đây là ràng buộc reentrancy ngầm. **Fix:** gọi callback ngoài lock, hoặc ghi chú rõ ràng buộc "callback không được gọi lại watcher API" tại định nghĩa callback.

**[Low] src/watcher/watcher.c — data race hình thức trên next_poll_ns / missing_root_count**

`poll_project` ghi `next_poll_ns`/`missing_root_count` ngoài `projects_lock`, trong khi `cbm_watcher_touch` (1216-1227) ghi `next_poll_ns` dưới lock. Race trên int64_t; thực tế benign nếu chỉ có một poll thread — giả định "single poll thread" này **chưa verify** trong code. **Fix:** ghi mọi field per-project dưới `projects_lock`, hoặc tài liệu hóa rõ giả định single-poller.

**[Low] src/foundation/mem.c:707-719 — double mutex init trong mem_phase_enabled**

Hai thread cùng thấy state `-1` sẽ cùng gọi `cbm_mutex_init(&g_mem_phase_mutex)` (dòng 715). Chỉ xảy ra khi `CBM_MEM_PHASES=1`. **Fix:** CAS state, hoặc init mutex một lần (pthread_once / InitOnceExecuteOnce).

**[Low] src/semantic/semantic.c:122, 135 — getenv() trần thay vì cbm_safe_getenv**

platform.c tự ghi `getenv` là MT-unsafe và cung cấp `cbm_safe_getenv`, nhưng semantic.c dùng `getenv()` trực tiếp. Thường chỉ gọi lúc init nên rủi ro thấp. **Fix:** đổi sang `cbm_safe_getenv` cho nhất quán.

### 3.2 IPC / daemon

**[Medium] src/daemon/ipc.c:2919-2931, 2959-2961 — probe POSIX coi ECONNREFUSED là "active"**

`cbm_daemon_ipc_endpoint_probe` cố ý coi ECONNREFUSED là active (để xử lý BSD listen-queue-full, fail-closed chống in-place update). Tuy nhiên trên Linux, socket file mồ côi sau crash cũng trả ECONNREFUSED → probe báo active mãi. Stale cleanup dưới startup lock xử lý qua marker record, nên trường hợp livelock startup (khi marker record mất) **cần kiểm chứng thêm**.

```c
// ipc.c:2959-2961
if (socket_error == ECONNREFUSED) {
    return 1;   /* active */
}
```

**Fix:** khi marker record vắng mặt, thử `connect` + handshake thật thay vì fail-closed theo ECONNREFUSED.

**[Low] src/daemon/ipc.c:2861-2872 — accepted socket không CLOEXEC ngay tại accept**

POSIX: `accept()` rồi mới `fd_set_cloexec`; tương tự `local_socket_new` (641-651). Không dùng `accept4(SOCK_CLOEXEC)` → cửa sổ nhỏ fd rò vào subprocess nếu có spawn giữa hai lời gọi. **Fix:** `accept4(..., SOCK_CLOEXEC)` trên Linux, fallback `fd_set_cloexec` ngay sau accept.

**[Low] src/daemon/bootstrap.c:940-956 — grandchild POSIX không reset SIG_IGN dispositions**

Có `sigprocmask` reset mask nhưng **không** reset các disposition SIG_IGN trước `execv` — khác với subprocess.c:1009-1021 (`cbm_posix_reset_child_signals`). Daemon thừa hưởn các signal bị ignore từ process bootstrap (vd SIGPIPE/SIGHUP). **Fix:** tái dùng `cbm_posix_reset_child_signals` trong đường bootstrap.

**[Low] src/foundation/compat_thread.c:147-154 — handle leak khi join fail trên Windows**

`cbm_thread_join` Windows không `CloseHandle` khi `WaitForSingleObject` thất bại → leak handle trên error path. **Fix:** CloseHandle trong mọi nhánh thoát.

### 3.3 Watcher / discover / git

**[Medium] stat() trần trên Windows với path UTF-8 (cluster 6 điểm)**

Không qua wide API, không nhất quán với `cbm_file_exists`/`cbm_file_size` trong platform.c:155-190 (đã dùng `GetFileAttributesW`):

| Vị trí | Ngữ cảnh |
|---|---|
| src/watcher/watcher.c:476 | `git_has_own_dot_git` |
| src/watcher/watcher.c:613 | `sig_fold_path_stat` — path non-ASCII stat fail → mất size/mtime trong signature → edit file dirty có tên non-ASCII không bị phát hiện |
| src/watcher/watcher.c:1244 | `init_baseline` |
| src/git/git_context.c:258 | git context stat |
| src/daemon/application.c:2365 | `set_context` |
| src/daemon/application.c:3387 | `background_index` |

Hậu quả: degrade phát hiện thay đổi trên Windows với tên file/thư mục non-ASCII; **không phải** vấn đề security. **Fix:** một helper wide-API stat thống nhất (ví dụ `cbm_stat_utf8`) và thay toàn bộ.

**[Low] src/watcher/watcher.c:1376-1389 + 1551-1556 — commit stale dirty signature**

Khi HEAD đổi nhưng `git_dirty_signature` trả `COMMAND_FAILED`, code commit `last_dirty_sig = pending_dirty_sig` (giá trị của poll trước) sau reindex → bỏ qua một trạng thái dirty. Edge case, tự khỏi ở poll kế tiếp. **Fix:** khi COMMAND_FAILED sau reindex, giữ nguyên `last_dirty_sig` cũ và để poll sau so sánh lại.

**[Low] src/discover/discover.c:887-898 + walk_push_subdir 862-868 — một path quá dài làm fail toàn repo**

Một path > CBM_SZ_4K set `out->failed = true` → CBM_DISCOVER_ERROR cho toàn repo thay vì skip subtree đó. **Fix:** đếm vào `skipped` và tiếp tục walk.

**[Low] src/discover/discover.c:900-998 — TOCTOU symlink trong walk**

Khoảng giữa `safe_stat` và `cbm_opendir` có thể bị thay symlink (cần attacker local, cửa sổ nhỏ; walk vốn skip symlink cả hai OS). **Fix:** `openat`/`fdopendir` theo thư mục cha nếu muốn khép kín.

**[Low] src/discover/discover.c:587-601 — dir_is_cache_tree case-sensitive**

So sánh tên thư mục case-sensitive trên filesystem case-insensitive (Windows/macOS mặc định) → có thể miss cache dir (informational, hậu quả là index thêm file cache).

**[Low] src/git/git_context.c:46-87 — git_capture: popen shell + buffer 4096**

Path dài bị cắt silently → `worktree_root` sai. `repo_path` đã validate metachar nên **không** có injection; trên Windows path chứa `%!^` bị từ chối. **Fix:** đọc stream thay buffer cố định, hoặc kiểm tra truncation rõ ràng.

### 3.4 HTTP server

**[Low] src/daemon/http_server.c:696-703 — handle_processes: chèn comm từ ps vào JSON không escape**

POSIX: trường `comm` từ `ps` chèn thẳng vào JSON. Chỉ bind localhost + đã có host-header check nên mặt tấn công rất nhỏ, nhưng tên process chứa `"` sẽ làm JSON hỏng. **Fix:** escape chuỗi JSON.

**[Low] src/daemon/http_server.c:753-847 — /api/browse liệt kê thư mục bất kỳ (informational)**

Theo thiết kế (file picker), đã có host-header check + loopback bind. Web page trên máy không gọi được nhờ origin/DNS-rebinding guard, nhưng một process local khác vẫn đọc được listing thư mục qua endpoint này.

### 3.5 Foundation

**[Low] src/foundation/arena.c:63, 67 — integer wrap khi align**

`(n + ARENA_ALIGN) & ~ARENA_ALIGN` wrap khi `n` gần SIZE_MAX → cấp buffer nhỏ cho request khổng lồ. Lý thuyết (cần caller truyền size phi thực tế). **Fix:** check overflow trước khi align-up.

**[Low] src/semantic/semantic.c:633 (và 571) — malloc không check NULL**

`batch_resolve_one_doc`: `int *ids = malloc(...)` không check NULL → `ids[i]` NULL deref khi OOM; cùng pattern ở `add_doc` (571). **Fix:** check NULL và trả lỗi.

*(Các mục mem.c double-init và compat_thread handle leak đã liệt kê ở nhóm Concurrency / IPC.)*

### 3.6 Performance

**[Low] src/daemon/subprocess.c:1044-1046 và bootstrap.c:930-938 — vòng lặp close(fd) tới _SC_OPEN_MAX**

Trong child trước exec, đóng fd bằng vòng lặp tới `_SC_OPEN_MAX` (tới 1.048.576 trên Linux hiện đại) → hàng trăm ms mỗi lần spawn khi RLIMIT_NOFILE cao. **Fix:** `close_range()` (Linux ≥ 5.9), `closefrom()` (BSD), hoặc đọc `/proc/self/fd` và chỉ đóng fd đang mở.

## 4. Đã kiểm chứng là TỐT (không báo bug)

- **httpd.c**: parse header chặt (CRLFCRLF, reject %00, duplicate Content-Length → 400, Transfer-Encoding → 411, body cap).
- **http_server.c**: DNS-rebinding guard (Host header check) + origin checks; `serve_embedded` exact-match, không path traversal.
- **watcher.c**: tombstone `registered` + deferred-free đúng; lock order coordination→projects nhất quán.
- **daemon.c**: batch callback không giữ mutex khi gọi ra ngoài.
- **runtime.c**: worker lifecycle đúng (done flag set trước khi send, reap ngoài mutex).
- **project_lock.c**: khóa SH/EX đúng ngữ nghĩa.
- **log.c**: atomic sink swap.
- **discover.c**: walk iterative, skip symlink cả hai OS.
- **private_file_lock.c + lock_registry.c**: FIFO fair queue, fork guard, tombstone retire đúng.
- **version_cohort.c**: lock order maintenance→admission→lifetime nhất quán.
- **frontend.c**: queue/cancel routing đúng.
- **win_security (ipc.c)**: owner-only ACL + pipe same-user check cả hai chiều.
- **str_util.c**: validate shell arg chặt (git_context không injection được).

## 5. Bảng tổng kết theo severity

| Severity | Số lượng | Hạng mục |
|---|---|---|
| Critical | 0 | — |
| High | 0 | — |
| Medium | 3 | race shutdown log (host.c); stat() trần Windows/UTF-8 (6 điểm); IPC probe ECONNREFUSED POSIX (ipc.c) |
| Low | 17 | reentrancy watcher callback; data race next_poll_ns; double mutex init mem.c; getenv trần; accept thiếu CLOEXEC; SIG_IGN bootstrap; handle leak thread join; stale dirty sig; path dài fail toàn discover; TOCTOU symlink; case-sensitive cache dir; git_capture buffer 4096; JSON không escape; /api/browse informational; arena align wrap; malloc không check NULL (gộp 2 điểm); vòng lặp close 1M fd |

## 6. Khuyến nghị (theo thứ tự ưu tiên)

1. **Thống nhất helper stat wide-API cho Windows** (`cbm_stat_utf8` dùng `GetFileAttributesW`/`_wstat`) và thay toàn bộ 6 điểm `stat()` trần — nhóm có hậu quả thực tế rõ nhất (mất phát hiện thay đổi file tên non-ASCII).
2. **Sửa race shutdown của log sink** trong host.c: lock mutex trước rồi mới check `g_host_log_file`, hoặc không destroy mutex/fclose (process-lifetime resource).
3. **Làm chặt probe IPC POSIX**: khi marker record vắng mặt, không fail-closed theo ECONNREFUSED mà thử handshake thật để phân biệt socket mồ côi sau crash — kèm test tái hiện (phần livelock cần kiểm chứng thêm).
4. **Thay vòng lặp `close(fd)` tới `_SC_OPEN_MAX`** bằng `close_range()`/`closefrom()` hoặc quét `/proc/self/fd` trong subprocess.c và bootstrap.c — giảm độ trễ spawn rõ rệt trên Linux có RLIMIT_NOFILE cao.
5. **Ghi rõ ràng buộc reentrancy cho watcher mutation callbacks** (không được gọi lại watcher API khi đang giữ coordination_lock), hoặc tốt hơn là gọi callback ngoài lock.
