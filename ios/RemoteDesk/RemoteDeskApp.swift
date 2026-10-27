import SwiftUI

@main
struct RemoteDeskApp: App {
    init() {
        // Chạy trên simulator: xcrun simctl launch --console <máy> vn.vdt.remotedesk --self-test
        if CommandLine.arguments.contains("--self-test") { SelfTest.run() }
    }

    var body: some Scene {
        WindowGroup {
            ContentView().tint(Theme.pill)  // sáng/tối theo Giao diện của iOS
        }
    }
}
