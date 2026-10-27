# Remote Desk

Điều khiển PC Windows từ iPhone, kiểu Moonlight: hình mã hoá bằng card đồ hoạ (HEVC/H.264), độ trễ thấp, keymap cho game.

**Tải bản dựng sẵn ở mục [Releases](../../releases)** — không cần build.

## Gồm những gì

| Phần | Mô tả |
|---|---|
| `remote_host.cpp` | App máy chủ trên PC (C++ thuần Win32, không thư viện ngoài). Chụp màn hình DXGI, mã hoá NVENC/QSV qua Media Foundation, WebSocket qua TLS (Schannel), đăng nhập bằng tài khoản (PBKDF2 + khoá thiết bị), bitrate tự điều chỉnh, mở cổng router bằng UPnP. Giao diện kiểu Apple, sáng/tối theo Windows. |
| `ios/` | App iPhone (SwiftUI, iOS 17+). Giải mã phần cứng, tự tìm PC trong mạng, keymap theo game (cần di chuyển, liên thanh, kéo để ngắm, con quay), tự chọn đường LAN / IP công khai. |
| `remote_desktop.html` | Bản cho Safari (WebCodecs), PC tự phục vụ ở `https://<IP PC>:8443`. |

## Dùng lần đầu

1. Trên PC: giải nén `RemoteDesk-Windows-*.zip`, chạy `remote_host.exe`, tạo tài khoản, cho phép qua Windows Firewall.
2. Trên iPhone: cài `RemoteDesk-iOS-*-unsigned.ipa` bằng Sideloadly (Apple ID của bạn). Cùng Wi‑Fi với PC, mở app, chọn PC, đăng nhập. Lần sau app tự vào.
3. Dùng ở ngoài: bật "Từ Internet" trên cửa sổ PC; router/modem phải chuyển cổng TCP 8443 về PC.

## Tự build

- PC (MSYS2 UCRT64, `pacman -S mingw-w64-ucrt-x86_64-gcc`): `bash build_host.sh`, kiểm tra bằng `remote_host.exe --self-test`.
- iPhone (Mac có Xcode 16+): `bash ios/build.sh` ra IPA chưa ký; `bash ios/build.sh --test` chạy self-test trên Simulator. Hoặc mở `ios/RemoteDesk.xcodeproj`, chọn Team rồi Run.
- Phát hành: `git tag v1.2.3 && git push origin v1.2.3` — GitHub Actions build cả hai và tạo Release.
