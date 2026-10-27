import Foundation

/// Một điều khiển trên màn hình. x, y là tâm theo tỉ lệ 0...1 của màn hình.
/// JSON tương thích remote_keymap.json (label, keys, x, y); các trường mới đều tuỳ chọn nên file cũ vẫn đọc được.
struct KeyButton: Codable, Identifiable, Equatable {
    enum Kind: String, Codable { case key, stick }
    /// hold: giữ là giữ phím; toggle: chạm bật, chạm lại tắt; turbo: giữ là bấm lặp nhanh (liên thanh)
    enum Mode: String, Codable, CaseIterable { case hold, toggle, turbo }

    var id = UUID()
    var label: String
    var keys: String  // nút: "CTRL+C"; cần: "W,A,S,D" (lên, trái, xuống, phải)
    var x: Double
    var y: Double
    var kind: Kind? = nil
    var mode: Mode? = nil
    var size: Double? = nil      // tỉ lệ kích thước, mặc định 1
    var aim: Bool? = nil         // kéo trên nút để xoay camera (nút bắn + ngắm một chạm)
    var floating: Bool? = nil    // cần nổi: đặt ngón đâu gần cần cũng được (mặc định bật)
    var sprint: String? = nil    // cần: phím thêm khi đẩy hết cỡ, ví dụ SHIFT để chạy

    private enum CodingKeys: String, CodingKey { case label, keys, x, y, kind, mode, size, aim, floating, sprint }

    var isStick: Bool { kind == .stick }
    var scale: Double { min(max(size ?? 1, 0.5), 2.5) }
}

/// Cần di chuyển 8 hướng ra 4 phím (kiểu D-pad của BlueStacks).
enum Stick {
    /// dx, dy tính từ tâm cần (dy dương là xuống), radius là bán kính đế.
    /// Vùng chết 25% bán kính để ngón cái rung không bấm nhầm; đẩy quá 90% thì thêm phím chạy.
    static func keys(dx: Double, dy: Double, radius: Double, map: String, sprint: String?) -> Set<String> {
        let dirs = map.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces).uppercased() }
        guard dirs.count == 4 else { return [] }
        let dist = (dx * dx + dy * dy).squareRoot()
        guard dist > radius * 0.25 else { return [] }
        let angle = atan2(-dy, dx)  // 0 = phải, π/2 = lên
        let sector = Int(((angle + 2 * .pi).truncatingRemainder(dividingBy: 2 * .pi) + .pi / 8) / (.pi / 4)) % 8
        let (up, left, down, right) = (dirs[0], dirs[1], dirs[2], dirs[3])
        let table: [Set<String>] = [[right], [right, up], [up], [left, up], [left], [left, down], [down], [right, down]]
        var s = table[sector]
        if let sprint, !sprint.isEmpty, dist >= radius * 0.9 { s.insert(sprint.uppercased()) }
        return s
    }
}

/// Bố cục keymap cho một game (hoặc dùng máy tính), kèm cách chạm vào nền màn hình.
struct KeyProfile: Codable, Identifiable, Equatable {
    var id = UUID()
    var name: String
    var touch: TouchMode
    var keys: [KeyButton]
    var preset: String?  // tên mẫu gốc, để "Khôi phục theo mẫu"
}

extension KeyButton {
    static let keyHelp = """
        Tên phím: ESC TAB ENTER SPACE BACK DEL INS HOME END PGUP PGDN UP DOWN LEFT RIGHT \
        SHIFT CTRL ALT WIN MENU CAPS PRTSC F1–F24, chữ A–Z, số 0–9, ; = , - . / ` [ \\ ] '. \
        Chuột: LMB RMB MMB WHEELUP WHEELDOWN. Bấm tổ hợp bằng dấu +, ví dụ CTRL+SHIFT+ESC.
        """

    /// Tin nhắn lưu keymap lên PC (cho trang Safari dùng chung). PC cắt "data" tới hết chuỗi nên "data" phải ở cuối.
    static func saveMessage(_ keys: [KeyButton]) -> String? {
        guard let d = try? JSONEncoder().encode(keys), let arr = String(data: d, encoding: .utf8) else { return nil }
        return "{\"t\":\"keymap\",\"data\":\(arr)}"
    }
}

extension KeyProfile {
    /// Mẫu bố cục theo thể loại. Nút hay dùng đặt chỗ ngón cái nghỉ (dưới hai bên), nút hệ thống ở góc trên trái,
    /// nút ít dùng ở mép trên; nút bắn + ngắm đặt gần giữa để còn chỗ kéo (theo hướng dẫn touch của Xbox Cloud Gaming).
    static let presets: [KeyProfile] = [
        KeyProfile(name: "Máy tính", touch: .trackpad, keys: [
            .init(label: "Esc", keys: "ESC", x: 0.06, y: 0.2), .init(label: "Tab", keys: "TAB", x: 0.06, y: 0.32),
            .init(label: "Win", keys: "WIN", x: 0.06, y: 0.44), .init(label: "Alt+Tab", keys: "ALT+TAB", x: 0.94, y: 0.2),
            .init(label: "Copy", keys: "CTRL+C", x: 0.94, y: 0.32), .init(label: "Paste", keys: "CTRL+V", x: 0.94, y: 0.44),
            .init(label: "Trái", keys: "LMB", x: 0.86, y: 0.7), .init(label: "Phải", keys: "RMB", x: 0.94, y: 0.82),
            .init(label: "Enter", keys: "ENTER", x: 0.78, y: 0.86),
        ], preset: "Máy tính"),
        KeyProfile(name: "Bắn súng (FPS)", touch: .look, keys: [
            .init(label: "Esc", keys: "ESC", x: 0.05, y: 0.09), .init(label: "Tab", keys: "TAB", x: 0.13, y: 0.09),
            .init(label: "Di chuyển", keys: "W,A,S,D", x: 0.17, y: 0.68, kind: .stick, sprint: "SHIFT"),
            .init(label: "Bắn", keys: "LMB", x: 0.76, y: 0.62, size: 1.5, aim: true),
            .init(label: "Ngắm", keys: "RMB", x: 0.92, y: 0.44, mode: .toggle, size: 1.2, aim: true),
            .init(label: "Nhảy", keys: "SPACE", x: 0.92, y: 0.76, size: 1.3),
            .init(label: "Ngồi", keys: "CTRL", x: 0.82, y: 0.9, mode: .toggle),
            .init(label: "Nạp", keys: "R", x: 0.64, y: 0.84),
            .init(label: "Dùng", keys: "E", x: 0.62, y: 0.6),
            .init(label: "1", keys: "1", x: 0.62, y: 0.09), .init(label: "2", keys: "2", x: 0.7, y: 0.09),
            .init(label: "Lựu", keys: "G", x: 0.8, y: 0.09),
        ], preset: "Bắn súng (FPS)"),
        KeyProfile(name: "Đua xe", touch: .trackpad, keys: [
            .init(label: "Esc", keys: "ESC", x: 0.05, y: 0.09),
            .init(label: "◀", keys: "A", x: 0.09, y: 0.72, size: 1.6), .init(label: "▶", keys: "D", x: 0.25, y: 0.72, size: 1.6),
            .init(label: "Ga", keys: "W", x: 0.9, y: 0.7, size: 1.8), .init(label: "Phanh", keys: "S", x: 0.72, y: 0.8, size: 1.4),
            .init(label: "Nitro", keys: "SHIFT", x: 0.9, y: 0.4, size: 1.2),
            .init(label: "Phanh tay", keys: "SPACE", x: 0.72, y: 0.56),
            .init(label: "Camera", keys: "C", x: 0.5, y: 0.09), .init(label: "Reset", keys: "R", x: 0.6, y: 0.09),
        ], preset: "Đua xe"),
        KeyProfile(name: "MOBA", touch: .directRight, keys: [
            .init(label: "Esc", keys: "ESC", x: 0.05, y: 0.09), .init(label: "Tab", keys: "TAB", x: 0.13, y: 0.09),
            .init(label: "Q", keys: "Q", x: 0.66, y: 0.88), .init(label: "W", keys: "W", x: 0.76, y: 0.76),
            .init(label: "E", keys: "E", x: 0.86, y: 0.66), .init(label: "R", keys: "R", x: 0.95, y: 0.52, size: 1.2),
            .init(label: "D", keys: "D", x: 0.56, y: 0.76), .init(label: "F", keys: "F", x: 0.66, y: 0.64),
            .init(label: "Đánh", keys: "A", x: 0.92, y: 0.86, size: 1.3),
            .init(label: "Về", keys: "B", x: 0.05, y: 0.88), .init(label: "Cam", keys: "SPACE", x: 0.05, y: 0.5),
            .init(label: "1", keys: "1", x: 0.5, y: 0.09), .init(label: "2", keys: "2", x: 0.57, y: 0.09),
            .init(label: "3", keys: "3", x: 0.64, y: 0.09),
        ], preset: "MOBA"),
        KeyProfile(name: "Hành động 2D", touch: .trackpad, keys: [
            .init(label: "Esc", keys: "ESC", x: 0.05, y: 0.09),
            .init(label: "Di chuyển", keys: "UP,LEFT,DOWN,RIGHT", x: 0.17, y: 0.7, kind: .stick, floating: true),
            .init(label: "Nhảy", keys: "SPACE", x: 0.9, y: 0.78, size: 1.4),
            .init(label: "Đánh", keys: "J", x: 0.76, y: 0.82, size: 1.3), .init(label: "Mạnh", keys: "K", x: 0.82, y: 0.58),
            .init(label: "Lướt", keys: "SHIFT", x: 0.94, y: 0.52), .init(label: "Liên", keys: "J", x: 0.66, y: 0.64, mode: .turbo),
        ], preset: "Hành động 2D"),
    ]
}
