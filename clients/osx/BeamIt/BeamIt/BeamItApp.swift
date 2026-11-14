import SwiftUI

@main
struct BeamItApp: App {
    @AppStorage("clipboardSyncEnabled") private var clipboardSyncEnabled = false
    @AppStorage("showHostCursor") private var showHostCursor = false
    var body: some Scene {
        WindowGroup {
            ContentView()
                .frame(minWidth: 520, minHeight: 560)
        }
        .commands {
            CommandGroup(after: .windowSize) {
                Button("Fullscreen") {
                    BeamItScalingMenu.shared.controller?.toggleFullscreen()
                }
                .disabled(BeamItScalingMenu.shared.controller == nil)
            }
            if !BeamItScalingMenu.shared.streamWindowIsKey {
                CommandMenu("Connection") {
                    Toggle("Show Host Cursor in Video", isOn: $showHostCursor)
                    Toggle("Clipboard Sync", isOn: $clipboardSyncEnabled)
                }
            }
            if BeamItScalingMenu.shared.streamWindowIsKey {
            CommandMenu("Stream") {
                Button("Add New Stream") {
                    BeamItMainWindow.show()
                }
                Menu("Connect Window") {
                    let candidates = BeamItStreamManager.shared.connectableWindows
                    ForEach(candidates, id: \.hwnd) { window in
                        Button(window.menuTitle) {
                            BeamItStreamManager.shared.connectWindow(window.hwnd)
                        }
                    }
                    if candidates.isEmpty {
                        Text("No Other Windows")
                    }
                }
                .disabled(BeamItScalingMenu.shared.controller.map { $0.sourceKind == "desktop" } ?? true)
                Button("Capture Mouse") {
                    BeamItStreamWindowController.captureMouseForActiveStream()
                }
                Toggle("Allow Aspect Ratio Scaling", isOn: Binding(
                    get: { BeamItScalingMenu.shared.allowsScaling },
                    set: { BeamItScalingMenu.shared.controller?.setAllowsScaling($0) }
                ))
                .disabled(BeamItScalingMenu.shared.controller == nil)
                ForEach([0.5, 1.0, 2.0], id: \.self) { scale in
                    Button("\(scale.formatted())×") {
                        BeamItScalingMenu.shared.controller?.setWindowScale(CGFloat(scale))
                    }
                    .disabled(!BeamItScalingMenu.shared.allowsScaling)
                }
                Toggle("Auto Move Mouse When Not in Focus", isOn: Binding(
                    get: { BeamItAntiIdle.shared.isEnabled },
                    set: { BeamItAntiIdle.shared.isEnabled = $0 }
                ))
                Divider()
                Button("Save PNG Screenshot…") {
                    BeamItScalingMenu.shared.controller?.saveScreenshot()
                }
                .disabled(BeamItScalingMenu.shared.controller == nil)
                Button("Change Parameters…") {
                    BeamItStreamWindowController.changeActiveStreamParameters()
                }
                Button("Stream info…") {
                    BeamItStreamWindowController.showActiveStreamInfo()
                }
                Toggle("Clipboard Sync", isOn: $clipboardSyncEnabled)
                Button("Sync Host to macOS") {
                    BeamItClipboardMenu.shared.synchronize(hostToMac: true)
                }
                .disabled(BeamItClipboardMenu.shared.sync == nil)
                Button("Sync macOS to Host") {
                    BeamItClipboardMenu.shared.synchronize(hostToMac: false)
                }
                .disabled(BeamItClipboardMenu.shared.sync == nil)
                Divider()
                Button("Send Text to Target…") {
                    BeamItStreamWindowController.presentSendTextForActiveStream()
                }
            }
            }
        }
    }
}
