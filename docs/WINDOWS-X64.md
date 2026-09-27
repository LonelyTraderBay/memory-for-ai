# Phạm vi Windows native x64

Quyết định ngày 2026-09-26: sản phẩm chỉ hỗ trợ Windows native x64
(`windows-amd64`). Không phát hành Linux, macOS, Windows ARM64 hoặc binary
chạy trong WSL. MSYS2 CLANG64 là toolchain chuẩn; Bash trong MSYS2 chỉ là
công cụ build, không phải runtime Linux của sản phẩm.

## Hợp đồng chuyển đổi

Objects: process supervisor, daemon IPC, private file lock, platform helpers.
States: giữ nguyên trạng thái Windows hiện có; không thêm trạng thái mới.
Inputs: giữ nguyên API nội bộ, MCP, schema SQLite và cấu hình người dùng.
Events: build, test, đóng gói, cài đặt và cập nhật trên Windows x64.
Transitions:
- RULE-001: host và compiler Windows x64 hợp lệ thì tiếp tục build/test.
- RULE-002: target khác Windows x64 bị từ chối trước build/download/install.
- RULE-003: release chỉ được đóng gói sau khi xác minh đủ ba candidate của
  windows-amd64 và chọn đúng bytes theo chính sách VirusTotal hiện có.
Invariants: không đổi ownership, ACL, khóa, timeout, backup, rollback hoặc
  error contract của backend Windows; không đổi thư viện vendored.
Side effects: chỉ build/test fixtures và release artifacts; không di chuyển
  hoặc sửa database, cache và cấu hình người dùng đang sử dụng.
Failure mode: dừng có lỗi; không hạ compiler warnings hoặc bỏ test để báo xanh.
Tests: native suites, MCP/edit integration, Windows product guards, UI,
  package wrappers, release hash/selection và từ chối target không hỗ trợ.

## Kiểm chứng

Toolchain duy nhất: MSYS2 CLANG64 (Clang/LLVM, UCRT, x86_64-w64-windows-gnu).
Node.js phục vụ kiểm tra UI/npm; Go dùng phiên bản trong `pkg/go/go.mod`.

```powershell
./scripts/setup-windows-toolchain.ps1
./scripts/verify-windows.ps1
```

Lệnh thứ hai chạy metadata, native/contracts, UI/browser, package wrappers,
product guards và lint. `-Phase` chỉ chọn một chặng CI; `-Suites` là vòng lặp
native có phạm vi hẹp, không thay thế lần kiểm tra đầy đủ. `-NoSanitizer`
phải được yêu cầu rõ ràng và chỉ chứng nhận hành vi không sanitizer.
Thời gian từng phase được ghi vào `build/windows-x64-<Phase>-verification.json`.
Trên cùng tài khoản Windows, chạy các phase tuần tự: native và product guards
cùng kiểm tra daemon cấp tài khoản nên chạy đồng thời có thể gây từ chối cohort.
Các job CI tách máy chạy độc lập; `-Phase All` đã thực hiện tuần tự.

CI có thể dùng Linux cho xử lý văn bản, giấy phép và artifact; những job đó
không chứng nhận khả năng chạy sản phẩm trên Linux.

Windows CLANG64 chạy ASan/UBSan khi runtime sẵn có. TSan/MSan của backend
POSIX bị loại khỏi phạm vi chuyển đổi; không tuyên bố các kiểm tra Windows
thay thế được chúng. Mọi lần chạy không sanitizer phải được ghi rõ.

## Đo lợi ích

Baseline nguồn: HEAD 698204d8 cùng 10 file đã staged trước chuyển đổi.
Release targets trước: 8; sau: 1. Ba candidate của Windows x64 vẫn được hash,
kiểm tra VirusTotal và chọn trước khi đóng gói ZIP/MCPB. Đây là số cấu hình
phát hành, không phải tỷ lệ giảm thời gian hay chi phí.

| Backend chính | Dòng trước | Dòng sau | Median compile trước/sau (giây) |
|---|---:|---:|---:|
| daemon IPC | 6.465 | 3.398 | 0,457 / 0,449 |
| process | 1.474 | 909 | 0,238 / 0,235 |
| private file lock | 1.627 | 1.029 | 0,280 / 0,265 |
| platform helper | 542 | 359 | 0,210 / 0,208 |

Tổng bốn file giảm 4.413 dòng, khoảng 43,7%. Số liệu compile là median ba
lần biên dịch riêng từng translation unit với Clang 22, `-O2`, cùng include
và flags; đo ngay sau tách backend, trước khi bỏ thêm bốn dòng fallback
không thể chạy tới trong platform helper. Máy còn chạy các kiểm tra khác.
Chênh lệch nhỏ này chưa chứng minh tăng tốc build toàn dự án hay runtime.

Trước khi bỏ fallback thừa, so sánh token C sau preprocessing Windows cho
bốn file khớp nhau, sau khi bỏ qua khoảng trắng và ba khai báo hàm ACL Darwin
không có caller Windows. Đây là bằng chứng bổ sung, không thay cho test.

Thời gian CI thực tế trước/sau và số giờ xử lý lỗi: **chưa đo**. Sau khi CI
chạy trên cùng loại runner, so sánh median các lần cùng cache/flags/suite,
tách queue time khỏi job time; ghi lỗi theo toolchain, nền tảng và logic.
Không suy diễn tám target xuống một tương đương giảm 87,5% thời gian CI.

## Điểm dừng thu gọn

Đã bỏ backend POSIX trong process, IPC, private file lock và nhóm platform
helpers; giữ interface, ownership và lỗi của Windows. Các caller cần ACL
Darwin trong activation/diagnostics đã được thu gọn cùng thay đổi.

Chưa xóa mọi nhánh tiền xử lý hoặc công cụ lịch sử trong repository. Phần
portable nhỏ dùng riêng cho fuzz helper vẫn được giữ; source parser vẫn
phân tích đầy đủ các ngôn ngữ, kể cả code dành cho Linux/macOS. Không sửa
vendored để ép chỉ còn Win32. Chỉ tiếp tục xóa khi một phần cụ thể còn tạo
chi phí bảo trì hoặc lỗi có thể chứng minh.

## Chặn chất lượng phát hiện khi chuyển toolchain

Memory gate cũ không đọc được đường dẫn ổ đĩa Windows và chẩn đoán có hậu tố
`,-warnings-as-errors`; pipeline còn có thể che exit code lỗi của analyzer.
Đã sửa parser, chuẩn hóa đường dẫn và giữ exit code qua `pipefail`. Ca kiểm
tra chẩn đoán giả xác nhận gate từ chối cả hai dạng thông báo; probe analyzer
không tồn tại cũng phải trả lỗi. Build C vẫn giữ `-Wall -Wextra -Werror`.

Sau sửa, các kết luận "memory gate clean" từ parser cũ không được dùng làm
bằng chứng. Lần chạy analyzer đầy đủ bằng Clang 22 hiện báo 13 chẩn đoán
ngoài vendored (sáu chẩn đoán ở lượt khảo sát đầu chưa phải danh sách đầy đủ):

| Vị trí | Chẩn đoán cần xử lý |
|---|---|
| `src/cypher/cypher.c:1816` | Nhánh parse lỗi có thể bỏ mất vùng nhớ `item.args`. |
| `src/cypher/cypher.c:3028` | Analyzer thấy phần tử row chưa khởi tạo; cần đối chiếu bất biến số cột ở caller. |
| `src/cypher/cypher.c:4271` | Cấp phát virtual binding trước khi kiểm tra giới hạn số biến. |
| `src/mcp/mcp.c:2192,2609,8537,9299,14501` | Năm đường dereference NULL cần đối chiếu tiền điều kiện caller và ownership. |
| `src/cli/hook_augment.c:1090` | Cần xác minh dữ liệu token trước khi đọc ký tự. |
| `src/cli/cli.c:2009,12042` | Hai đường đọc `basename`/`argv` có thể NULL theo analyzer. |
| `internal/cbm/ac.c:478` | Cần chứng minh output-list không truy cập `seen_extra` khi không cấp phát. |
| `internal/cbm/sqlite_writer.c:699` | Cần xác minh bất biến leaf count trước khi đọc page đầu. |

Các vị trí này không thuộc phần backend đã thu gọn; `mcp.c` có thay đổi đã
staged từ trước, được giữ riêng. Đây là chẩn đoán cần triage,
không phải 13 lỗi runtime đã được tái hiện. Không thêm suppression hoặc
sửa thuật toán ngoài phạm vi để làm gate xanh. Khi gate còn đỏ, thay đổi
chưa đủ điều kiện hợp nhất/phát hành.

## Kết quả kiểm tra ngày 2026-09-26

- Native không sanitizer: 144 suite, 7.808 passed, 0 failed, 65 skipped.
- Native ASan/UBSan: 144 suite, 7.808 passed, 0 failed, 65 skipped. Linker
  báo duplicate symbol từ CRT/vendored ở test runner và product binary;
  không thêm suppression. Phase Native đầy đủ kết thúc exit 0 trong 1.282,7
  giây, gồm compile, contracts và tests; đây không phải thời gian compile thuần.
  Worker error-response, retrieval 3/3 và security-string checks passed.
  Ba shell guard parent-watchdog, worker-watchdog và watcher-disabled tự skip
  trên Windows; không coi kết quả này là coverage của ba guard đó.
- UI: 59 tests, build và một browser smoke WebGL passed. Còn cảnh báo bundle
  Three.js lớn hơn 500 kB; chưa đổi cấu trúc UI trong đợt này.
- Go/npm/PyPI wrappers passed; PyPI có 28 tests.
- Sáu contract Windows x64, metadata (23 tool / 162 ngôn ngữ), release hash /
  candidate selection / bundle contracts passed. Symlink fixture bị giới hạn
  quyền Windows; CI artifact lane Linux giữ kiểm tra này.
- 23 workflow YAML được đọc thành công, dependency job và input của reusable
  workflows khớp. Chưa chạy GitHub Actions cho diff này.
- Cppcheck, clang-format và no-skips passed; memory analyzer gate **failed**
  với 13 chẩn đoán ở trên. Lệnh kiểm tra đầy đủ chưa xanh.
