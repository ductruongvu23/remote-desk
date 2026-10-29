# Remote Desk

Điều khiển PC Windows từ iPhone, kiểu Moonlight: hình mã hoá bằng card đồ hoạ (HEVC/H.264), độ trễ thấp, keymap cho game.

**Tải bản dựng sẵn ở mục [Releases](../../releases)**, không cần build.

## Gồm những gì

| Phần | Mô tả |
|---|---|
| `remote_host.cpp` | App máy chủ trên PC (C++ thuần Win32, không thư viện ngoài). Chụp màn hình DXGI, mã hoá NVENC/QSV qua Media Foundation, WebSocket qua TLS (Schannel), ghép iPhone bằng mã QR, đăng nhập bằng tài khoản (PBKDF2 + khoá thiết bị), bitrate tự điều chỉnh, mở cổng router bằng UPnP, tên miền riêng (DDNS). Giao diện kiểu Apple, sáng/tối theo Windows. |
| `ios/` | App iPhone (SwiftUI, iOS 17+). Giải mã phần cứng, quét mã QR trên PC, tự tìm PC trong mạng, tự chọn đường LAN / Internet. Keymap có sẵn cho Minecraft, CS2, Valorant, PUBG, Genshin, Star Rail, ZZZ, Wuthering Waves, Roblox, GTA V, MOBA (cần di chuyển, liên thanh, kéo để ngắm, con quay, nút "Chuột" bấm tại chỗ). Thanh công cụ và thông số ẩn hoặc làm mờ được. |
| `remote_desktop.html` | Bản cho Safari (WebCodecs), PC tự phục vụ ở `https://<IP PC>:8443`. |

## Dùng lần đầu

1. Trên PC: chạy bộ cài `RemoteDesk-Windows-*-Setup.exe` (không cần quyền quản trị; hoặc giải nén bản zip), mở Remote Desk, tạo tài khoản, cho phép qua Windows Firewall.
2. Trên iPhone: cài `RemoteDesk-iOS-*-unsigned.ipa` bằng Sideloadly (Apple ID của bạn).
3. Trên PC bấm **Ghép bằng mã QR**, trên iPhone bấm **Quét mã QR trên PC** (hoặc quét bằng app Camera). Không cần nhập IP hay mật khẩu. Lần sau app tự vào, ở nhà hay ở ngoài.

## Dùng ở ngoài

- Bật **Từ Internet** trên cửa sổ PC. Router/modem phải chuyển cổng TCP 8443 về PC (PC tự mở cổng trên router có UPnP; modem nhà mạng thường phải làm tay một lần).
- Đặt **Tên miền** (DDNS) để IP nhà đổi vẫn vào được. Tạo miễn phí ở [duckdns.org](https://www.duckdns.org), điền tên + token, PC tự cập nhật. Modem đã có DDNS thì chỉ cần điền tên.
- Ở xa và chưa ghép bằng mã QR: trên iPhone bấm **Thêm bằng địa chỉ**, nhập tên miền và tài khoản của PC.

## Tự build

- PC (MSYS2 UCRT64, `pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-nsis`): `bash build_host.sh`, kiểm tra bằng `remote_host.exe --self-test`; bộ cài: `makensis -DVERSION=1.2.3 installer.nsi`.
- iPhone (Mac có Xcode 16+): `bash ios/build.sh` ra IPA chưa ký; `bash ios/build.sh --test` chạy self-test trên Simulator. Hoặc mở `ios/RemoteDesk.xcodeproj`, chọn Team rồi Run.
- Phát hành: `git tag v1.2.3 && git push origin v1.2.3`. GitHub Actions build cả hai và tạo Release.
