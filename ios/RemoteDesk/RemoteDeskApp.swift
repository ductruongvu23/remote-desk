import SwiftUI

@main
struct RemoteDeskApp: App {
    init() {
        // Chạy trên simulator: xcrun simctl launch --console <máy> vn.vdt.remotedesk --self-test
        if CommandLine.arguments.contains("--self-test") { SelfTest.run() }
    }

    var body: some Scene {
        WindowGroup {
            Group {  // --demo-stream: chụp ảnh màn điều khiển trên CI (build.sh --test)
                if CommandLine.arguments.contains("--demo-stream") { DemoStream() } else { ContentView() }
            }
            .tint(Theme.pill)  // sáng/tối theo Giao diện của iOS
        }
    }
}
