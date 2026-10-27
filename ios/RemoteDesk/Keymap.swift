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
    var cursor: Bool? = nil      // lúc nút đang bật/giữ: chạm nền = bấm chuột trái tại chỗ (bấm menu trong game khoá chuột)

    private enum CodingKeys: String, CodingKey { case label, keys, x, y, kind, mode, size, aim, floating, sprint, cursor }

    var isStick: Bool { kind == .stick }
    var scale: Double { min(max(size ?? 1, 0.5), 2.5) }

    /// Tên phím PC bấm được, khớp bảng VK trong remote_host.cpp (thêm chữ, số, F1–F24).
    static let names: Set<String> = [
        "BACK", "TAB", "ENTER", "SHIFT", "CTRL", "ALT", "PAUSE", "CAPS", "ESC", "SPACE", "PGUP", "PGDN", "END", "HOME",
        "LEFT", "UP", "RIGHT", "DOWN", "PRTSC", "INS", "DEL", "WIN", "MENU", "LSHIFT", "RSHIFT", "LCTRL", "RCTRL", "LALT", "RALT",
        ";", "=", ",", "-", ".", "/", "`", "[", "\\", "]", "'", "LMB", "RMB", "MMB", "WHEELUP", "WHEELDOWN",
    ]
    static func validKeys(_ s: String) -> Bool {
        s.uppercased().split(separator: "+", omittingEmptySubsequences: false).allSatisfy { p in
            let k = p.trimmingCharacters(in: .whitespaces)
            if names.contains(k) { return true }
            if k.count == 1, let c = k.first, c.isASCII, c.isLetter || c.isNumber { return true }
            return k.hasPrefix("F") && (Int(k.dropFirst()).map { (1...24).contains($0) } ?? false)
        }
    }
    /// PC bấm được. Nút "Chuột" thuần (chỉ đổi cách chạm nền) được để trống phím.
    var isValid: Bool {
        if isStick {
            let d = keys.split(separator: ",", omittingEmptySubsequences: false)
            return d.count == 4 && d.allSatisfy { Self.validKeys(String($0)) } && (sprint.map(Self.validKeys) ?? true)
        }
        return keys.isEmpty ? cursor == true : Self.validKeys(keys)
    }
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
        Chuột: LMB RMB MMB WHEELUP WHEELDOWN. Bấm tổ hợp bằng dấu +, ví dụ CTRL+SHIFT+ESC. \
        Nút có "Chạm nền = bấm chuột" được để trống phím.
        """

    /// Tin nhắn lưu keymap lên PC (cho trang Safari dùng chung). PC cắt "data" tới hết chuỗi nên "data" phải ở cuối.
    static func saveMessage(_ keys: [KeyButton]) -> String? {
        guard let d = try? JSONEncoder().encode(keys), let arr = String(data: d, encoding: .utf8) else { return nil }
        return "{\"t\":\"keymap\",\"data\":\(arr)}"
    }
}

extension KeyProfile {
    private static func move(_ sprint: String? = nil) -> KeyButton {
        .init(label: "Di chuyển", keys: "W,A,S,D", x: 0.17, y: 0.68, kind: .stick, sprint: sprint)
    }
    /// Nút "Chuột" bên trái: bật thì chạm nền thành bấm chuột tại chỗ (mở túi đồ, bản đồ, menu).
    /// Game miHoYo/Kuro phải giữ ALT mới hiện chuột; game khác hiện chuột sẵn khi mở menu nên để trống.
    private static func cursor(_ keys: String = "") -> KeyButton {
        .init(label: "Chuột", keys: keys, x: 0.06, y: 0.36, mode: .toggle, cursor: true)
    }
    /// Hàng phím số ở góc trên phải (đổi súng, đổi nhân vật).
    private static func slots(_ keys: [String]) -> [KeyButton] {
        keys.enumerated().map { i, k in .init(label: k, keys: k, x: 0.94 - Double(keys.count - 1 - i) * 0.07, y: 0.09, size: 0.9) }
    }
    /// Phần chung của game bắn súng PC: bắn và ngắm đều kéo được để xoay camera.
    private static func shooter(sprint: String? = nil) -> [KeyButton] {
        [
            .init(label: "Esc", keys: "ESC", x: 0.05, y: 0.09), .init(label: "Tab", keys: "TAB", x: 0.13, y: 0.09),
            move(sprint), cursor(),
            .init(label: "Bắn", keys: "LMB", x: 0.76, y: 0.62, size: 1.5, aim: true),
            .init(label: "Ngắm", keys: "RMB", x: 0.92, y: 0.44, size: 1.2, aim: true),
            .init(label: "Nhảy", keys: "SPACE", x: 0.92, y: 0.76, size: 1.3),
            .init(label: "Nạp", keys: "R", x: 0.64, y: 0.84),
        ]
    }
    /// Phần chung của game nhập vai hành động (Genshin, Wuthering Waves...): như bản điện thoại, nút đánh to ở góc phải,
    /// chiêu và nộ vòng quanh, hai nút ở mép phải (nhảy/né/đổi người).
    private static func arpg(edge: [(String, String)], skill: String, burst: String) -> [KeyButton] {
        [
            .init(label: "Esc", keys: "ESC", x: 0.05, y: 0.09), cursor("ALT"), move(),
            .init(label: "Đánh", keys: "LMB", x: 0.84, y: 0.7, size: 1.6),
            .init(label: edge[0].0, keys: edge[0].1, x: 0.95, y: 0.86, size: 1.25),
            .init(label: edge[1].0, keys: edge[1].1, x: 0.95, y: 0.56, size: 1.2),
            .init(label: "Chiêu", keys: skill, x: 0.69, y: 0.84, size: 1.3),
            .init(label: "Nộ", keys: burst, x: 0.72, y: 0.56, size: 1.3),
        ]
    }

    /// Mẫu bố cục. Nút hay dùng đặt chỗ ngón cái nghỉ (dưới hai bên), nút hệ thống ở góc trên trái, phím số ở góc trên phải,
    /// giữa mép trên để trống cho thanh công cụ; nút bắn + ngắm đặt gần giữa để còn chỗ kéo (theo hướng dẫn touch của
    /// Xbox Cloud Gaming). Phím theo mặc định trên PC của từng game.
    static let presets: [KeyProfile] = [
        KeyProfile(name: "Máy tính", touch: .trackpad, keys: [
            .init(label: "Esc", keys: "ESC", x: 0.06, y: 0.18), .init(label: "Tab", keys: "TAB", x: 0.06, y: 0.32),
            .init(label: "Win", keys: "WIN", x: 0.06, y: 0.46), .init(label: "Alt+Tab", keys: "ALT+TAB", x: 0.94, y: 0.18),
            .init(label: "Copy", keys: "CTRL+C", x: 0.94, y: 0.32), .init(label: "Paste", keys: "CTRL+V", x: 0.94, y: 0.46),
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
            .init(label: "1", keys: "1", x: 0.8, y: 0.09), .init(label: "2", keys: "2", x: 0.87, y: 0.09),
            .init(label: "Lựu", keys: "G", x: 0.94, y: 0.09),
        ], preset: "Bắn súng (FPS)"),
        KeyProfile(name: "Đua xe", touch: .trackpad, keys: [
            .init(label: "Esc", keys: "ESC", x: 0.05, y: 0.09),
            .init(label: "◀", keys: "A", x: 0.09, y: 0.72, size: 1.6), .init(label: "▶", keys: "D", x: 0.25, y: 0.72, size: 1.6),
            .init(label: "Ga", keys: "W", x: 0.9, y: 0.7, size: 1.8), .init(label: "Phanh", keys: "S", x: 0.72, y: 0.8, size: 1.4),
            .init(label: "Nitro", keys: "SHIFT", x: 0.9, y: 0.4, size: 1.2),
            .init(label: "Phanh tay", keys: "SPACE", x: 0.72, y: 0.56),
            .init(label: "Camera", keys: "C", x: 0.81, y: 0.09), .init(label: "Reset", keys: "R", x: 0.93, y: 0.09),
        ], preset: "Đua xe"),
        KeyProfile(name: "MOBA (Liên Minh, Dota 2)", touch: .directRight, keys: [
            .init(label: "Esc", keys: "ESC", x: 0.05, y: 0.09), .init(label: "Tab", keys: "TAB", x: 0.13, y: 0.09),
            .init(label: "Q", keys: "Q", x: 0.66, y: 0.88), .init(label: "W", keys: "W", x: 0.76, y: 0.76),
            .init(label: "E", keys: "E", x: 0.86, y: 0.66), .init(label: "R", keys: "R", x: 0.95, y: 0.52, size: 1.2),
            .init(label: "D", keys: "D", x: 0.56, y: 0.76), .init(label: "F", keys: "F", x: 0.66, y: 0.64),
            .init(label: "Đánh", keys: "A", x: 0.92, y: 0.86, size: 1.3),
            .init(label: "Về", keys: "B", x: 0.05, y: 0.88), .init(label: "Cam", keys: "SPACE", x: 0.05, y: 0.5),
        ] + slots(["1", "2", "3"]), preset: "MOBA (Liên Minh, Dota 2)"),
        KeyProfile(name: "Hành động 2D", touch: .trackpad, keys: [
            .init(label: "Esc", keys: "ESC", x: 0.05, y: 0.09),
            .init(label: "Di chuyển", keys: "UP,LEFT,DOWN,RIGHT", x: 0.17, y: 0.7, kind: .stick, floating: true),
            .init(label: "Nhảy", keys: "SPACE", x: 0.9, y: 0.78, size: 1.4),
            .init(label: "Đánh", keys: "J", x: 0.76, y: 0.82, size: 1.3), .init(label: "Mạnh", keys: "K", x: 0.82, y: 0.58),
            .init(label: "Lướt", keys: "SHIFT", x: 0.94, y: 0.52), .init(label: "Liên", keys: "J", x: 0.66, y: 0.64, mode: .turbo),
        ], preset: "Hành động 2D"),

        // Minecraft Java: kéo trên Phá/Đặt để vừa đào vừa xoay; cuộn chuột đổi ô đồ; mở túi thì bật "Chuột" để bấm ô.
        KeyProfile(name: "Minecraft", touch: .look, keys: [
            .init(label: "Esc", keys: "ESC", x: 0.05, y: 0.09), .init(label: "Góc nhìn", keys: "F5", x: 0.15, y: 0.09),
            .init(label: "Túi", keys: "E", x: 0.25, y: 0.09), cursor(), move("CTRL"),
            .init(label: "Ngồi", keys: "SHIFT", x: 0.06, y: 0.52, mode: .toggle),
            .init(label: "Phá", keys: "LMB", x: 0.8, y: 0.66, size: 1.5, aim: true),
            .init(label: "Đặt", keys: "RMB", x: 0.92, y: 0.44, size: 1.3, aim: true),
            .init(label: "Nhảy", keys: "SPACE", x: 0.92, y: 0.82, size: 1.4),
            .init(label: "Ném", keys: "Q", x: 0.66, y: 0.86),
            .init(label: "◀", keys: "WHEELUP", x: 0.44, y: 0.9), .init(label: "▶", keys: "WHEELDOWN", x: 0.56, y: 0.9),
        ], preset: "Minecraft"),
        // CS2: chuột phải là ống ngắm (bấm để đổi), B mở cửa hàng (bật "Chuột" để mua)
        KeyProfile(name: "Counter-Strike 2", touch: .look, keys: shooter() + [
            .init(label: "Mua", keys: "B", x: 0.21, y: 0.09),
            .init(label: "Ngồi", keys: "CTRL", x: 0.82, y: 0.9, mode: .toggle),
            .init(label: "Dùng", keys: "E", x: 0.62, y: 0.6), .init(label: "Đổi", keys: "Q", x: 0.62, y: 0.36),
            .init(label: "Đi bộ", keys: "SHIFT", x: 0.06, y: 0.9, mode: .toggle),
        ] + slots(["1", "2", "3", "4"]), preset: "Counter-Strike 2"),
        // Valorant: C Q E là chiêu, X là tuyệt chiêu, F dùng/gỡ bom
        KeyProfile(name: "Valorant", touch: .look, keys: shooter() + [
            .init(label: "Mua", keys: "B", x: 0.21, y: 0.09),
            .init(label: "Ngồi", keys: "CTRL", x: 0.82, y: 0.9, mode: .toggle),
            .init(label: "C", keys: "C", x: 0.52, y: 0.84), .init(label: "Q", keys: "Q", x: 0.56, y: 0.62),
            .init(label: "E", keys: "E", x: 0.64, y: 0.4), .init(label: "Ult", keys: "X", x: 0.8, y: 0.26),
            .init(label: "F", keys: "F", x: 0.44, y: 0.64),
            .init(label: "Đi bộ", keys: "SHIFT", x: 0.06, y: 0.9, mode: .toggle),
        ] + slots(["1", "2", "3", "4"]), preset: "Valorant"),
        // PUBG: đẩy cần hết cỡ là giữ SHIFT để chạy, C ngồi, Z nằm, Q/E nghiêng, Tab túi đồ
        KeyProfile(name: "PUBG", touch: .look, keys: shooter(sprint: "SHIFT") + [
            .init(label: "Bản đồ", keys: "M", x: 0.22, y: 0.09),
            .init(label: "Ngồi", keys: "C", x: 0.82, y: 0.9), .init(label: "Nằm", keys: "Z", x: 0.72, y: 0.92),
            .init(label: "Nhặt", keys: "F", x: 0.62, y: 0.6),
            .init(label: "Q", keys: "Q", x: 0.66, y: 0.36), .init(label: "E", keys: "E", x: 0.74, y: 0.36),
        ] + slots(["1", "2", "3", "G"]), preset: "PUBG"),
        // Genshin: E kỹ năng nguyên tố, Q nộ, SHIFT lướt, F nhặt, 1–4 đổi nhân vật; giữ ALT hiện chuột
        KeyProfile(name: "Genshin Impact", touch: .look, keys: arpg(edge: [("Nhảy", "SPACE"), ("Lướt", "SHIFT")], skill: "E", burst: "Q") + [
            .init(label: "Bản đồ", keys: "M", x: 0.14, y: 0.09), .init(label: "Túi", keys: "B", x: 0.23, y: 0.09),
            .init(label: "Nhặt", keys: "F", x: 0.6, y: 0.62),
        ] + slots(["1", "2", "3", "4"]), preset: "Genshin Impact"),
        // Star Rail: ngoài trận bấm chuột trái để đánh trước; trong trận Q đánh thường, E chiến kỹ, 1–4 tuyệt kỹ, V tự đánh
        KeyProfile(name: "Honkai: Star Rail", touch: .look, keys: [
            .init(label: "Esc", keys: "ESC", x: 0.05, y: 0.09), .init(label: "Bản đồ", keys: "M", x: 0.14, y: 0.09),
            .init(label: "Auto", keys: "V", x: 0.23, y: 0.09), cursor("ALT"), move("SHIFT"),
            .init(label: "Đánh", keys: "LMB", x: 0.84, y: 0.7, size: 1.6),
            .init(label: "Thường", keys: "Q", x: 0.69, y: 0.84, size: 1.2),
            .init(label: "Chiêu", keys: "E", x: 0.72, y: 0.56, size: 1.2),
            .init(label: "Space", keys: "SPACE", x: 0.95, y: 0.86, size: 1.1),
            .init(label: "F", keys: "F", x: 0.95, y: 0.56, size: 1.2),
        ] + slots(["1", "2", "3", "4"]), preset: "Honkai: Star Rail"),
        // ZZZ: SHIFT né, SPACE đổi người kế, C đổi người trước, E chiêu đặc biệt, Q tuyệt kỹ, chuột giữa khoá mục tiêu
        KeyProfile(name: "Zenless Zone Zero", touch: .look, keys: arpg(edge: [("Né", "SHIFT"), ("Đổi", "SPACE")], skill: "E", burst: "Q") + [
            .init(label: "Đổi lùi", keys: "C", x: 0.6, y: 0.62), .init(label: "Dùng", keys: "F", x: 0.6, y: 0.4),
            .init(label: "Khoá", keys: "MMB", x: 0.86, y: 0.4),
        ], preset: "Zenless Zone Zero"),
        // Wuthering Waves: E kỹ năng cộng hưởng, R giải phóng, Q Echo, SHIFT né, 1–3 đổi nhân vật
        KeyProfile(name: "Wuthering Waves", touch: .look, keys: arpg(edge: [("Nhảy", "SPACE"), ("Né", "SHIFT")], skill: "E", burst: "R") + [
            .init(label: "Bản đồ", keys: "M", x: 0.14, y: 0.09), .init(label: "Túi", keys: "B", x: 0.23, y: 0.09),
            .init(label: "Echo", keys: "Q", x: 0.6, y: 0.62), .init(label: "Nhặt", keys: "F", x: 0.6, y: 0.4),
        ] + slots(["1", "2", "3"]), preset: "Wuthering Waves"),
        // Roblox: chuột tự do như trackpad; giữ "Xoay" (chuột phải) rồi kéo để xoay camera, SHIFT là khoá camera
        KeyProfile(name: "Roblox", touch: .trackpad, keys: [
            .init(label: "Esc", keys: "ESC", x: 0.05, y: 0.09), move(),
            .init(label: "Bấm", keys: "LMB", x: 0.78, y: 0.64, size: 1.4),
            .init(label: "Xoay", keys: "RMB", x: 0.92, y: 0.46, size: 1.3, aim: true),
            .init(label: "Nhảy", keys: "SPACE", x: 0.92, y: 0.8, size: 1.4),
            .init(label: "Khoá cam", keys: "SHIFT", x: 0.62, y: 0.46), .init(label: "Dùng", keys: "E", x: 0.62, y: 0.82),
        ] + slots(["1", "2", "3"]), preset: "Roblox"),
        // GTA V: đi bộ và lái xe chung cần (W ga, S phanh), SPACE nhảy/phanh tay, F lên xe, giữ Tab rồi kéo để chọn súng
        KeyProfile(name: "GTA V", touch: .look, keys: shooter(sprint: "SHIFT").filter { $0.keys != "TAB" } + [
            .init(label: "Cam", keys: "V", x: 0.12, y: 0.09), .init(label: "ĐT", keys: "UP", x: 0.19, y: 0.09),
            .init(label: "Menu", keys: "M", x: 0.27, y: 0.09),
            .init(label: "Lên xe", keys: "F", x: 0.62, y: 0.6), .init(label: "Nấp", keys: "Q", x: 0.82, y: 0.9),
            .init(label: "Súng", keys: "TAB", x: 0.74, y: 0.36, aim: true),
        ], preset: "GTA V"),
    ]
}
