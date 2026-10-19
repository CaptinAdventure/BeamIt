import Observation

@Observable
final class BeamItScalingMenu {
    static let shared = BeamItScalingMenu()
    weak var controller: BeamItStreamWindowController? {
        didSet { allowsScaling = controller?.allowsScaling ?? false }
    }
    var allowsScaling = false
}
