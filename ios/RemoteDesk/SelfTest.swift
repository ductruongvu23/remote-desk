import CoreMedia
import UIKit
import Foundation

/// Kiểm tra nhanh phần không cần mạng: tách H.264, dựng định dạng từ SPS/PPS thật của NVENC, keymap.
enum SelfTest {
    static func run() -> Never {
        setvbuf(stdout, nil, _IONBF, 0)  // in ngay từng dòng: app có văng thì log CI vẫn thấy tới bước nào
        print("self-test: bắt đầu")
        var failed = 0
        func check(_ ok: Bool, _ what: String, line: Int = #line) {
            if !ok { failed += 1; print("SELF-TEST LỖI dòng \(line): \(what)") }
        }
        func hex(_ s: String) -> [UInt8] {
            stride(from: 0, to: s.count, by: 2).map { i in
                UInt8(s[s.index(s.startIndex, offsetBy: i)..<s.index(s.startIndex, offsetBy: i + 2)], radix: 16)!
            }
        }

        // SPS/PPS lấy từ remote_host.exe (NVIDIA H.264 Encoder MFT, 1920×1200)
        let sps = hex("674d403295a01e0097b01100000303e80001d4c00f1c2aa0"), pps = hex("68ee3c80")
        let start: [UInt8] = [0, 0, 0, 1]
        let idr: [UInt8] = [0x65, 0x88, 0x80, 0x10, 0x00]  // có byte 0 ở cuối: phải cắt đi
        var packet: [UInt8] = [1]  // cờ khung khoá, rồi AUD, SPS, PPS, IDR (start code 3 byte)
        for part in [[0x09, 0xF0], sps, pps] as [[UInt8]] { packet += start + part }
        packet += [0, 0, 1] + idr
        let (ps, avcc) = VideoRenderer.split(packet, from: 1)
        check(ps == [Data(sps), Data(pps)], "tách SPS/PPS")
        check(avcc == Data([0, 0, 0, 4, 0x65, 0x88, 0x80, 0x10]), "AVCC: \(Array(avcc))")
        if let ps, let f = VideoRenderer.makeFormat(ps, hevc: false) {
            let d = CMVideoFormatDescriptionGetDimensions(f)
            check(d.width == 1920 && d.height == 1200, "kích thước \(d.width)x\(d.height)")
            check(VideoRenderer.sample(avcc, f) != nil, "tạo CMSampleBuffer")
        } else {
            check(false, "CMVideoFormatDescriptionCreateFromH264ParameterSets")
        }

        // HEVC: VPS/SPS/PPS thật từ remote_host.exe (NVIDIA HEVC Encoder MFT, 1920×1080)
        let vps = hex("40010c01ffff21600000030090000003000003007bac09")
        let hsps = hex("42010121600000030090000003000003007ba003c0801107cb96b4a421192e30142000002080000003008000001e04")
        let hpps = hex("4401c1f7c0cc90"), hidr: [UInt8] = [0x26, 0x01, 0xaf, 0x10]
        var hp: [UInt8] = [3]  // cờ: khung khoá + HEVC; rồi AUD (35), VPS, SPS, PPS, IDR
        for part in [[0x46, 0x01, 0x10], vps, hsps, hpps, hidr] as [[UInt8]] { hp += start + part }
        let (hps, havcc) = VideoRenderer.split(hp, from: 1, hevc: true)
        check(hps?.count == 3 && havcc == Data([0, 0, 0, 4] + hidr), "tách HEVC")
        if let hps, let f = VideoRenderer.makeFormat(hps, hevc: true) {
            let d = CMVideoFormatDescriptionGetDimensions(f)
            check(d.width == 1920 && d.height == 1080, "HEVC: kích thước \(d.width)x\(d.height)")
            check(VideoRenderer.sample(havcc, f) != nil, "HEVC: tạo CMSampleBuffer")
        } else {
            check(false, "CMVideoFormatDescriptionCreateFromHEVCParameterSets")
        }
        check(VideoRenderer.split(hp, from: 1, hevc: false).params == nil, "gói HEVC không bị đọc nhầm thành H.264")

        print("self-test: xong phần hình")
        // Keymap: đọc đúng file của PC, tin nhắn lưu có "data" ở cuối
        let json = #"[{"label":"Nhảy","keys":"SPACE","x":0.5,"y":0.25}]"#
        let km = try? JSONDecoder().decode([KeyButton].self, from: Data(json.utf8))
        check(km?.first?.label == "Nhảy" && km?.first?.y == 0.25, "đọc keymap")
        let msg = KeyButton.saveMessage(km ?? []) ?? ""
        check(msg.hasPrefix(#"{"t":"keymap","data":["#) && msg.hasSuffix("]}"), "tin nhắn keymap: \(msg)")
        check(!msg.contains("\"id\""), "keymap không gửi id")
        check(SavedHost.normalize(" 192.168.1.12 ") == "192.168.1.12:8443" && SavedHost.normalize("a:1") == "a:1", "địa chỉ")
        check(LinkStats(ping: 20).quality == 0 && LinkStats(ping: 60).quality == 1 && LinkStats(ping: 150).quality == 2,
              "chấm màu chất lượng mạng")
        check(km?.first?.kind == nil && km?.first?.mode == nil && km?.first?.scale == 1, "file keymap cũ: trường mới để trống")
        let fire = try? JSONDecoder().decode(KeyButton.self, from: Data(#"{"label":"Bắn","keys":"LMB","x":0.7,"y":0.6,"mode":"turbo","aim":true,"size":1.5}"#.utf8))
        check(fire?.mode == .turbo && fire?.aim == true && fire?.scale == 1.5, "đọc nút liên thanh + kéo để ngắm")

        // Địa chỉ: Wi‑Fi thử LAN trước, 4G thử Tailscale trước, không trùng lặp
        let pc = SavedHost(name: "PC", address: "192.168.0.103:8443", addresses: ["192.168.0.103:8443", "100.99.115.103:8443"])
        check(pc.candidates(onWiFi: true) == ["192.168.0.103:8443", "100.99.115.103:8443"], "thứ tự trên Wi‑Fi")
        check(pc.candidates(onWiFi: false) == ["100.99.115.103:8443", "192.168.0.103:8443"], "thứ tự trên 4G")
        check(SavedHost.isTailscale("100.64.0.1:8443") && !SavedHost.isTailscale("100.128.0.1") && !SavedHost.isTailscale("10.0.0.1"),
              "dải Tailscale")
        let out = SavedHost(name: "PC", address: "192.168.0.103:8443",
                            addresses: ["192.168.0.103:8443", "14.162.162.13:8443", "100.99.115.103:8443"])
        check(out.candidates(onWiFi: false).first == "14.162.162.13:8443" && out.candidates(onWiFi: true).last == "14.162.162.13:8443",
              "IP công khai: 4G thử trước, Wi‑Fi thử sau cùng")
        check(SavedHost.kind("172.20.10.2:8443") == .lan && SavedHost.kind("14.162.162.13:8443") == .internet
              && SavedHost.kind("my-pc.local:8443") == .lan && SavedHost.kind("Nha.DuckDNS.org:8443") == .internet
              && SavedHost.kind("mypc:8443") == .lan, "phân loại địa chỉ")

        // Cần di chuyển: vùng chết, 8 hướng, đẩy hết cỡ thì thêm phím chạy
        let wasd = "W,A,S,D"
        check(Stick.keys(dx: 5, dy: 5, radius: 60, map: wasd, sprint: nil).isEmpty, "vùng chết")
        check(Stick.keys(dx: 0, dy: -40, radius: 60, map: wasd, sprint: nil) == ["W"], "lên")
        check(Stick.keys(dx: -40, dy: 0, radius: 60, map: wasd, sprint: nil) == ["A"], "trái")
        check(Stick.keys(dx: 30, dy: 30, radius: 60, map: wasd, sprint: nil) == ["S", "D"], "xuống phải")
        check(Stick.keys(dx: -30, dy: -30, radius: 60, map: wasd, sprint: nil) == ["W", "A"], "lên trái")
        check(Stick.keys(dx: 0, dy: -59, radius: 60, map: wasd, sprint: "shift") == ["W", "SHIFT"], "đẩy hết cỡ để chạy")
        check(Stick.keys(dx: 0, dy: -40, radius: 60, map: "W,A,S", sprint: nil).isEmpty, "cần thiếu phím")

        // Mẫu keymap: cần có đủ 4 phím, hồ sơ đọc/ghi lại được
        let sticks = KeyProfile.presets.flatMap(\.keys).filter(\.isStick)
        check(!sticks.isEmpty && sticks.allSatisfy { $0.keys.split(separator: ",").count == 4 }, "mẫu: cần di chuyển đủ 4 phím")
        let round = (try? JSONEncoder().encode(KeyProfile.presets)).flatMap { try? JSONDecoder().decode([KeyProfile].self, from: $0) }
        check(round?.map(\.name) == KeyProfile.presets.map(\.name) && round?[1].touch == .look, "lưu/đọc hồ sơ")
        check(KeyButton.validKeys("CTRL+SHIFT+ESC") && KeyButton.validKeys("f5") && KeyButton.validKeys("WHEELUP")
              && !KeyButton.validKeys("LSHIFTT") && !KeyButton.validKeys("F25") && !KeyButton.validKeys("Đ") && !KeyButton.validKeys(""),
              "tên phím")
        check(KeyProfile.presets.count >= 15 && Set(KeyProfile.presets.map(\.name)).count == KeyProfile.presets.count, "đủ mẫu, không trùng tên")
        print("self-test: xong keymap, kiểm bố cục \(KeyProfile.presets.count) mẫu")
        // Mỗi mẫu: phím PC hiểu được; nút không chồng nhau, không nằm dưới thanh công cụ, không lòi ra ngoài (iPhone 11 Pro Max nằm ngang)
        let screen = CGRect(x: 0, y: 0, width: 896, height: 414), bar = CGRect(x: 448 - 165, y: 0, width: 330, height: 48)
        for p in KeyProfile.presets {
            for k in p.keys where !k.isValid { check(false, "\(p.name): phím \"\(k.keys)\" của nút \(k.label)") }
            let rects = p.keys.filter { !$0.isStick }.map { k -> CGRect in
                let c = KeyCap(k).bounds.size
                return CGRect(x: k.x * screen.width - c.width / 2, y: k.y * screen.height - c.height / 2, width: c.width, height: c.height)
            }
            for i in rects.indices {
                let name = p.keys.filter { !$0.isStick }[i].label
                check(screen.contains(rects[i]), "\(p.name): nút \(name) lòi ra ngoài màn hình")
                check(!rects[i].intersects(bar), "\(p.name): nút \(name) nằm dưới thanh công cụ")
                for j in rects.indices where j > i && rects[i].insetBy(dx: -3, dy: -3).intersects(rects[j]) {
                    check(false, "\(p.name): nút \(name) chồng lên \(p.keys.filter { !$0.isStick }[j].label)")
                }
            }
        }

        print("self-test: xong bố cục")
        // Mã QR trên PC: địa chỉ thiếu cổng thì thêm 8443, chứng chỉ ghi sẵn để khỏi hỏi tin
        let qr = URL(string: "remotedesk://pair?n=DESKTOP-1&a=192.168.0.103,14.162.162.13:9000&c=0123abcd&f=AABB")!
        let pr = SavedHost.pair(qr)
        check(pr?.host.address == "192.168.0.103:8443" && pr?.host.addresses == ["192.168.0.103:8443", "14.162.162.13:9000"]
              && pr?.code == "0123abcd" && pr?.host.certHash == "aabb" && pr?.host.name == "DESKTOP-1", "đọc mã QR")
        check(SavedHost.pair(URL(string: "remotedesk://pair?a=1.2.3.4")!) == nil && SavedHost.pair(URL(string: "https://x/pair?a=1&c=2")!) == nil,
              "mã QR thiếu mã hoặc sai loại")

        print(failed == 0 ? "self-test OK" : "self-test LỖI: \(failed)")
        usleep(300_000)  // cho simctl --console kịp đọc dòng cuối trước khi app thoát
        exit(failed == 0 ? 0 : 1)
    }
}
