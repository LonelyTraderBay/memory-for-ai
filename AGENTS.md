# Quy định cho AI khi phát triển `memory-for-ai`

## Phạm vi nền tảng

Chỉ hỗ trợ Windows native x64 (`windows-amd64`), dùng MSYS2 CLANG64.
Không phát hành Linux/macOS/Windows ARM64/WSL. Lõi C, SQLite, MCP và dữ liệu
người dùng giữ nguyên hợp đồng. Các job Linux chỉ xử lý source/artifact hoặc
fuzz helper độc lập, không chứng nhận sản phẩm trên Linux.

## 1. Mục tiêu ưu tiên

Khi thiết kế hoặc sửa code, luôn ưu tiên theo thứ tự:

1. Đúng với yêu cầu thực tế và các bất biến của hệ thống.
2. Đơn giản, dễ hiểu và ít trạng thái nhất.
3. Nhanh đủ theo nhu cầu thực tế.
4. Ổn định, an toàn và dễ khôi phục khi lỗi.
5. Dễ kiểm thử, quan sát và bảo trì.

“Đơn giản” không có nghĩa là ít dòng code nhất. Một thiết kế đơn giản là thiết kế có ít trạng thái, ít nhánh xử lý, ít thành phần, ít kết nối và ít điểm có thể sai nhưng vẫn đáp ứng đầy đủ yêu cầu.

Không được thêm kiến trúc, abstraction, cache, retry, queue, state hoặc dependency chỉ vì “sau này có thể cần”. Mọi thành phần mới phải có lý do hiện tại, tác động rõ ràng và cách kiểm thử.

## 2. Trước khi viết code

AI phải:

1. Đọc yêu cầu và xác định kết quả người dùng thật sự cần.
2. Khảo sát code, test, luồng gọi và giới hạn của module liên quan.
3. Xác định các đối tượng, dữ liệu vào/ra, trạng thái thật sự cần, sự kiện, bất biến và side effect.
4. Phân biệt lỗi nằm ở yêu cầu, giả định, thiết kế, code hay test.
5. Chọn phạm vi nhỏ nhất có thể sửa an toàn; chỉ mở rộng khi có bằng chứng lỗi lan xa.

Nếu có nhiều cách hiểu làm thay đổi hành vi, phải nêu giả định và dừng ở mức phân tích trước khi viết code.

## 3. Quy tắc thiết kế trạng thái

- Chỉ tạo trạng thái khi nó làm thay đổi cách hệ thống hoạt động.
- Gộp các trạng thái có cùng cách xử lý.
- Không lưu trạng thái có thể suy ra đáng tin cậy từ dữ liệu khác.
- Không biến mọi điều kiện hoặc lỗi kỹ thuật thành trạng thái nghiệp vụ riêng.
- Chỉ tạo `pending`, `waiting`, `retrying` hoặc tương tự khi nó có cách xử lý riêng.
- Mỗi đối tượng phải giữ đủ dữ liệu để tự quyết định bước tiếp theo của nó.
- Chỉ một nơi được phép thay đổi trạng thái của một đối tượng; nơi khác chỉ gửi dữ liệu hoặc sự kiện.
- Lỗi hiếm nhưng nguy hiểm phải được phát hiện và dừng an toàn.
- Lỗi hiếm và ít nguy hiểm nên được báo rõ để con người xử lý thay vì làm hệ thống phức tạp hơn.

Trước khi thêm trạng thái, hãy thử xóa nó, gộp nó, biểu diễn bằng dữ liệu/điều kiện hoặc chuyển việc xử lý ra ngoài. Nếu hệ thống vẫn đúng thì không giữ thiết kế phức tạp hơn.

## 4. Tương tác giữa các đối tượng

- Ưu tiên giao tiếp giữa các đối tượng gần nhau.
- Một thay đổi ở A trước hết chỉ kiểm tra B và các đối tượng nối trực tiếp với A.
- Không để A phải biết toàn bộ chuỗi B → C → D.
- Nếu A nối B và B nối C, A gửi thông tin cho B; B chịu trách nhiệm xử lý C.
- Với mỗi cặp A/B phải xác định rõ B giữ nguyên trạng thái, đổi trạng thái, từ chối hay chuyển cho con người.
- Không để một sự kiện đi qua nhiều lớp chỉ để thay đổi một đối tượng ở cuối chuỗi.
- Nếu thay đổi nhỏ buộc phải hiểu nhiều module xa nhau, phải xem lại thiết kế.
- Kiểm tra vòng lặp A → B → C → A, bảo đảm có điểm dừng và không xử lý vô hạn.
- Khi nhiều tác động đến B, B vẫn là nơi duy nhất quyết định trạng thái và phải có thứ tự xử lý rõ ràng.

## 5. Luồng chính và tác vụ phụ

- Mỗi tính năng phải có một luồng chính ngắn, thể hiện rõ nghiệp vụ đang đi đến bước nào.
- Chỉ đưa vào luồng chính những việc quyết định bước tiếp theo.
- Thông báo, thống kê, audit log, lịch sử và tác vụ không quyết định kết quả chính nên phản ứng theo sự kiện hoặc được tách ra.
- Nếu một tác vụ chưa hoàn thành mà luồng chính vẫn được phép tiếp tục, tác vụ đó thường không thuộc luồng chính.
- Không để tác vụ phụ âm thầm làm thay đổi kết quả chính.

## 6. Dữ liệu và dịch vụ bên ngoài

- Chỉ truyền lượng thông tin tối thiểu cần cho quyết định kế tiếp.
- Không sao chép toàn bộ dữ liệu của đối tượng khác nếu chỉ cần vài trường.
- Không truyền dữ liệu toàn cục vào mọi đối tượng.
- Thông tin toàn cục nên là input/điều kiện; chỉ lưu phần thật sự cần cho quyết định về sau.
- Không dùng giá trị hiện tại để giải thích quyết định trong quá khứ; hãy lưu snapshot/input tại thời điểm quyết định.
- Dịch vụ bên ngoài chỉ cần được mô hình hóa bằng những gì hệ thống quan sát được, không mô phỏng toàn bộ dịch vụ.
- Gom các lỗi kỹ thuật có cùng cách xử lý thành một dạng thông tin chung; không tạo hàng chục trạng thái cho timeout, mất mạng, quá tải hoặc phản hồi chậm.
- Tách logic quyết định khỏi file system, SQLite, mạng, process, thread và UI. Logic nhận input và trả kết quả/trạng thái; lớp khác thực hiện side effect.

## 7. Quy tắc viết code

- Code phải phản ánh đúng thiết kế; không âm thầm thêm state, nhánh hoặc hành vi ngoài thiết kế.
- Ưu tiên thay đổi nhỏ, cục bộ và dễ review.
- Không refactor rộng hoặc sửa phần không liên quan để giải quyết lỗi cục bộ.
- Không che giấu lỗi bằng return im lặng, giá trị mặc định tùy tiện, retry vô hạn hoặc bỏ qua kết quả lỗi.
- Không sửa test chỉ để làm build xanh; nếu test sai phải giải thích và cập nhật hợp đồng liên quan.
- Không dùng global mutable state nếu có thể truyền dữ liệu rõ ràng qua interface gần nhất.
- Ownership, vòng đời, cleanup, rollback và khả năng gọi lại phải rõ ràng, đặc biệt trong C/C++.
- Mọi đường lỗi phải giữ dữ liệu nhất quán; thao tác nhiều bước phải có rollback, backup hoặc trạng thái `PARTIAL` được báo rõ.

### Quy tắc riêng cho repository

- Giữ tương thích với kiến trúc C native, SQLite, MCP, daemon, pipeline, graph store và watcher hiện có.
- Khi sửa graph/index, kiểm tra cả dữ liệu mới, dữ liệu cũ, incremental update và full rebuild nếu bị ảnh hưởng.
- Khi sửa MCP, giữ đúng JSON-RPC/MCP contract, input schema, error contract và hành vi dry-run/mutation.
- Khi sửa edit/mutation tool, phải có plan, kiểm tra drift/mtime, backup, cleanup khi lỗi và re-index/verify sau khi ghi.
- Khi sửa C/C++, phải kiểm tra NULL, overflow, buffer, ownership, double free, use-after-free, race, deadlock và rollback.
- Khi sửa UI, phải kiểm tra reload, đổi project, click nhanh, request cũ trả sau request mới, GPU resource và filter/search.
- Không thay đổi database, file cấu hình, dữ liệu người dùng hoặc worktree ngoài phạm vi yêu cầu.

## 8. Thiết kế phải đọc được bằng máy

Với tính năng có nhiều trạng thái, trước khi code phải ghi mô hình ngắn theo mẫu:

```text
Objects:       các đối tượng liên quan
States:        chỉ trạng thái làm thay đổi hành vi
Inputs:        dữ liệu đầu vào cần thiết
Events:        sự kiện làm thay đổi trạng thái
Transitions:   RULE-001, RULE-002, ...
Invariants:    điều luôn phải đúng
Side effects:  file/DB/network/process/thread/UI
Failure mode:  giữ nguyên, rollback, partial, retry hữu hạn hay dừng
Tests:         trạng thái, transition, lỗi và bất biến cần kiểm tra
```

Mỗi transition phải có mã riêng, điều kiện đúng/sai, dữ liệu được lưu và việc cần làm sau transition.

## 9. Vòng kiểm chứng thiết kế và code

Thiết kế ban đầu chỉ là giả thuyết. AI phải chủ động thử chứng minh nó sai bằng cách xóa trạng thái, gộp trạng thái, bỏ thành phần, bỏ tự động hóa và rút ngắn luồng nhiều nhánh. Sau mỗi lần đơn giản hóa phải kiểm tra lại bất biến.

Sau khi viết code, AI phải đọc ngược code như một AI chưa biết thiết kế ban đầu, tự suy ra trạng thái/transition/kết nối/side effect rồi so sánh với thiết kế. Nếu code có state, biến đặc biệt, nhánh ngoại lệ hoặc kết nối không được thiết kế, phải dừng và giải thích. Nếu code chứng minh thiết kế sai, sửa thiết kế trước rồi mới sửa code.

## 10. Kiểm thử bắt buộc

Không chỉ kiểm tra code có chạy qua. Phải kiểm tra:

- mọi trạng thái và transition quan trọng;
- điều kiện đúng và sai;
- retry, duplicate và request lặp lại;
- sự kiện sai thứ tự hoặc đến gần như đồng thời;
- lỗi giữa các bước có side effect;
- dừng chương trình rồi chạy lại;
- request mất phản hồi, timeout, mất mạng, dữ liệu cũ và dữ liệu khác nhau;
- bất biến và quy luật toàn hệ thống quan trọng.

Ưu tiên test theo quy luật và bất biến thay vì chỉ thêm fixture cố định. Cố ý làm sai code bằng cách bỏ điều kiện, đổi trạng thái, bỏ xử lý duplicate hoặc đổi phép so sánh để xác nhận test thật sự bắt được lỗi. Mỗi lỗi thực tế phải trở thành regression test mới.

## 11. Lệnh kiểm tra tối thiểu

AI phải chạy kiểm tra phù hợp và báo rõ kiểm tra nào không chạy được:

```powershell
python scripts/check-product-metadata.py

cd graph-ui
npm.cmd test -- --run
npm.cmd run build
cd ..

# Native: dùng entry point chuẩn khi môi trường hỗ trợ
./scripts/verify-windows.ps1 -Suites "suite-lien-quan"
```

Lệnh đầy đủ chuẩn: `./scripts/verify-windows.ps1`. CLANG64 dùng ASan/UBSan
mặc định. Chỉ dùng `-NoSanitizer` khi cần kiểm tra native không sanitizer và
phải báo rõ giới hạn. TSan/MSan toàn sản phẩm không còn nằm trong ma trận;
không tuyên bố các test Windows thay thế được chúng.

Với thay đổi MCP/edit, tối thiểu chạy các suite `mcp`, `edit` và integration tương ứng. Với thay đổi pipeline/store/index, chạy regression suite của module đó. Không tuyên bố hoàn tất nếu chỉ compile mà chưa kiểm tra hành vi.

## 12. Khi nào cần con người duyệt

AI phải yêu cầu hoặc chờ duyệt khi thay đổi yêu cầu/giả định nghiệp vụ, state model, bất biến chính, public API/MCP contract, database schema, bảo mật, quyền truy cập, tiền/số dư, dependency mới, kiến trúc lớn hoặc hành vi mà test chưa chứng minh được.

## 13. Nguyên tắc cuối cùng

- Ít thành phần hơn nếu vẫn đáp ứng đủ yêu cầu.
- Ít trạng thái hơn nếu không mất khả năng cần thiết.
- Quyết định cục bộ hơn thay vì truy vấn xa.
- Đối tượng tự quản lý trạng thái của chính nó.
- Luồng chính ngắn và rõ.
- Đưa độ phức tạp vào mô hình kiểm thử thay vì code chạy thật nếu có thể.
- Mọi thành phần thêm vào phải giải thích được vì sao cần; thành phần không chứng minh được là cần phải được thử bỏ.
- Code phải phản ánh đúng thiết kế; nếu code hoặc test chứng minh thiết kế sai thì sửa thiết kế trước.

Mục tiêu cuối cùng là hệ thống nhỏ nhất, rõ nhất, dễ hiểu nhất, dễ kiểm thử nhất nhưng vẫn đúng, nhanh, ổn định, an toàn và đáng tin cậy.
## 14. Build va canh bao

- Build chuan phai giu `-Wall -Wextra -Werror`; khong ha muc loi de lam build xanh.
- Xu ly canh bao tai nguyen nhan: sua gioi han, ownership, format, overflow hoac hop dong API; khong dung cast, pragma hoac `-Wno-*` cho code moi de che loi.
- Cac GCC-only suppression da co trong `Makefile.cbm` chi duoc phep cho canh bao false-positive da duoc ghi ro, va khong thay the cho viec sua loi logic. Neu them suppression moi, phai ghi ly do, pham vi compiler va test xac nhan.
- Khi compiler moi phat hien canh bao moi, phai phan loai no la loi that, gioi han phan tich qua bao thu, hoac code sai; khong bo qua im lang.
- Trinh bay ket qua build voi moi thay doi: lenh da chay, suite da chay, canh bao con lai neu moi truong khong the build day du.

## 15. Dong bo tai lieu

- `AGENTS.md` la nguon chuan cho quy tac AI va quy trinh sua code.
- `docs/DEVELOPMENT-STANDARD.md` la ban tom tat cho con nguoi; khong duoc trai voi `AGENTS.md`.
- Khi thay doi MCP tool, schema, CLI, build command, test command, cau truc module, hoac hanh vi public, phai cap nhat tai lieu lien quan trong cung thay doi.
- Kiem tra cac con so tool/language/test trong `README.md`, `CONTRIBUTING.md`, `docs/AGENT_GUIDE.md`, `docs/llms.txt` va product metadata; khong de tai lieu cu huong AI hoac nguoi dung den quy trinh sai.
- Tai lieu phai noi ro gioi han, coverage, stale index, dry-run/mutation, rollback va lenh verify; khong dung tu tuyet doi neu chua co test hoac coverage evidence.
