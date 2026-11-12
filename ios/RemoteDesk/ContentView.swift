import Network
import SwiftUI

/// Màn hình đầu: danh sách máy. PC đã lưu và PC đang thấy trong mạng gộp thành một dòng (chấm xanh = đang trong mạng),
/// chạm là vào. Thêm máy bằng nút + ở góc trên; chưa có PC nào thì hướng dẫn ghép bằng mã QR ngay trong danh sách.
struct ContentView: View {
    @StateObject private var store = HostStore()
    @StateObject private var discovery = Discovery()
    @StateObject private var rfb = Discovery(type: "_rfb._tcp")
    @StateObject private var vncStore = VNCStore()
    @State private var vnc: VNCClient?
    @State private var addingVNC = false
    @State private var vncAddress = ""
    @State private var vncPass = ""
    @State private var pendingVNC: VNCTarget?  // mở sau khi sheet đóng hẳn
    @State private var active: Session?
    @State private var loginTarget: SavedHost?
    @State private var form: LoginForm?
    @State private var pending: (host: SavedHost, password: String)?  // mở phiên sau khi sheet đóng hẳn
    @State private var user = ""
    @State private var pass = ""
    @State private var address = ""
    @State private var resolving: String?
    @State private var error: String?
    @State private var scanning = false
    @State private var scanned: URL?
    @State private var newVersion: String?

    private var pcs: [Machine<SavedHost, NWBrowser.Result>] {
        Machine.merge(store.hosts, discovery.found, name: \.name, address: \.address, foundName: Discovery.name)
    }
    private var phones: [Machine<VNCTarget, NWBrowser.Result>] {
        Machine.merge(vncStore.targets, rfb.found, name: \.name, address: \.address, foundName: Discovery.name)
    }

    var body: some View {
        NavigationView {
            list
                .navigationTitle("Remote Desk")
                .toolbar {
                    ToolbarItem(placement: .primaryAction) {
                        Menu {
                            Button { scanning = true } label: { Label("Quét mã QR trên PC", systemImage: "qrcode.viewfinder") }
                            Button { form = .add } label: { Label("Thêm PC bằng địa chỉ", systemImage: "network") }
                            Button { addingVNC = true } label: { Label("Thêm iPhone (TrollVNC)", systemImage: "iphone") }
                        } label: {
                            Image(systemName: "plus")
                        }
                        .accessibilityLabel("Thêm máy")
                    }
                }
        }
        .navigationViewStyle(.stack)
        .background(Color.clear  // iPhone (TrollVNC) gắn riêng: iOS 15 không mở được hai sheet gắn trên cùng một view
            .fullScreenCover(item: $vnc) { c in VNCView(client: c) { vnc = nil } }
            .sheet(isPresented: $addingVNC, onDismiss: {
                if let t = pendingVNC { pendingVNC = nil; openVNC(t, password: vncPass) }
                vncPass = ""
            }) { addVNC })
        .alert("Không kết nối được", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK", role: .cancel) {}
        } message: { Text(error ?? "") }
        .fullScreenCover(item: $active) { s in
            StreamView(session: s) { active = nil }
        }
        .sheet(isPresented: $scanning, onDismiss: {  // mở phiên sau khi sheet đóng hẳn, không thì fullScreenCover không hiện
            if let u = scanned { scanned = nil; pair(u) }
        }) { scanner }
        .onOpenURL(perform: pair)  // quét bằng app Camera: iOS mở remotedesk://pair?... vào thẳng app
        .onAppear {
            discovery.start()
            rfb.start()
        }
        .task {
            if let v = await AppUpdate.latest(), AppUpdate.newer(v, than: AppUpdate.current) { newVersion = v }
        }
    }

    private var list: some View {
        List {
            if let newVersion { Section { UpdateRow(version: newVersion) } }
            Section {
                if pcs.isEmpty { FirstPC(scan: { scanning = true }, add: { form = .add }) }
                ForEach(pcs) { m in
                    Button { tap(m) } label: {
                        DeviceRow(name: m.name, detail: detail(m, m.saved?.address, new: "Trong mạng này, chưa đăng nhập"), icon: "desktopcomputer",
                                  online: m.found != nil, busy: resolving == m.name)
                    }
                    .swipeActions { forget(m.saved) }
                    .contextMenu { forget(m.saved) }
                }
            } header: {
                Text("Máy tính").font(.title3.weight(.semibold)).foregroundStyle(Theme.ink).textCase(nil)
            } footer: {
                if !pcs.isEmpty { Text("Vuốt sang trái hoặc giữ lâu một máy đã lưu để quên nó.") }
            }
            if !phones.isEmpty {
                Section {
                    ForEach(phones) { m in
                        Button { tapVNC(m) } label: {
                            DeviceRow(name: m.name, detail: detail(m, m.saved?.address), icon: "iphone", online: m.found != nil,
                                      busy: resolving == m.name)
                        }
                        .swipeActions { forgetVNC(m.saved) }
                        .contextMenu { forgetVNC(m.saved) }
                    }
                } header: {
                    Text("iPhone (TrollVNC)").font(.title3.weight(.semibold)).foregroundStyle(Theme.ink).textCase(nil)
                } footer: {
                    Text("iPhone, iPad khác đã jailbreak và bật TrollVNC, cùng mạng Wi‑Fi. Nút Home, bàn phím và nút nguồn ở nút nhỏ giữa mép trên.")
                }
            }
        }
        .listStyle(.insetGrouped)
        .sheet(item: $form, onDismiss: {
            if let p = pending { pending = nil; start(p.host, password: p.password) }
        }) { f in
            LoginSheet(title: f == .add ? "Thêm PC bằng địa chỉ" : "Đăng nhập",
                       message: f == .add
                           ? "Ở xa không quét được mã QR thì nhập tên miền riêng đặt trên cửa sổ PC (hoặc IP nhà) và tài khoản của PC."
                           : "Tài khoản tạo trên cửa sổ Remote Desk ở \(loginTarget?.name ?? "PC").",
                       address: f == .add ? $address : nil, user: $user, pass: $pass) {
                if f == .add {
                    let a = SavedHost.normalize(address)
                    pending = (SavedHost(name: a, address: a, user: user), pass)
                    address = ""
                } else if var h = loginTarget {
                    h.user = user
                    pending = (h, pass)
                }
                pass = ""
                form = nil
            } cancel: {
                pass = ""
                form = nil
            }
        }
    }

    /// Dòng phụ: có trong mạng thì báo vậy (kèm chấm xanh), không thì địa chỉ đã lưu (bỏ cổng).
    private func detail<T>(_ m: Machine<T, NWBrowser.Result>, _ address: String?, new: String = "Trong mạng này") -> String {
        if m.found != nil { return m.saved == nil ? new : "Trong mạng này" }
        guard let a = address else { return "" }
        return a.lastIndex(of: ":").map { String(a[..<$0]) } ?? a
    }

    @ViewBuilder private func forget(_ h: SavedHost?) -> some View {
        if let h {
            Button(role: .destructive) { store.hosts.removeAll { $0.address == h.address } } label: {
                Label("Quên PC này", systemImage: "trash")
            }
        }
    }

    @ViewBuilder private func forgetVNC(_ t: VNCTarget?) -> some View {
        if let t {
            Button(role: .destructive) {
                vncStore.targets.removeAll { $0.address == t.address }
                Keychain.delete("vnc:" + t.address)
            } label: {
                Label("Quên iPhone này", systemImage: "trash")
            }
        }
    }

    private var scanner: some View {
        NavigationView {
            Group {
                if #available(iOS 16, *), QRScanner.available {
                    QRScanner { scanned = $0; scanning = false }.ignoresSafeArea()
                } else {
                    Text("Không quét được trong app (cần iOS 16 và quyền camera). Mở app Camera của iPhone, quét mã trên PC rồi chạm thông báo: Remote Desk tự mở và vào thẳng.")
                        .font(.body).foregroundStyle(Theme.slate).padding(28)
                }
            }
            .navigationTitle("Quét mã QR trên PC")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Huỷ") { scanning = false } } }
        }
        .navigationViewStyle(.stack)
    }

    private var addVNC: some View {
        NavigationView {
            Form {
                Section {
                    TextField("IP iPhone, ví dụ 192.168.0.101", text: $vncAddress).keyboardType(.numbersAndPunctuation)
                        .textInputAutocapitalization(.never).autocorrectionDisabled()
                    SecureField("Mật khẩu VNC (bỏ trống nếu không đặt)", text: $vncPass)
                } footer: { Text("Xem IP ở Cài đặt > TrollVNC trên iPhone kia. Cổng mặc định 5901.") }
            }
            .navigationTitle("Thêm iPhone (TrollVNC)")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Huỷ") { addingVNC = false } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Kết nối") {
                        let a = VNCTarget.normalize(vncAddress)
                        pendingVNC = VNCTarget(name: a, address: a)
                        addingVNC = false
                    }
                    .disabled(vncAddress.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
        }
        .navigationViewStyle(.stack)
        .sheetStyle()
    }

    /// Có trong mạng thì hỏi lại địa chỉ (IP trong nhà có thể đã đổi), không thì dùng các địa chỉ đã lưu.
    private func tap(_ m: Machine<SavedHost, NWBrowser.Result>) {
        if let f = m.found { pick(f) } else if let h = m.saved { open(h) }
    }

    private func tapVNC(_ m: Machine<VNCTarget, NWBrowser.Result>) {
        if let f = m.found { pickVNC(f) } else if let t = m.saved { openVNC(t) }
    }

    private func pickVNC(_ r: NWBrowser.Result) {
        let name = Discovery.name(r)
        resolving = name
        Discovery.resolve(r.endpoint) { addr in
            resolving = nil
            guard let addr else { error = "Không tìm được địa chỉ iPhone này. Thêm bằng IP trong Cài đặt > TrollVNC."; return }
            openVNC(VNCTarget(name: name, address: addr))
        }
    }

    private func openVNC(_ t: VNCTarget, password: String? = nil) {
        guard !VNCTarget.isSelf(t.address) else {
            error = "Đây là chính iPhone này. TrollVNC chỉ điều khiển được từ máy khác (PC hoặc iPhone/iPad khác)."
            return
        }
        let c = VNCClient(target: t, password: password.flatMap { $0.isEmpty ? nil : $0 } ?? Keychain.get("vnc:" + t.address))
        c.onConnected = { vncStore.upsert($0) }  // chỉ lưu khi đã vào được
        vnc = c
    }

    private func pick(_ r: NWBrowser.Result) {
        let name = Discovery.name(r)
        resolving = name
        Discovery.resolve(r.endpoint) { addr in
            resolving = nil
            guard let addr else { error = "Kiểm tra iPhone và PC cùng mạng, hoặc thêm PC bằng địa chỉ IP."; return }
            var h = store.hosts.first { $0.address == addr || ($0.addresses ?? []).contains(addr) }
                ?? SavedHost(name: name, address: addr)
            h.name = name
            open(h)
        }
    }

    /// Đã có khoá thiết bị thì vào thẳng, chưa có thì hỏi tài khoản.
    private func open(_ h: SavedHost) {
        if Keychain.get(h.address) != nil {
            start(h)
        } else {
            loginTarget = h
            user = h.user ?? ""
            form = .login
        }
    }

    /// Mã QR trên PC: lưu địa chỉ + chứng chỉ trong mã, vào bằng mã dùng một lần thay cho mật khẩu.
    private func pair(_ url: URL) {
        guard active == nil, let p = SavedHost.pair(url) else { return }
        var h = p.host
        h.user = store.hosts.first { $0.address == h.address }?.user
        start(h, pairCode: p.code)
    }

    private func start(_ h: SavedHost, password: String? = nil, pairCode: String? = nil) {
        let s = Session(host: h, password: password, pairCode: pairCode)
        s.onHostUpdate = { store.upsert($0) }  // chỉ lưu máy chủ khi PC đã nhận đăng nhập
        active = s
    }
}

/// Máy trong danh sách: bản đã lưu và/hoặc bản đang thấy trong mạng (Bonjour), gộp theo tên.
struct Machine<Saved, Found>: Identifiable {
    let id: String
    let name: String
    let saved: Saved?
    let found: Found?

    static func merge(_ saved: [Saved], _ found: [Found], name: (Saved) -> String, address: (Saved) -> String,
                      foundName: (Found) -> String) -> [Machine] {
        var left = found
        var out = saved.map { s -> Machine in
            let i = left.firstIndex { foundName($0) == name(s) }
            return Machine(id: address(s), name: name(s), saved: s, found: i.map { left.remove(at: $0) })
        }
        out += left.map { Machine(id: "mdns:" + foundName($0), name: foundName($0), saved: nil, found: $0) }
        return out
    }
}

enum LoginForm: Identifiable {
    case login, add
    var id: Self { self }
}

/// Đăng nhập (kèm địa chỉ PC khi thêm bằng tay) dạng sheet: ô nhập trong alert chỉ có từ iOS 16.
struct LoginSheet: View {
    let title: String
    let message: String
    var address: Binding<String>? = nil
    @Binding var user: String
    @Binding var pass: String
    var submit: () -> Void
    var cancel: () -> Void

    var body: some View {
        NavigationView {
            Form {
                Section {
                    if let address {
                        TextField("IP hoặc tên miền, ví dụ nha.duckdns.org", text: address).keyboardType(.URL)
                            .textInputAutocapitalization(.never).autocorrectionDisabled()
                    }
                    TextField("Tên đăng nhập", text: $user).textInputAutocapitalization(.never).autocorrectionDisabled()
                    SecureField("Mật khẩu", text: $pass)
                } footer: { Text(message) }
            }
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Huỷ", action: cancel) }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Đăng nhập", action: submit).disabled(pass.isEmpty || address?.wrappedValue.isEmpty == true)
                }
            }
        }
        .navigationViewStyle(.stack)
        .sheetStyle()
    }
}

/// Chưa có PC nào: việc đầu tiên là ghép bằng mã QR (hoặc nhập địa chỉ khi ở xa).
struct FirstPC: View {
    var scan: () -> Void
    var add: () -> Void

    var body: some View {
        VStack(spacing: 14) {
            Image(systemName: "qrcode.viewfinder")
                .font(.system(size: 44, weight: .light))
                .foregroundStyle(Theme.pill)
                .accessibilityHidden(true)
            Text("Ghép PC đầu tiên").font(.title3.weight(.semibold))
            Text("Trên PC mở Remote Desk, bấm \"Ghép iPhone bằng mã QR\" rồi quét. Không cần nhập IP hay mật khẩu.")
                .font(.subheadline)
                .foregroundStyle(Theme.slate)
                .multilineTextAlignment(.center)
            Button(action: scan) {
                Label("Quét mã QR trên PC", systemImage: "qrcode.viewfinder").frame(maxWidth: 240)
            }
            .primaryStyle()
            .controlSize(.large)
            Button("Nhập địa chỉ PC", action: add)
                .font(.subheadline)
                .buttonStyle(.borderless)  // trong một dòng List: chỉ nút được chạm mới chạy
            HStack(spacing: 8) {
                ProgressView()
                Text("Đang tìm PC trong mạng…")
            }
            .font(.footnote)
            .foregroundStyle(Theme.slate)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 20)
    }
}

/// Một máy: ô biểu tượng màu thương hiệu, tên, tình trạng (chấm xanh khi có trong mạng), nút vào.
struct DeviceRow: View {
    let name: String
    let detail: String
    let icon: String
    var online = false
    var busy = false

    var body: some View {
        HStack(spacing: 14) {
            Image(systemName: icon)
                .font(.system(size: 19, weight: .medium))
                .foregroundStyle(.white)
                .frame(width: 42, height: 42)
                .background(Theme.brand, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
            VStack(alignment: .leading, spacing: 3) {
                Text(name).font(.headline).foregroundStyle(Theme.ink).lineLimit(1)
                HStack(spacing: 6) {
                    if online { Circle().fill(.green).frame(width: 7, height: 7) }
                    Text(detail).font(.subheadline).foregroundStyle(Theme.slate).lineLimit(1)
                }
            }
            Spacer(minLength: 8)
            if busy {
                ProgressView()
            } else {
                Image(systemName: "play.fill")
                    .font(.system(size: 12, weight: .bold))
                    .foregroundStyle(.white)
                    .frame(width: 30, height: 30)
                    .background(Theme.pill, in: Circle())
            }
        }
        .padding(.vertical, 6)
        .contentShape(Rectangle())
        .accessibilityElement(children: .combine)
        .accessibilityHint("Chạm để kết nối")
    }
}

/// Báo bản mới: tải IPA ký lại bằng Sideloadly, hoặc thêm nguồn một lần để AltStore/SideStore tự cập nhật các bản sau.
struct UpdateRow: View {
    let version: String

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Label("Có bản mới \(version)", systemImage: "arrow.down.circle.fill").font(.headline)
            Text("Bạn đang dùng \(AppUpdate.current). Cài bằng AltStore hoặc SideStore thì thêm nguồn một lần, các bản sau tự cập nhật.")
                .font(.subheadline)
                .foregroundStyle(Theme.slate)
            HStack(spacing: 16) {
                Link("Tải bản mới", destination: AppUpdate.page).primaryStyle()
                Menu("Thêm nguồn") {
                    if let u = AppUpdate.addSource("altstore") { Link("AltStore", destination: u) }
                    if let u = AppUpdate.addSource("sidestore") { Link("SideStore", destination: u) }
                }
                .font(.subheadline)
            }
        }
        .padding(.vertical, 6)
    }
}
