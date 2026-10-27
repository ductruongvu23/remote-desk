import Network
import SwiftUI

/// Màn hình đầu kiểu "white gallery": sân khấu trắng ở trên, dải #f5f5f7 với các thẻ PC trắng bo 28.
struct ContentView: View {
    @StateObject private var store = HostStore()
    @StateObject private var discovery = Discovery()
    @State private var active: Session?
    @State private var loginTarget: SavedHost?
    @State private var askLogin = false
    @State private var adding = false
    @State private var user = ""
    @State private var pass = ""
    @State private var address = ""
    @State private var resolving: String?
    @State private var error: String?
    @State private var scanning = false
    @State private var scanned: URL?

    var body: some View {
        ScrollView {
            VStack(spacing: 0) {
                hero
                hosts
            }
        }
        .background(Theme.band.ignoresSafeArea())
        .alert("Đăng nhập", isPresented: $askLogin) {
            TextField("Tên đăng nhập", text: $user).textInputAutocapitalization(.never).autocorrectionDisabled()
            SecureField("Mật khẩu", text: $pass)
            Button("Đăng nhập") {
                if var h = loginTarget { h.user = user; start(h, password: pass) }
                pass = ""
            }
            Button("Huỷ", role: .cancel) { pass = "" }
        } message: { Text("Tài khoản tạo trên cửa sổ Remote Desk ở \(loginTarget?.name ?? "PC").") }
        .alert("Thêm PC bằng địa chỉ", isPresented: $adding) {
            TextField("IP hoặc tên miền, ví dụ nha.duckdns.org", text: $address).keyboardType(.URL)
                .textInputAutocapitalization(.never).autocorrectionDisabled()
            TextField("Tên đăng nhập", text: $user).textInputAutocapitalization(.never).autocorrectionDisabled()
            SecureField("Mật khẩu", text: $pass)
            Button("Đăng nhập") {
                let a = SavedHost.normalize(address)
                start(SavedHost(name: a, address: a, user: user), password: pass)
                address = ""
                pass = ""
            }
            Button("Huỷ", role: .cancel) { pass = "" }
        } message: {
            Text("Ở xa không quét được mã QR thì nhập tên miền riêng đặt trên cửa sổ PC (hoặc IP nhà) và tài khoản của PC.")
        }
        .alert("Không tìm được địa chỉ PC", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK", role: .cancel) {}
        } message: { Text(error ?? "") }
        .fullScreenCover(item: $active) { s in
            StreamView(session: s) { active = nil }
        }
        .sheet(isPresented: $scanning, onDismiss: {  // mở phiên sau khi sheet đóng hẳn, không thì fullScreenCover không hiện
            if let u = scanned { scanned = nil; pair(u) }
        }) {
            NavigationStack {
                Group {
                    if QRScanner.available {
                        QRScanner { scanned = $0; scanning = false }.ignoresSafeArea()
                    } else {
                        Text("Không mở được camera trong app. Cho phép Remote Desk dùng camera trong Cài đặt, hoặc quét mã bằng app Camera của iPhone.")
                            .font(.text(17)).foregroundStyle(Theme.slate).padding(28)
                    }
                }
                .navigationTitle("Quét mã QR trên PC")
                .navigationBarTitleDisplayMode(.inline)
                .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Huỷ") { scanning = false } } }
            }
        }
        .onOpenURL(perform: pair)  // quét bằng app Camera: iOS mở remotedesk://pair?... vào thẳng app
        .onAppear { discovery.start() }
    }

    /// Sân khấu: nhãn sản phẩm, tiêu đề lớn, một dòng phụ, hình thiết bị đứng giữa không có khung.
    private var hero: some View {
        VStack(spacing: 12) {
            Text("Remote Desk").font(.display(21)).tracking(0.23)
            Text("Điều khiển PC\nngay trên iPhone.")
                .font(.display(40)).tracking(-0.6)
                .multilineTextAlignment(.center)
            Text("Hình HEVC giải mã phần cứng, độ trễ thấp.\nKeymap sẵn cho game, tự sắp lại được.")
                .font(.text(17)).tracking(-0.37)
                .foregroundStyle(Theme.slate)
                .multilineTextAlignment(.center)
            Button {
                scanning = true
            } label: {
                Label("Quét mã QR trên PC", systemImage: "qrcode.viewfinder")
            }
            .buttonStyle(BluePill())
            .padding(.top, 8)
            Image(systemName: "macbook.and.iphone")
                .font(.system(size: 120, weight: .ultraLight))
                .padding(.top, 28)
                .accessibilityHidden(true)
        }
        .foregroundStyle(Theme.ink)
        .padding(.horizontal, 24)
        .padding(.top, 64)
        .padding(.bottom, 52)
        .frame(maxWidth: .infinity)
        .background(Theme.canvas)
    }

    private var hosts: some View {
        VStack(alignment: .leading, spacing: 20) {
            HStack(alignment: .firstTextBaseline) {
                Text("PC của bạn").font(.display(28)).foregroundStyle(Theme.ink)
                Spacer()
                Button("Thêm bằng địa chỉ ›") { adding = true }
                    .font(.text(17)).foregroundStyle(Theme.link)
            }
            if discovery.found.isEmpty && store.hosts.isEmpty {
                HStack(spacing: 16) {
                    ProgressView()
                    Text("Đang tìm PC… Mở Remote Desk trên PC, cùng mạng Wi‑Fi với iPhone, hoặc quét mã QR.")
                        .font(.text(17)).tracking(-0.37).foregroundStyle(Theme.slate)
                }
                .padding(28)
                .frame(maxWidth: .infinity, alignment: .leading)
                .card()
            }
            ForEach(discovery.found, id: \.self) { r in
                let name = Discovery.name(r)
                HostCard(title: name, subtitle: "Trong mạng này", busy: resolving == name) { pick(r) }
            }
            ForEach(store.hosts) { h in
                HostCard(title: h.name, subtitle: h.address, busy: false) { open(h) }
                    .contextMenu {
                        Button("Quên PC này", role: .destructive) { store.hosts.removeAll { $0.address == h.address } }
                    }
            }
            Text("PC chạy remote_host.exe là máy chủ. Lần đầu bấm \"Ghép bằng mã QR\" trên PC rồi quét: không cần nhập IP hay mật khẩu, mã có sẵn cả địa chỉ để vào từ ngoài. Các lần sau app tự vào. Giữ lâu một PC đã lưu để quên nó.")
                .font(.text(12)).tracking(-0.12)
                .foregroundStyle(Theme.slate)
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 40)
        .frame(maxWidth: 720)
        .frame(maxWidth: .infinity)
        .background(Theme.band)
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
            askLogin = true
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

/// Thẻ trắng bo 28: biểu tượng máy, tên, dòng phụ, nút xanh nhỏ "Kết nối".
struct HostCard: View {
    let title: String
    let subtitle: String
    let busy: Bool
    let action: () -> Void

    var body: some View {
        HStack(spacing: 16) {
            Image(systemName: "desktopcomputer")
                .font(.system(size: 28, weight: .light))
                .foregroundStyle(Theme.ink)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.display(19)).tracking(0.23).foregroundStyle(Theme.ink).lineLimit(1)
                Text(subtitle).font(.text(14)).tracking(-0.22).foregroundStyle(Theme.slate).lineLimit(1)
            }
            Spacer(minLength: 8)
            if busy { ProgressView() } else { Button("Kết nối", action: action).buttonStyle(BluePill()) }
        }
        .padding(.horizontal, 24)
        .padding(.vertical, 20)
        .card()
        .contentShape(RoundedRectangle(cornerRadius: 28, style: .continuous))
        .onTapGesture(perform: action)
    }
}
