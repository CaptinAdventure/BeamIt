import Foundation

struct SeededGenerator: RandomNumberGenerator {
    var state: UInt64
    mutating func next() -> UInt64 {
        state = state &* 6364136223846793005 &+ 1442695040888963407
        return state
    }
}

@main
struct AntiIdleTests {
    static func main() {
        func close(_ p: CGPoint, _ x: Double, _ y: Double) -> Bool { abs(p.x - x) < 0.001 && abs(p.y - y) < 0.001 }
        // Area diameter 216 = 20% of 1080, centred at (960, 540).
        precondition(close(BeamItAntiIdle.position(offset: .zero, width: 1920, height: 1080), 960, 540))
        precondition(close(BeamItAntiIdle.position(offset: CGPoint(x: 1, y: 0), width: 1920, height: 1080), 1068, 540))
        precondition(close(BeamItAntiIdle.position(offset: CGPoint(x: 0, y: -1), width: 1080, height: 1920), 540, 852))

        let a = CGPoint(x: -1, y: 0), b = CGPoint(x: 1, y: 0)
        precondition(close(BeamItAntiIdle.offset(from: a, to: b, progress: 0), -1, 0))
        precondition(close(BeamItAntiIdle.offset(from: a, to: b, progress: 0.5), 0, 0))
        precondition(close(BeamItAntiIdle.offset(from: a, to: b, progress: 1.5), 1, 0))
        // Eased: slow near the endpoints.
        precondition(BeamItAntiIdle.offset(from: a, to: b, progress: 0.1).x < -0.9)

        var generator = SeededGenerator(state: 42)
        for _ in 0..<10_000 {
            let p = BeamItAntiIdle.randomOffset(using: &generator)
            precondition(p.x * p.x + p.y * p.y <= 1.000001)
        }
        print("Anti-idle checks passed")
    }
}
