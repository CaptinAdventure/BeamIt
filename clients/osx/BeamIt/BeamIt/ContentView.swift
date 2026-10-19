import SwiftUI

struct ContentView: View {
    @AppStorage("clipboardSyncEnabled") private var clipboardSyncEnabled = false
    @State private var serverURL = UserDefaults.standard.string(forKey: "lastSuccessfulServerURL") ?? ""
    @State private var password = ""
    @State private var certificateFingerprint = UserDefaults.standard.string(forKey: "lastSuccessfulCertificateFingerprint") ?? ""
    @State private var trustAnyCertificate = UserDefaults.standard.bool(forKey: "lastSuccessfulTrustAnyCertificate")
    @State private var connection: BeamItConnection?
    @State private var applications: [BeamItApplication] = []
    @State private var selectedSource: BeamItSource?
    @State private var isConnecting = false
    @State private var isStartingStream = false
    @State private var connectionError: String?
    @State private var streamWindows: [String: BeamItStreamWindowController] = [:]
    @State private var applicationSearch = ""

    private var canConnect: Bool {
        guard
            let components = URLComponents(string: serverURL.trimmingCharacters(in: .whitespacesAndNewlines)),
            ["wss", "https"].contains(components.scheme?.lowercased() ?? ""),
            let host = components.host,
            !host.isEmpty,
            !password.isEmpty
        else {
            return false
        }

        guard !trustAnyCertificate else { return true }
        let fingerprint = certificateFingerprint.filter { $0 != ":" && !$0.isWhitespace }
        return fingerprint.count == 64 && fingerprint.allSatisfy(\.isHexDigit)
    }

    var body: some View {
        VStack {
            Spacer(minLength: 0)

            if connection == nil {
                connectScreen
            } else {
                sourceScreen
            }

            Spacer(minLength: 0)
        }
        .padding(24)
        .onChange(of: clipboardSyncEnabled) { _, enabled in
            connection?.clipboardSync.enabled = enabled
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background {
            LinearGradient(
                colors: [Color.accentColor.opacity(0.08), .clear],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )
            .ignoresSafeArea()
        }
        .background(BeamItMainWindowAccessor().frame(width: 0, height: 0))
    }

    private var connectScreen: some View {
        VStack(alignment: .leading, spacing: 26) {
            VStack(alignment: .leading, spacing: 12) {
                Image(systemName: "rectangle.connected.to.line.below")
                    .font(.system(size: 30, weight: .medium))
                    .foregroundStyle(.tint)
                    .frame(width: 58, height: 58)
                    .background(.tint.opacity(0.1), in: RoundedRectangle(cornerRadius: 16))

                VStack(alignment: .leading, spacing: 6) {
                    Text("Connect to BeamIt")
                        .font(.system(size: 28, weight: .semibold))
                        .foregroundStyle(.primary)

                    Text("Enter the address, shared password, and certificate fingerprint for your BeamIt host.")
                        .font(.body)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }

            VStack(alignment: .leading, spacing: 18) {
                VStack(alignment: .leading, spacing: 8) {
                    Text("SERVER URL")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.secondary)

                    TextField("wss://192.168.1.20:17389", text: $serverURL)
                        .textFieldStyle(.roundedBorder)
                        .font(.body.monospaced())
                        .accessibilityLabel("Server URL")

                    Text("The secure WebSocket address of your host.")
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                }

                VStack(alignment: .leading, spacing: 8) {
                    Text("PASSWORD")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.secondary)

                    SecureField("Shared host password", text: $password)
                        .textFieldStyle(.roundedBorder)
                        .accessibilityLabel("Password")
                }

                VStack(alignment: .leading, spacing: 8) {
                    Text("CERTIFICATE SHA-256")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.secondary)

                    TextField("AA:BB:CC:…", text: $certificateFingerprint)
                        .textFieldStyle(.roundedBorder)
                        .font(.body.monospaced())
                        .accessibilityLabel("Certificate SHA-256 fingerprint")
                        .disabled(trustAnyCertificate)

                    Text(trustAnyCertificate
                         ? "Certificate checks are disabled. Only use this on a network you trust."
                         : "Copy the fingerprint from the BeamIt host log and verify it before connecting.")
                        .font(.caption)
                        .foregroundStyle(trustAnyCertificate ? Color.orange : Color.secondary.opacity(0.7))
                        .fixedSize(horizontal: false, vertical: true)
                }

                Toggle("Trust any server certificate (not recommended)", isOn: $trustAnyCertificate)
                    .toggleStyle(.checkbox)
            }

            if let connectionError {
                Label(connectionError, systemImage: "exclamationmark.triangle.fill")
                    .font(.callout)
                    .foregroundStyle(.red)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityAddTraits(.updatesFrequently)
            }

            Button(action: connect) {
                HStack(spacing: 8) {
                    if isConnecting {
                        ProgressView()
                            .controlSize(.small)
                    }
                    Text(isConnecting ? "Connecting…" : "Connect")
                    if !isConnecting {
                        Image(systemName: "arrow.right")
                    }
                }
                .font(.body.weight(.semibold))
                .frame(maxWidth: .infinity)
                .padding(.vertical, 10)
            }
            .buttonStyle(.borderedProminent)
            .disabled(!canConnect || isConnecting)
        }
        .padding(32)
        .frame(maxWidth: 440)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 24))
        .overlay {
            RoundedRectangle(cornerRadius: 24)
                .strokeBorder(.primary.opacity(0.08), lineWidth: 1)
        }
    }

    private var sourceScreen: some View {
        VStack(alignment: .leading, spacing: 22) {
            HStack(alignment: .top) {
                VStack(alignment: .leading, spacing: 6) {
                    Text("What do you want to stream?")
                        .font(.system(size: 26, weight: .semibold))
                    Text("Choose a desktop capture method or an application window.")
                        .font(.body)
                        .foregroundStyle(.secondary)
                }

                Spacer()

                Button("Disconnect", action: disconnect)
                    .buttonStyle(.bordered)
            }

            if streamWindows.values.contains(where: { $0.sourceKind == "application" }) {
                Menu("Add another stream") {
                    ForEach(applications) { application in
                        Button(application.title) {
                            startStream(.application(application.pid))
                        }
                    }
                    if applications.isEmpty {
                        Text("No applications available")
                    }
                }
                .disabled(isStartingStream)
            }

            if let connectionError {
                Label(connectionError, systemImage: "exclamationmark.triangle.fill")
                    .font(.callout)
                    .foregroundStyle(.red)
                    .fixedSize(horizontal: false, vertical: true)
            }

            if isStartingStream {
                Label("Starting stream…", systemImage: "arrow.triangle.2.circlepath")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            } else if !streamWindows.isEmpty {
                VStack(alignment: .leading, spacing: 6) {
                    Label(
                        "\(streamWindows.count) active stream\(streamWindows.count == 1 ? "" : "s") · ⌘W closes the focused stream",
                        systemImage: "dot.radiowaves.left.and.right"
                    )
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    ForEach(streamWindows.keys.sorted(), id: \.self) { streamID in
                        if let window = streamWindows[streamID] {
                            HStack {
                                Text(window.title)
                                    .lineLimit(1)
                                Spacer()
                                Button("Show") { window.show() }
                                    .buttonStyle(.borderless)
                                Button("Stop") { window.close() }
                                    .buttonStyle(.borderless)
                                    .foregroundStyle(Color.red)
                            }
                            .font(.callout)
                        }
                    }
                }
            }

            VStack(alignment: .leading, spacing: 10) {
                Text("DESKTOP")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.secondary)

                sourceRow(
                    title: "Desktop · DXGI",
                    subtitle: "Desktop Duplication",
                    symbol: "display",
                    source: .desktopDXGI
                )
                sourceRow(
                    title: "Desktop · WGC",
                    subtitle: "Windows.Graphics.Capture",
                    symbol: "rectangle.dashed",
                    source: .desktopWGC
                )
            }

            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Text("APPLICATIONS")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.secondary)
                    Spacer()
                    Text(applicationSearch.isEmpty
                         ? "\(applications.count)"
                         : "\(filteredApplications.count) of \(applications.count)")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.tertiary)
                }

                if !applications.isEmpty {
                    HStack(spacing: 6) {
                        Image(systemName: "magnifyingglass")
                            .foregroundStyle(.secondary)
                        TextField("Search applications and windows", text: $applicationSearch)
                            .textFieldStyle(.plain)
                            .onSubmit {
                                if filteredApplications.count == 1, let match = filteredApplications.first {
                                    startStream(.application(match.pid))
                                }
                            }
                        if !applicationSearch.isEmpty {
                            Button {
                                applicationSearch = ""
                            } label: {
                                Image(systemName: "xmark.circle.fill")
                                    .foregroundStyle(.secondary)
                            }
                            .buttonStyle(.plain)
                            .help("Clear search")
                        }
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 7)
                    .background(.background.opacity(0.6), in: RoundedRectangle(cornerRadius: 8))
                }

                if applications.isEmpty {
                    ContentUnavailableView(
                        "No applications found",
                        systemImage: "macwindow",
                        description: Text("The host did not report any capturable application windows.")
                    )
                    .frame(height: 150)
                } else if filteredApplications.isEmpty {
                    ContentUnavailableView.search(text: applicationSearch)
                        .frame(height: 150)
                } else {
                    ScrollView {
                        VStack(spacing: 8) {
                            ForEach(filteredApplications) { application in
                                sourceRow(
                                    title: application.title,
                                    subtitle: applicationSubtitle(application),
                                    symbol: "macwindow",
                                    source: .application(application.pid)
                                )
                            }
                        }
                    }
                    .frame(maxHeight: 260)
                }
            }

            if let selectedSource {
                Label("Selected: \(selectedTitle(for: selectedSource))", systemImage: "checkmark.circle.fill")
                    .font(.callout)
                    .foregroundStyle(.green)
                    .accessibilityAddTraits(.updatesFrequently)
            } else {
                Text("Choose a desktop or application source.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(28)
        .frame(maxWidth: 620)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 24))
        .overlay {
            RoundedRectangle(cornerRadius: 24)
                .strokeBorder(.primary.opacity(0.08), lineWidth: 1)
        }
    }

    private func sourceRow(title: String, subtitle: String, symbol: String, source: BeamItSource) -> some View {
        Button {
            selectedSource = source
            startStream(source)
        } label: {
            HStack(spacing: 12) {
                Image(systemName: symbol)
                    .font(.system(size: 18))
                    .foregroundStyle(.tint)
                    .frame(width: 36, height: 36)
                    .background(.tint.opacity(0.1), in: RoundedRectangle(cornerRadius: 10))

                VStack(alignment: .leading, spacing: 3) {
                    Text(title)
                        .font(.body.weight(.medium))
                        .lineLimit(1)
                    Text(subtitle)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }

                Spacer()

                if selectedSource == source {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(.tint)
                }
            }
            .padding(10)
            .contentShape(RoundedRectangle(cornerRadius: 12))
            .background {
                RoundedRectangle(cornerRadius: 12)
                    .fill(selectedSource == source ? Color.accentColor.opacity(0.1) : Color.primary.opacity(0.035))
            }
            .overlay {
                RoundedRectangle(cornerRadius: 12)
                    .strokeBorder(selectedSource == source ? Color.accentColor.opacity(0.35) : .clear, lineWidth: 1)
            }
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selectedSource == source ? .isSelected : [])
    }

    private func connect() {
        guard !isConnecting else { return }
        isConnecting = true
        connectionError = nil
        let address = serverURL
        let secret = password
        let fingerprint = certificateFingerprint
        let trustsAnyCertificate = trustAnyCertificate

        Task {
            var pendingConnection: BeamItConnection?
            do {
                let newConnection = try await BeamItConnection.connect(
                    serverURL: address,
                    password: secret,
                    certificateFingerprint: fingerprint,
                    trustAnyCertificate: trustsAnyCertificate
                )
                pendingConnection = newConnection
                newConnection.clipboardSync.enabled = clipboardSyncEnabled
                newConnection.clipboardSync.onError = { message in
                    connectionError = "Clipboard sync: \(message)"
                }
                let windows = try await newConnection.listWindows()
                newConnection.onStreamStopped = { streamID, reason in
                    DispatchQueue.main.async {
                        streamWindows.removeValue(forKey: streamID)?.closeAfterHostStop()
                        if let reason, reason != "requested" {
                            connectionError = "Stream stopped: \(reason)."
                        }
                        if streamWindows.isEmpty {
                            selectedSource = nil
                            BeamItMainWindow.show()
                        }
                    }
                }
                newConnection.onDisconnect = { [weak newConnection] error in
                    DispatchQueue.main.async {
                        newConnection?.disconnect()
                        for window in streamWindows.values {
                            window.closeAfterHostStop()
                        }
                        streamWindows.removeAll()
                        connection = nil
                        applications = []
                        selectedSource = nil
                        connectionError = error.localizedDescription
                        BeamItMainWindow.show()
                    }
                }
                connection = newConnection
                pendingConnection = nil
                UserDefaults.standard.set(address.trimmingCharacters(in: .whitespacesAndNewlines), forKey: "lastSuccessfulServerURL")
                UserDefaults.standard.set(fingerprint, forKey: "lastSuccessfulCertificateFingerprint")
                UserDefaults.standard.set(trustsAnyCertificate, forKey: "lastSuccessfulTrustAnyCertificate")
                applications = Self.applications(from: windows)
                selectedSource = nil
                password = ""
            } catch {
                pendingConnection?.disconnect()
                connectionError = error.localizedDescription
            }
            isConnecting = false
        }
    }

    private func disconnect() {
        for window in streamWindows.values {
            window.closeAfterHostStop()
        }
        streamWindows.removeAll()
        connection?.disconnect()
        connection = nil
        applications = []
        applicationSearch = ""
        selectedSource = nil
        connectionError = nil
        BeamItMainWindow.show()
    }

    private func startStream(_ source: BeamItSource) {
        guard let connection, !isStartingStream else { return }
        isStartingStream = true
        connectionError = nil
        Task {
            do {
                let stream = try await connection.startStream(source: source)
                let window = BeamItStreamWindowController(stream: stream)
                window.onError = { message in
                    connectionError = message
                }
                window.onClose = { [weak window] in
                    guard let window else { return }
                    DispatchQueue.main.async {
                        streamWindows.removeValue(forKey: window.stream.offer.streamID)
                        if streamWindows.isEmpty {
                            selectedSource = nil
                            BeamItMainWindow.show()
                        }
                    }
                }
                streamWindows[stream.offer.streamID] = window
                window.show()
                BeamItMainWindow.hide()
            } catch {
                connectionError = error.localizedDescription
            }
            isStartingStream = false
        }
    }

    private var searchTerms: [String] {
        applicationSearch.split(whereSeparator: \.isWhitespace).map(String.init)
    }

    /// Every search word must appear in the app title, one of its window titles, or its PID.
    private var filteredApplications: [BeamItApplication] {
        let terms = searchTerms
        guard !terms.isEmpty else { return applications }
        return applications.filter { application in
            let haystack = ([application.title, String(application.pid)] + application.windows.map(\.title))
                .joined(separator: "\n")
            return terms.allSatisfy { haystack.localizedStandardContains($0) }
        }
    }

    private func applicationSubtitle(_ application: BeamItApplication) -> String {
        let summary = "\(application.windows.count) window\(application.windows.count == 1 ? "" : "s") · PID \(application.pid)"
        let terms = searchTerms
        guard !terms.isEmpty,
              !terms.allSatisfy({ application.title.localizedStandardContains($0) }),
              let match = application.windows.first(where: { window in
                  terms.contains { window.title.localizedStandardContains($0) }
              })
        else { return summary }
        return "\(summary) · “\(match.title)”"
    }

    private func selectedTitle(for source: BeamItSource) -> String {
        if case .application(let pid) = source,
           let app = applications.first(where: { $0.pid == pid }) {
            return app.title
        }
        return source.title
    }

    private static func applications(from windows: [BeamItWindow]) -> [BeamItApplication] {
        Dictionary(grouping: windows, by: \.pid)
            .map { pid, appWindows in
                let sortedWindows = appWindows.sorted {
                    if $0.primary != $1.primary { return $0.primary && !$1.primary }
                    return $0.title.localizedStandardCompare($1.title) == .orderedAscending
                }
                let title = sortedWindows.first(where: { !$0.title.isEmpty })?.title ?? "Application"
                return BeamItApplication(pid: pid, title: title, windows: sortedWindows)
            }
            .sorted { $0.title.localizedStandardCompare($1.title) == .orderedAscending }
    }
}

#Preview {
    ContentView()
}
