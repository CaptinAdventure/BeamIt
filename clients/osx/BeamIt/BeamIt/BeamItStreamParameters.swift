import AppKit
import SwiftUI

private struct BeamItStreamParametersView: View {
    let stream: BeamItVideoStream
    let dismiss: () -> Void
    @State var bitrate: String
    @State var frameRate: String
    @State private var bitrateUnit = BitrateUnit.kbits
    @State private var changePeak = false
    @State private var automaticPeak = true
    @State private var peak = ""
    @State private var applying = false
    @State private var error: String?
    @State private var requestTask: Task<Void, Never>?

    private enum BitrateUnit: String, CaseIterable {
        case kbits = "kbit/s"
        case mbits = "Mbit/s"

        var multiplier: Double { self == .kbits ? 1 : 1000 }
    }

    private var values: (bitrate: Int, fps: Int)? {
        guard let entered = Double(bitrate), entered.isFinite else { return nil }
        let kbps = entered * bitrateUnit.multiplier
        guard (1...1_000_000).contains(kbps),
              let fps = Int(frameRate), (1...240).contains(fps) else { return nil }
        return (Int(kbps.rounded()), fps)
    }

    private var peakValue: Int? {
        guard changePeak else { return nil }
        if automaticPeak { return 0 }
        guard let entered = Double(peak), entered.isFinite else { return nil }
        let kbps = entered * bitrateUnit.multiplier
        guard (1...4_000_000).contains(kbps) else { return nil }
        return Int(kbps.rounded())
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Change Parameters").font(.headline)
            Form {
                HStack {
                    TextField("Average bitrate", text: $bitrate)
                    Picker("Units", selection: $bitrateUnit) {
                        ForEach(BitrateUnit.allCases, id: \.self) { unit in
                            Text(unit.rawValue).tag(unit)
                        }
                    }
                    .frame(width: 150)
                    .onChange(of: bitrateUnit) { oldUnit, newUnit in
                        if let value = Double(bitrate), value.isFinite {
                            bitrate = String(value * oldUnit.multiplier / newUnit.multiplier)
                        }
                        if let value = Double(peak), value.isFinite {
                            peak = String(value * oldUnit.multiplier / newUnit.multiplier)
                        }
                    }
                }
                Text(bitrateUnit == .kbits ? "1–1,000,000 kbit/s" : "0.001–1,000 Mbit/s")
                    .font(.caption).foregroundStyle(.secondary)
                Toggle("Change peak bitrate", isOn: $changePeak)
                if changePeak {
                    Toggle("Automatic peak (2× average)", isOn: $automaticPeak)
                    if !automaticPeak {
                        TextField("Peak (\(bitrateUnit.rawValue))", text: $peak)
                        Text("Up to \(bitrateUnit == .kbits ? "4,000,000 kbit/s" : "4,000 Mbit/s"); values below the average are raised to it.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                } else {
                    Text("Keep the host's peak policy. Current effective peak: \(stream.video.max_bitrate_kbps.map(String.init) ?? "unknown") kbit/s.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                HStack {
                    Slider(value: Binding(
                        get: { Double(Int(frameRate).map { min(240, max(1, $0)) } ?? 60) },
                        set: { frameRate = String(Int($0.rounded())) }
                    ), in: 1...240, step: 1) {
                        Text("Frame rate")
                    }
                    TextField("FPS", text: $frameRate)
                        .monospacedDigit()
                        .multilineTextAlignment(.trailing)
                        .frame(width: 50)
                        .accessibilityLabel("Frame rate")
                    Text("FPS")
                }
            }
            .disabled(applying)
            Text("Codec, pixel size and chroma sampling stay unchanged. Changing the frame rate restarts the encoder.")
                .font(.callout).foregroundStyle(.secondary)
            if let error {
                Text(error).foregroundStyle(.red).textSelection(.enabled)
            }
            HStack {
                if applying { ProgressView().controlSize(.small) }
                Spacer()
                Button("Cancel") {
                    requestTask?.cancel()
                    dismiss()
                }
                .keyboardShortcut(.cancelAction)
                Button("Apply") { apply() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(applying || values == nil || (changePeak && peakValue == nil))
            }
            if applying {
                Text("A submitted change cannot be undone by Cancel.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .padding(24)
        .frame(width: 460)
        .onDisappear { requestTask?.cancel() }
    }

    private func apply() {
        guard let values, !applying, !changePeak || peakValue != nil else { return }
        let requestedPeak = peakValue
        applying = true
        error = nil
        requestTask = Task {
            do {
                try await stream.setRate(bitrate: values.bitrate, frameRate: values.fps, peak: requestedPeak)
                try Task.checkCancellation()
                dismiss()
            } catch is CancellationError {
                // The sheet or stream closed while waiting for the host.
            } catch {
                guard !Task.isCancelled else { return }
                self.error = error.localizedDescription
                applying = false
            }
        }
    }
}

extension BeamItStreamWindowController {
    static func changeActiveStreamParameters() {
        guard let controller = activeController, !controller.stream.isClosed,
              let parent = controller.window else {
            NSSound.beep()
            return
        }
        guard parent.attachedSheet == nil else { return }
        let panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 508, height: 280),
            styleMask: [.titled], backing: .buffered, defer: false
        )
        panel.title = "Change Parameters — \(controller.title)"
        panel.isReleasedWhenClosed = false
        panel.contentView = NSHostingView(rootView: BeamItStreamParametersView(
            stream: controller.stream,
            dismiss: { [weak controller, weak panel] in
                guard let panel else { return }
                panel.sheetParent?.endSheet(panel)
                panel.close()
                controller?.parametersWindow = nil
            },
            bitrate: controller.stream.video.bitrate_kbps.map(String.init) ?? "",
            frameRate: String(controller.stream.video.frame_rate ?? 60)
        ))
        controller.parametersWindow = panel
        parent.beginSheet(panel)
    }
}
