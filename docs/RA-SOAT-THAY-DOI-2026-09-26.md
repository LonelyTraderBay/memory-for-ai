# Rà soát thay đổi chờ commit — 26/09/2026

Phạm vi ban đầu: 40 file sửa trên nền `284d48a1`. Nhánh
`codex/pending-hardening` giữ riêng lô này với PR #13 (`codex/priority-hardening`).
Không có thay đổi database schema, dependency trực tiếp mới hoặc phát hành binary.

## Thứ tự ưu tiên

1. **P1 — Tính đúng của C và truy vấn:** chặn overflow khi căn chỉnh/grow arena;
   từ chối bộ lọc vượt sức chứa bind thay vì trả kết quả thiếu; kiểm tra NULL khi
   lấy source node; escape tên process trong JSON của UI trên POSIX.
2. **P1 — Windows và tính toàn vẹn index:** đường dẫn Unicode/dài, cleanup ảnh
   executable sau khi thoát, freshness theo timestamp độ phân giải native, và
   lỗi `path_too_long` giữ nguyên generation đã publish. Kiểm thử IPC dùng event
   có tín hiệu rõ thay cho vòng polling.
3. **P2 — Hướng dẫn sử dụng:** graph chỉ mô tả các quan hệ đã ghi nhận; kiểm tra
   coverage/source với callback, dynamic reference và kết luận phủ định. Tách
   chi phí manifest khỏi chi phí truy vấn trong ghi chép đo lường.
4. **P2 — Release và dependency:** thống nhất nhận diện Windows của Makefile,
   dòng LF cho pre-push, hợp đồng release archive/venue và lockfile UI.

## Điểm đã sửa thêm khi review

- Bỏ `strlen(source)` vừa được thêm vào `cbm_node_text`: source được parser nhận
  theo độ dài byte, có thể chứa NUL và không cần NUL kết thúc. Quét cả source ở
  mỗi node vừa sai hợp đồng vừa làm tăng chi phí theo kích thước file. Hàm giữ
  kiểm tra NULL và dùng byte range của cây tương ứng; test xác nhận byte NUL ở
  giữa source vẫn được sao chép. Truyền cây cũ cùng source khác vẫn là sai hợp đồng.
- Bỏ bước `stat()` theo ANSI trước bước đọc metadata UTF-8 trên Windows. Một
  file Unicode tồn tại phải được nhận diện đúng trước khi so mtime/size. Test
  kiểm tra cả metadata khớp và nội dung đã đổi kích thước.
- Định dạng các file C/H trong phạm vi sửa và cập nhật chú thích giới hạn bind.

## Mô hình index được kiểm tra

```text
Objects:       repository, staging database, published generation
States:        staging, published (giữ mô hình hiện có)
Inputs:        cây file hiện tại, mode, giới hạn biểu diễn đường dẫn 4095 byte UTF-8
Events:        khám phá xong hoặc không thể biểu diễn đầy đủ đường dẫn
Transitions:   RULE-001: đường dẫn hợp lệ -> tiếp tục pipeline hiện có
               RULE-002: vượt giới hạn -> trả path_too_long, bỏ staging, giữ published
Invariants:    không publish graph thiếu dữ liệu do cắt cụt đường dẫn
Side effects:  đọc filesystem, tạo/xóa staging; chỉ replace DB sau validation
Failure mode:  dừng, giữ generation trước đó; người dùng rút ngắn đường dẫn rồi chạy lại
Tests:         root quá dài, descendant quá dài, pipeline và MCP giữ symbol cũ
```

Cleanup executable Windows giữ kết quả `DEFERRED` hiện có: helper chạy ẩn chờ
mutex của caller, rồi thử xóa tối đa 40 lần cách nhau 250 ms; nếu không khởi chạy
được helper thì thử đăng ký xóa lúc reboot. `DEFERRED` chỉ xác nhận đã khởi chạy
helper/đăng ký cleanup, không bảo đảm file đã bị xóa. Guard sản phẩm kiểm tra
backup thực sự biến mất sau uninstall trong đường dẫn dài, Unicode và dấu nháy.

## Giới hạn kiểm chứng của năm commit ban đầu

- Native Windows dùng Clang, `SANITIZE=`, giữ `-Wall -Wextra -Werror`.
  Không suy ra ASan/UBSan/TSan hoặc tính đúng của nhánh POSIX từ kết quả này.
- Lint đầy đủ bị chặn bởi thiếu `cppcheck`; `clang-tidy` cũng chưa có trong
  toolchain. `clang-format` toàn repo còn một lỗi có sẵn ở `internal/cbm/ac.c`.
  Các file C/H production thay đổi được kiểm tra riêng; định dạng test ngoài hunk thay đổi được giữ nguyên để tránh diff không liên quan. Không xem việc bỏ pre-commit hook
  là gate đã qua; pre-push DCO vẫn phải chạy.
- Linker có cảnh báo trùng symbol CRT trong grammar vendored dùng cấu hình
  `--allow-multiple-definition` có sẵn; không thêm suppression mới.
- UI còn cảnh báo chunk Three.js lớn hơn 500 kB. Báo cáo A/B trong
  `AB-RESULTS.md` là các lần đo lịch sử với raw artifact cục bộ, không phải kết
  quả đo lại hiệu quả của nhánh này.

## Kết quả đã chạy trước khi tích hợp

- `python scripts/check-product-metadata.py`: qua (23 MCP tools, 162 languages).
- `npm.cmd ci`: audit 329 package, không báo lỗ hổng; có cảnh báo package
  `whatwg-encoding` deprecated và install script esbuild chưa nằm trong allowScripts.
- `npm.cmd test -- --run`: 12 file / 54 test qua.
- `npm.cmd run build`: TypeScript/Vite qua, còn cảnh báo chunk Three.js nêu trên.
- `bash tests/test_venue_parity_contract.sh`: qua (23 workflow marker,
  10 venue workflow, 18 help interface, 8 strict-flag interface).
- `bash tests/test_release_archive_extractor_contract.sh`: qua (14 container,
  56 member associations; từ chối archive thừa/thiếu và MCPB hỏng).
- `bash scripts/security-audit.sh`: qua; vẫn có ghi chú REVIEW về 28 file-read
  operations của MCP so với ngưỡng 15, không phải kết quả quét bảo mật toàn diện.
- Mutation ở bản sao arena trong `build/`: bỏ guard căn chỉnh overflow làm test
  `arena_alloc_rejects_size_overflow` thất bại (31 qua, 1 lỗi); suite arena của
  runner source thật qua 32 test.
- Build native test qua. Build product qua với `OS=windows` khi chạy trong MSYS2
  Bash; gọi Windows PowerShell recipe dưới Bash mà để `OS=Windows_NT` sẽ làm
  Bash mở rộng biến trong recipe. Không dùng kết quả compile để thay thế test.
- `tests/windows/test_non_ascii_path.py` và `test_windows_update_handoff.py`:
  bị Windows Application Control chặn executable bằng `WinError 4551`; chưa
  xác nhận end-to-end cleanup/update. Không đổi chính sách hệ điều hành.

Lệnh native đã dùng (MSYS2 CLANG64, PATH gọn và temp root có ACL riêng theo
`scripts/ci/new-protected-temp-root.ps1`):

```bash
make -j4 -f Makefile.cbm BUILD_DIR=build/review-native CC=clang CXX=clang++ SANITIZE= build/review-native/test-runner
bash scripts/run-tests-parallel.sh build/review-native/test-runner.exe 4
make -j4 -f Makefile.cbm OS=windows BUILD_DIR=build/pending-product CC=clang CXX=clang++ SANITIZE= cbm
```

Kết quả runner: **7.638 test qua, 0 lỗi, 65 bỏ qua theo nền tảng; đủ 144 suite**.
Các nhóm liên quan: arena 32, store_search 75, discover 114, pipeline 260,
pipeline_semantic_manifest_repro 31, extraction 325, activation_transaction 20,
daemon_ipc 34, CLI 272, MCP 250 (9 skip), edit_integration 17. Full/incremental
publication, lỗi descendant quá dài giữ generation cũ và freshness Unicode đều
qua trên native lane. Suite `incremental` riêng không có test trong cấu hình
Windows này; kết quả không chứng nhận các nhánh POSIX/sanitizer.


## Tích hợp hai nhánh trước khi đồng bộ main

Giữ lịch sử chín commit của `codex/priority-hardening` và
`codex/pending-hardening`; tái tạo lockfile từ package.json đã kết hợp.
Các sửa bổ sung được giới hạn ở lỗi build, lint và kiểm thử quan sát được:

- P0: đọc giới hạn số nguyên bằng kiểu đủ rộng trước khi kiểm tra INT_MAX;
  coverage không được biến dòng vượt INT32_MAX thành dòng hợp lệ trên Windows.
  Test kiểm tra giá trị biên, overflow, số âm, số không và chuỗi không hợp lệ.
- P1: recipe POSIX tạo stamp/response file bằng printf để chạy với Make hệ thống
  macOS; blob chỉ đọc dùng chung không phụ thuộc cấu hình test. Kiểm thử incremental
  chạy các target test/test-foundation, kiểm tra no-op, source, header và flags.
  Recipe Windows gọi đích .exe được đặt trong dấu nháy, dùng được cả cmd và MSYS2.
- P1: full flush và incremental merge dùng chung phần ghi node/edge, giữ transaction
  tại caller và giải phóng map ID ở một nơi. Fault injection chỉ tồn tại trong test;
  các test rollback cho cả hai đường vẫn bắt buộc.
- P1: tách bước publish backup và đọc edge thành hàm cục bộ với ownership rõ ràng;
  giữ kiểm tra drift, backup không ghi đè, cleanup và lỗi SQLite. Không thêm trạng thái,
  retry, dependency runtime, public API hay schema database.
- P2: đưa guard lựa chọn binary vào entry point Windows chuẩn, provision Chromium
  qua npm script và dùng full parallel harness trong pre-commit. Không giảm gate,
  ngưỡng cảnh báo hay số suite để làm kiểm tra xanh.

Toolchain lint cục bộ nay có cppcheck 2.21 và clang-tidy/clang-format 22. CI vẫn
chạy các phiên bản đã pin trong workflow. Kết quả CI phải được kiểm tra trên đúng
head SHA của PR trước khi merge, đặc biệt macOS, Windows, Linux và sanitizer.
Kết quả native `SANITIZE=` không thay thế các lane đó. Cảnh báo chunk Three.js và
CRT vendored nêu trên vẫn cần được phân biệt với lỗi kiểm thử.


Ở lượt tích hợp, clang-tidy theo staged diff, cppcheck, clang-format, metadata và
security audit đều qua trong pre-commit. Native đã có một lượt 7.643 qua, 0 lỗi,
65 platform skip / 144 suite trước sửa cleanup I/O cuối cùng. Khi hook rebuild
binary cuối, Windows Application Control chặn chạy với WinError 4551 (đã xác nhận
bằng subprocess trực tiếp); không được coi lượt native trước đó là kiểm chứng
binary mới. Không thay đổi chính sách Windows hoặc sửa binary để né kiểm soát.

Commit tích hợp dùng ngoại lệ `git commit --no-verify -s` mô tả trong
CONTRIBUTING.md để đưa mã lên PR kiểm chứng trên CI. Pre-push DCO vẫn chạy;
chỉ được merge main khi `dco` và `ci-ok` cùng toàn bộ job bắt buộc xanh trên đúng
head SHA. UI hiện qua 59 test, build và 1 browser smoke; coverage dòng 52,99%.
Regression Make đã qua cả cmd/PowerShell và MSYS2; mutation riêng của bộ đọc
coverage qua với mã hiện tại và thất bại khi đổi về kiểu long hẹp trên Windows.

### Sửa lỗi phát hiện trong CI đa nền tảng

- P1: discovery phân biệt `ENAMETOOLONG` của filesystem với lỗi I/O khác, kể cả
  khi đường dẫn chưa vượt buffer nội bộ. Full walk và bounded count cùng trả lỗi
  có kiểu; pipeline/MCP giữ nguyên generation đã publish theo RULE-002 ở trên.
  Fixture đường dẫn hợp lệ lấy giới hạn thực tế từ `pathconf`; test cây quá dài
  vẫn bắt buộc kiểm tra lỗi và dữ liệu cũ, không thêm skip trên macOS.
- P2: entry point native truyền đúng đuôi `.exe` trên Windows cho các consumer
  yêu cầu file tồn tại, bao gồm retrieval smoke. Contract chạy đoạn entry thật
  với artifact Windows/POSIX và thư mục có khoảng trắng.
- P2: test watcher dùng cơ chế chờ hữu hạn đã có theo hợp đồng phát hiện ít nhất
  một lần; cleanup chạy trước assertion để lỗi test không tạo leak giả.
  Test deadline của lock registry giữ nguyên tỷ lệ thời hạn/giới hạn và chuỗi
  đánh thức, tăng cửa sổ quan sát từ 100 ms lên 1 giây để runner macOS bận có thể
  quan sát thread vào queue. Không đổi timeout hay logic khóa trong sản phẩm.
