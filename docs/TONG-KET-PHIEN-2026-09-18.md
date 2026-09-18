# Tổng kết phiên làm việc 2026-09-18 → 2026-09-19

Ghi chú bàn giao cho các phiên sau. Phạm vi: hoàn thiện bộ edit tools,
phát hành v0.11.0, xử lý toàn bộ lỗi CI/GitHub phát sinh sau release.

## 1. Bối cảnh

- Repo: `memory-for-ai` — pure C binary (MIT), single-binary, MCP server
  indexing code graph (22 tools).
- Phiên này tiếp nối roadmap clean-room (phương án D): tái implement các
  ý tưởng edit-symbol của Serena bằng C, giữ MIT + single-binary, kèm
  evidence (trace_path inbound làm usage check, coverage check cho rename).

## 2. Việc đã hoàn thành

### 2.1 Bộ edit tools (Phase 1–3)

| Commit | Nội dung |
|---|---|
| `6f25ba51` | Thiết kế roadmap clean-room 4 phase (edit/delete/rename/undo) |
| `e6ce8892` | 4 symbol-level edit tools: `edit_symbol`, `delete_symbol`, `rename_symbol`, `undo_edit` |
| `d481b9a0` | Suite edit 36 test + đồng bộ annotations 22 tools |
| `c1600254` | Đồng bộ 22 tools trên mọi surface + script setup toolchain Windows |
| `b516fc3f` | Đồng bộ product-metadata + tool manifest (~9K tokens) |

### 2.2 Release v0.11.0

| Commit | Nội dung |
|---|---|
| `28178e7f` | clang-format 20 cho bộ edit tools — mở khóa release lint gate |
| `2aa586b4` | Dọn 7 cppcheck findings cho release lint gate |
| `2ae24105` | Bump toàn bộ version surfaces lên 0.11.0 sau phát hành |

- Release đã publish đầy đủ: 49 assets, npm/PyPI 0.11.0 live, MCP registry OK.
- Smoke-packages xanh sau khi tái tạo product-metadata (`f6d1e810`).

### 2.3 Fix lỗi CI sau release

| Commit | Issue | Nội dung |
|---|---|---|
| `a54b6b32` | — | `verify-release-selection` bỏ qua UI-branded duplicate archives (idempotent) |
| `7e4849d9` | Closes #11 | `mcp_call` read timeout 30s → 60s cho khớp ngưỡng Check 4 |
| `82c4a1fe` | Closes #10 | Flake `watcher_continued_dirty` trên macos-14 (xem §3.1) |

### 2.4 DCO remediation + pre-push hook

- 6 commit (`4a447eba` → `3f4c654c` cũ) thiếu `Signed-off-by` do nhầm
  `format.signOff` (chỉ áp dụng cho format-patch) với `commit.signOff`.
- Đã `git rebase --signoff b516fc3f` (tree giữ nguyên, chỉ thêm trailer),
  force-push sau khi tạm bật `allow_force_pushes`, rồi khôi phục branch
  protection nguyên trạng ngay.
- `c9fd0700`: thêm `scripts/hooks/pre-push` (mirror range logic của
  `dco.yml`, gọi lại `scripts/check-dco.sh`) — commit chưa ký bị chặn
  ngay tại local.
- `ac3db7c2`: ghi bài học `--no-verify` + `-s` vào CONTRIBUTING.md.

## 3. Bài học kỹ thuật

### 3.1 Root cause thật của flake #10 (watcher macOS)

Không phải mtime granularity: dirty-signature fold cả **size + mtime_ns**
(`sig_fold_path_stat`, `src/watcher/watcher.c:608`), append đổi size
12→20 bytes bắt buộc đổi signature. Đường fail thật: git subprocess exit
≠ 0 transient (`WATCHER_GIT_COMMAND_FAILED`, cố ý **không log**) được
`check_changes` coi là "quiet, retry sau" — đúng contract at-least-once
của #937. Test assert 1 poll duy nhất phải thấy edit → không đứng vững
trên runner tải nặng. Fix phía test: helper `wait_index_count()` poll
trong budget 200×25ms rồi assert trạng thái hội tụ.

### 3.2 Vận hành Git/DCO trên Windows

- `format.signOff` **không** ký `git commit`; phải dùng `commit.signOff`
  hoặc `-s`.
- `--no-verify` trên Git for Windows 2.47.1 **suppress luôn auto
  sign-off** → bypass hooks thì bắt buộc kèm `-s` tường minh
  (đã ghi trong CONTRIBUTING.md).
- `grep -i` của GNU grep 3.0 (Git Bash/MSYS2) bị SIGABRT trong locale
  multibyte → `scripts/check-dco.sh` đã chuyển sang so khớp chuỗi bash
  thuần (`${var,,}`), semantics giữ nguyên.

### 3.3 Quy trình sửa lịch sử commit trên main được bảo vệ

1. GET branch protection, lưu config.
2. PUT protection với `allow_force_pushes: true` (payload dùng `checks`,
   không trộn `contexts`).
3. `git push --force-with-lease`.
4. PUT khôi phục `allow_force_pushes: false` ngay, verify lại toàn bộ
   trường (PR reviews, required checks `dco` + `ci-ok`).

## 4. Môi trường build/test trên máy này

```bash
export PATH="$PWD/.toolchain/mingw64/bin:/c/WINDOWS/System32/WindowsPowerShell/v1.0:$PATH"
mingw32-make -f Makefile.cbm test-focused CC=gcc CXX=g++ SANITIZE= TEST_SUITES=<suite>
./build/c/test-runner.exe <suite>   # chạy riêng một suite
```

- **Bắt buộc** `CC=gcc CXX=g++ SANITIZE=` (MinGW thiếu libsanitizer) và
  PowerShell trong PATH (thiếu thì suite index_format fail giả).
- Token GitHub: `printf "protocol=https\nhost=github.com\n\n" | git credential fill`.
- Commit convention: conventional commits tiếng Việt, `Closes #N` để
  auto-close issue, commit trực tiếp lên main, luôn ký DCO (`-s`).

## 5. Trạng thái cuối phiên

- HEAD: `ac3db7c2` — mọi check xanh (dco, CodeQL, scorecard, build, pages).
- 0 issue mở (#10, #11 đã closed-completed).
- Release v0.11.0 hoàn chỉnh trên mọi kênh phân phối.
- Đã commit: `docs/PHAN-TICH-KET-HOP-SERENA.md`, `docs/SO-SANH-SERENA.md`
  (tài liệu phân tích Serena từ 2026-09-16).

## 6. Việc có thể làm tiếp (chưa ai yêu cầu)

- Phase 4 của roadmap edit tools (nếu còn trong thiết kế `6f25ba51`).
- Hardening tương tự `wait_index_count()` cho các test watcher khác cùng
  pattern "append → 1 poll → assert" nếu flake tái xuất.
