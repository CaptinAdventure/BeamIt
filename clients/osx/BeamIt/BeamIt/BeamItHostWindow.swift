import Foundation

/// Host top-level window reported by `window_opened`/`window_changed`/`window_closed` (PROTOCOL.md §5.12).
struct BeamItHostWindow: Decodable, Equatable {
    let hwnd: String
    let pid: Int
    let role: String
    let owner: String?
    let rect: Rect
    let state: String
    var title: String?
    var image_name: String?

    struct Rect: Decodable, Equatable {
        let x: Double
        let y: Double
        let width: Double
        let height: Double
    }
}

extension BeamItHostWindow {
    var menuTitle: String {
        var text = title.flatMap { $0.isEmpty ? nil : $0 } ?? "Untitled \(role) window"
        if state == "minimized" { text += " (minimized)" }
        return text
    }
}
