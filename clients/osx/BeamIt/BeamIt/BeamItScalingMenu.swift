import Observation

@Observable
final class BeamItScalingMenu {
    static let shared = BeamItScalingMenu()
    weak var controller: BeamItStreamWindowController? {
        didSet { allowsScaling = controller?.allowsScaling ?? false }
    }
    var allowsScaling = false
    /// True only while a stream window is the key window; selects which menu the menu bar shows.
    var streamWindowIsKey = false
}
