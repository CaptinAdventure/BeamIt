import AppKit

@MainActor
final class BeamItApplicationLifetime: NSObject, NSApplicationDelegate {
    private let lifetime: BeamItClipboardLifetime
    private var termination: Task<Void, Never>?

    override init() {
        lifetime = .shared
        super.init()
    }
    // Native tests supply a private-root coordinator and a fake reply.
    init(lifetime: BeamItClipboardLifetime) {
        self.lifetime = lifetime
        super.init()
    }
    func start(observeAutomatically: Bool = true) { lifetime.start(observeAutomatically: observeAutomatically) }
    func applicationDidFinishLaunching(_ notification: Notification) { start() }

    func requestTermination(reply: @escaping @MainActor () -> Void) {
        guard termination == nil else { return }
        termination = Task {
            await lifetime.shutdown()
            reply()
        }
    }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        requestTermination { sender.reply(toApplicationShouldTerminate: true) }
        return .terminateLater
    }
}
