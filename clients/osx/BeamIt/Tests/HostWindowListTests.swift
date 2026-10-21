import Foundation

@main
struct HostWindowListTests {
    static func window(_ hwnd: String, pid: Int, role: String = "main", title: String? = nil,
                       image: String? = nil, state: String = "normal", x: Double = 0) -> BeamItHostWindow {
        BeamItHostWindow(hwnd: hwnd, pid: pid, role: role, owner: nil,
                         rect: .init(x: x, y: 0, width: 100, height: 100), state: state,
                         title: title, image_name: image)
    }

    static func main() {
        let list = BeamItHostWindowList()
        var events: [(String, Bool)] = []
        list.onChange = { events.append(($0.hwnd, $1)) }

        list.update(window("0x1", pid: 10, title: "Microsoft Teams", image: "ms-teams.exe"), closed: false)
        list.update(window("0x2", pid: 10, role: "owned", title: "Settings", image: "ms-teams.exe"), closed: false)
        list.update(window("0x3", pid: 10, role: "popup", image: "ms-teams.exe"), closed: false)
        list.update(window("0x4", pid: 20, role: "tool", image: "tool.exe"), closed: false)
        list.update(window("0x5", pid: 30, role: "popup", title: "Tooltip"), closed: false)

        // Popups are excluded; untitled apps fall back to the executable name.
        let apps = list.applications
        precondition(apps.map(\.pid) == [10, 20], "\(apps.map(\.pid))")
        precondition(apps[0].title == "Microsoft Teams" && apps[0].imageName == "ms-teams.exe")
        precondition(apps[0].windows.map(\.hwnd) == ["0x1", "0x2"])
        precondition(apps[1].title == "tool.exe")

        // Moves update stored geometry but do not change the listing.
        list.update(window("0x1", pid: 10, title: "Microsoft Teams", image: "ms-teams.exe", x: 50), closed: false)
        precondition(list.window("0x1")?.rect.x == 50)
        list.update(window("0x2", pid: 10), closed: true)
        precondition(list.window("0x2") == nil && list.applications[0].windows.count == 1)
        precondition(events.count == 7 && events.last! == ("0x2", true))

        let all = [
            window("0xA", pid: 1), window("0xB", pid: 1, role: "owned"), window("0xC", pid: 1, role: "tool"),
            window("0xD", pid: 1, role: "popup"), window("0xE", pid: 1, state: "minimized"),
            window("0xF", pid: 2), window("0x10", pid: 1, state: "maximized"),
        ]
        let follow = BeamItHostWindowList.windowsToFollow(all, followed: [1], excluded: ["0xA"]).map(\.hwnd)
        precondition(follow == ["0xB", "0xC", "0x10"], "\(follow)")
        precondition(BeamItHostWindowList.windowsToFollow(all, followed: [], excluded: []).isEmpty)

        precondition(window("0x1", pid: 1, title: "Chat").menuTitle == "Chat")
        precondition(window("0x1", pid: 1, role: "owned", title: "").menuTitle == "Untitled owned window")
        precondition(window("0x1", pid: 1, title: "Chat", state: "minimized").menuTitle == "Chat (minimized)")
        print("Host window list checks passed")
    }
}
