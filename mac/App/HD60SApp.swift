import SwiftUI
import CoreText
import SystemExtensions

private let extensionID = "net.mintopia.hd60s.camera"
let appName = "Elgato HD60 S"
private let repoURL = URL(string: "https://github.com/applied-chaos/elgato-hd60s-mac")!
private let siteURL = URL(string: "https://appliedentropy.dev")!

enum Status: Equatable {
    case notInstalled, working, needsApproval, installed, needsRestart, cancelled, failed(String)

    var title: String {
        switch self {
        case .notInstalled: "Not installed"
        case .working: "Working"
        case .needsApproval: "Approval needed"
        case .installed: "Installed"
        case .needsRestart: "Restart required"
        case .cancelled: "Cancelled"
        case .failed: "Something went wrong"
        }
    }

    var detail: String {
        switch self {
        case .notInstalled: "Run from /Applications, then click Install."
        case .working: "Waiting for macOS to finish the request."
        case .needsApproval: "Approve it in System Settings > General > Login Items & Extensions > Camera Extensions."
        case .installed: "The HD60 S now appears as a camera and microphone in your apps."
        case .needsRestart: "Restart your Mac to finish."
        case .cancelled: "Administrator permission was not given. Nothing was changed."
        case .failed(let message): message
        }
    }

    var colour: Color {
        switch self {
        case .notInstalled, .working: Color(hex: 0x6B7280)
        case .needsApproval, .needsRestart, .cancelled: Color(hex: 0xFBBF24)
        case .installed: Color(hex: 0x4ADE80)
        case .failed: Color(hex: 0xF87171)
        }
    }

    var canInstall: Bool {
        switch self {
        case .notInstalled, .cancelled, .failed: true
        case .working, .needsApproval, .installed, .needsRestart: false
        }
    }

    var cameraInstalled: Bool { self == .installed || self == .needsApproval }

    static func preview(_ name: String) -> Status? {
        switch name {
        case "notInstalled": .notInstalled
        case "working": .working
        case "needsApproval": .needsApproval
        case "installed": .installed
        case "needsRestart": .needsRestart
        case "cancelled": .cancelled
        case "failed": .failed("The extension could not be loaded.")
        default: nil
        }
    }
}

extension Color {
    init(hex: UInt32) {
        self.init(red: Double(hex >> 16 & 0xFF) / 255, green: Double(hex >> 8 & 0xFF) / 255, blue: Double(hex & 0xFF) / 255)
    }

    static let ink = Color(hex: 0x0B0D17)
    static let canvas = Color(hex: 0xF8FAFC)
    static let muted = Color(hex: 0x9CA3AF)
    static let border = Color(hex: 0xD1D5DB)
    static let neutral = Color(hex: 0x6B7280)
    static let cyan = Color(hex: 0x06B6D4)
    static let hairline = Color(hex: 0xE5E7EB)
    static let amber = Color(hex: 0xB45309)
    static let link = Color(hex: 0x0E7490)
    static let linkUnderline = Color(hex: 0xA5F3FC)
    static let ok = Color(hex: 0x4ADE80)
    static let warn = Color(hex: 0xFBBF24)
    static let bad = Color(hex: 0xF87171)
}

extension Font {
    static func display(_ size: CGFloat) -> Font { .custom("Red Hat Display", size: size).weight(.heavy) }
    static func text(_ size: CGFloat, _ weight: Weight = .regular) -> Font { .custom("Red Hat Text", size: size).weight(weight) }
}

struct BrandButton: ButtonStyle {
    let primary: Bool
    @State private var hovering = false
    @Environment(\.isEnabled) private var enabled

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.text(14, .semibold))
            .foregroundStyle(Color.ink)
            .padding(.horizontal, 22)
            .frame(height: 38)
            .background(primary ? Color.cyan.opacity(configuration.isPressed ? 0.75 : hovering ? 0.9 : 1)
                                : Color.ink.opacity(configuration.isPressed ? 0.1 : hovering ? 0.05 : 0),
                        in: RoundedRectangle(cornerRadius: 10))
            .overlay { if !primary { RoundedRectangle(cornerRadius: 10).strokeBorder(Color.border) } }
            .opacity(enabled ? 1 : 0.4)
            .onHover { hovering = $0 }
    }
}

final class Installer: NSObject, ObservableObject, OSSystemExtensionRequestDelegate {
    static let shared = Installer()
    @Published var status = Status.working
    @Published var audio = AudioStatus.notInstalled

    let dext = DextInstaller()
    let flavour = Flavour.current
    private var installing = true
    private var query: OSSystemExtensionRequest?

    func refresh() {
        refreshAudio()
        dext.refresh()
        let request = OSSystemExtensionRequest.propertiesRequest(forExtensionWithIdentifier: extensionID, queue: .main)
        query = request
        submit(request)
    }

    func request(_ request: OSSystemExtensionRequest, foundProperties properties: [OSSystemExtensionProperties]) {
        let live = properties.filter { !$0.isUninstalling }
        status = live.contains { $0.isEnabled } ? .installed
            : live.contains { $0.isAwaitingUserApproval } ? .needsApproval : .notInstalled
    }

    func submit(_ request: OSSystemExtensionRequest) {
        status = .working
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    func refreshAudio() {
        let installed = Bundle(path: AudioPlugin.halDirectory + "/" + AudioPlugin.name)?.object(forInfoDictionaryKey: "CFBundleVersion") as? String
        let bundled = bundledAudio.flatMap { Bundle(url: $0)?.object(forInfoDictionaryKey: "CFBundleVersion") as? String } ?? "0"
        audio = AudioPlugin.status(installed: installed, bundled: bundled)
    }

    private var bundledAudio: URL? { Bundle.main.builtInPlugInsURL?.appendingPathComponent(AudioPlugin.name) }

    // One admin prompt; the extension request only follows if it succeeds, so a cancel changes nothing.
    func install() {
        installing = true
        let activate = {
            self.dext.activate()
            self.submit(.activationRequest(forExtensionWithIdentifier: extensionID, queue: .main))
        }
        switch flavour.installStep(audio: audio, plugInSource: bundledAudio?.path) {
        case .none: activate()
        case .admin(let script): runAsAdmin(script, then: activate)
        case .missingPlugIn: status = .failed("The audio plug-in is missing from the app.")
        }
    }

    var canInstall: Bool {
        status.canInstall || flavour.offersReinstall(cameraInstalled: status == .installed, audio: audio, dext: dext.status)
    }

    func uninstall() {
        installing = false
        let deactivate = {
            self.dext.deactivate()
            self.submit(.deactivationRequest(forExtensionWithIdentifier: extensionID, queue: .main))
        }
        switch flavour.uninstallStep(audio: audio) {
        case .admin(let script): runAsAdmin(script, then: deactivate)
        case .none, .missingPlugIn: deactivate()
        }
    }

    private func runAsAdmin(_ script: String, then next: @escaping () -> Void) {
        status = .working
        DispatchQueue.global().async {
            let process = Process()
            let stderr = Pipe()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
            process.arguments = ["-e", AudioPlugin.appleScript(for: script)]
            process.standardError = stderr
            process.standardOutput = FileHandle.nullDevice
            let outcome: AdminOutcome
            do {
                try process.run()
                let message = String(decoding: stderr.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
                process.waitUntilExit()
                outcome = AudioPlugin.outcome(exitCode: process.terminationStatus, stderr: message)
            } catch {
                outcome = .failed(error.localizedDescription)
            }
            DispatchQueue.main.async {
                self.refreshAudio()
                switch outcome {
                case .done: next()
                case .cancelled: self.status = .cancelled
                case .failed(let message): self.status = .failed(message)
                }
            }
        }
    }

    func request(_ request: OSSystemExtensionRequest, actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        status = .needsApproval
    }

    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        guard request !== query else { return }
        status = result == .completed ? (installing ? .installed : .notInstalled) : .needsRestart
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        if !installing, (error as? OSSystemExtensionError)?.code == .extensionNotFound { return refresh() }
        status = .failed(error.localizedDescription)
    }
}

enum Tab: CaseIterable {
    case install, status, settings

    var label: String {
        switch self {
        case .install: "Install"
        case .status: "Status"
        case .settings: "Settings"
        }
    }
}

// The hidden title bar still takes this much of the window, so designed offsets from the top are reduced by it.
private let titleBarHeight: CGFloat = 32

private let rainbow = LinearGradient(colors: [0xC21CF0, 0xFF2D8A, 0xFF5A2D, 0xFF9A00, 0xFFE000, 0x3EE05A, 0x00C8FF, 0x3D5AFE, 0x8E2DE2].map { Color(hex: $0) },
                                     startPoint: .leading, endPoint: .trailing)

extension StatusView.Dot {
    var colour: Color {
        switch self {
        case .ok: .ok
        case .warn: .warn
        case .bad: .bad
        }
    }
}

struct TabSwitch: View {
    @Binding var selection: Tab

    var body: some View {
        HStack(spacing: 2) {
            ForEach(Tab.allCases, id: \.self) { tab in
                Button { selection = tab } label: {
                    Text(tab.label).font(.text(12, .semibold))
                        .foregroundStyle(selection == tab ? Color.ink : Color.muted)
                        .padding(.horizontal, 14).frame(height: 24)
                        .background(selection == tab ? Color.cyan : .clear, in: RoundedRectangle(cornerRadius: 6))
                        .contentShape(RoundedRectangle(cornerRadius: 6))
                }
                .buttonStyle(.plain)
                .accessibilityAddTraits(selection == tab ? .isSelected : [])
            }
        }
        .padding(3)
        .background(Color.canvas.opacity(0.08), in: RoundedRectangle(cornerRadius: 9))
    }
}

struct StateBlock: View {
    let dot: Color
    let title: String
    let detail: String
    var extra: [String] = []

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Circle().fill(dot).frame(width: 10, height: 10).padding(.top, 6)
            VStack(alignment: .leading, spacing: 3) {
                Text(title).font(.text(16, .semibold)).foregroundStyle(Color.ink)
                Text(detail).font(.text(13)).foregroundStyle(Color.neutral).frame(maxWidth: 452, alignment: .leading)
                ForEach(extra, id: \.self) { Text($0).font(.text(13)).foregroundStyle(Color.neutral) }
            }
            .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
        }
        .accessibilityElement(children: .combine)
    }
}

struct FactsColumn: View {
    let heading: String
    let rows: [StatusView.Row]

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(heading).font(.text(13, .semibold)).foregroundStyle(Color.ink)
            Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 6) {
                ForEach(rows, id: \.label) { row in
                    GridRow {
                        Text(row.label).foregroundStyle(Color.neutral)
                        Text(row.value).monospacedDigit()
                            .foregroundStyle(row.flag ? Color.amber : Color.ink)
                            .fontWeight(row.flag ? .semibold : .regular)
                    }
                    .font(.text(13))
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .topLeading)
    }
}

struct StatusTab: View {
    @ObservedObject var poller: StatusPoller

    var body: some View {
        if let v = poller.view {
            StateBlock(dot: v.dot.colour, title: v.title, detail: v.detail)
            if !v.connection.isEmpty {
                HStack(alignment: .top, spacing: 32) {
                    if !v.input.isEmpty { FactsColumn(heading: "Input", rows: v.input) }
                    FactsColumn(heading: "Connection", rows: v.connection)
                }
                .padding(.top, 28)
            }
        } else {
            StateBlock(dot: .neutral, title: "Checking", detail: "Looking for the HD60 S.")
        }
    }
}

struct InstallTab: View {
    @ObservedObject var installer: Installer
    @ObservedObject var dext: DextInstaller

    private var audioDiffers: Bool { installer.audio != (installer.status == .installed ? .installed : .notInstalled) }

    var body: some View {
        let status = installer.status
        var extra: [String] = []
        if installer.flavour.showsAudioPlugIn && audioDiffers { extra.append("Audio plug-in: \(installer.audio.label)") }
        if let label = dext.status.label { extra.append("Driver extension: \(label)") }
        return VStack(alignment: .leading, spacing: 0) {
            StateBlock(dot: status.colour, title: status.title, detail: status.detail, extra: extra)
            Group {
                switch InstallAction.offered(camera: status.cameraInstalled, audio: installer.audio, dext: dext.status) {
                case .uninstall:
                    Button("Uninstall", action: installer.uninstall).buttonStyle(BrandButton(primary: false))
                        .disabled(status == .working)
                case .install:
                    Button("Install", action: installer.install).buttonStyle(BrandButton(primary: true))
                        .keyboardShortcut(.defaultAction).disabled(!installer.canInstall)
                }
            }
            .padding(.top, 24).padding(.leading, 22)
        }
    }
}

struct MainWindow: View {
    @StateObject private var installer = Installer.shared
    @StateObject private var dext = Installer.shared.dext
    @StateObject private var poller = StatusPoller(preview: HD60SApp.launchArguments.value(after: "--preview-facts").flatMap(PreviewFacts.init))
    @StateObject private var settings = SettingsStore(url: SettingsFile.url(), preview: HD60SApp.launchArguments.value(after: "--preview-settings").flatMap(PictureAudioSettings.init(json:)))
    @State private var tab = Tab.install
    @State private var chosen = HD60SApp.launchArguments.value(after: "--preview-tab") != nil

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(alignment: .center, spacing: 16) {
                Image("Mark").resizable().scaledToFit().frame(width: 44, height: 53)
                VStack(alignment: .leading, spacing: 4) {
                    Text(appName).font(.display(28)).tracking(-0.7).foregroundStyle(Color.canvas).frame(height: 31)
                    Text("Camera and microphone for your Mac").font(.text(13)).foregroundStyle(Color.muted).frame(height: 16)
                }
                Spacer(minLength: 0)
            }

            .padding(.horizontal, 36).padding(.top, 54 - titleBarHeight).padding(.bottom, 26)
            .frame(maxWidth: .infinity, alignment: .leading)
            .frame(height: 54 - titleBarHeight + 53 + 26)
            .background(Color.ink.ignoresSafeArea(edges: .top))
            rainbow.frame(height: 2)

            VStack(alignment: .leading, spacing: 0) {
                switch tab {
                case .install: InstallTab(installer: installer, dext: dext)
                case .status: StatusTab(poller: poller)
                case .settings: SettingsTab(store: settings)
                }
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 36).padding(.top, 28)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)

            Rectangle().fill(Color.hairline).frame(height: 1)
            HStack(spacing: 8) {
                Image("MarkSmall").resizable().scaledToFit().frame(height: 18)
                Text("An Applied Entropy project").font(.text(12)).foregroundStyle(Color.neutral)
            }
            .padding(.horizontal, 36).frame(height: 47)
        }
        .frame(width: 560, height: 490 - titleBarHeight, alignment: .topLeading)
        .background(Color.canvas)
        .overlay(alignment: .top) {
            TabSwitch(selection: Binding(get: { tab }, set: { tab = $0; chosen = true }))
                .offset(y: 8 - titleBarHeight)
        }
        .preferredColorScheme(.light)
        .onAppear {
            if let name = HD60SApp.launchArguments.value(after: "--preview-tab") { tab = name == "status" ? .status : name == "settings" ? .settings : .install }
            if tab == .status { poller.start() }
            dext.onActivated = { [settings] in settings.pushToDriver() }
        }
        .onDisappear { poller.stop() }
        .onChange(of: tab) { _, new in new == .status ? poller.start() : poller.stop() }
        .onChange(of: installer.status) { _, new in
            if !chosen, new != .working { tab = new == .installed ? .status : .install }
        }
    }
}

struct AboutWindow: View {
    private var version: String {
        let info = Bundle.main.infoDictionary
        return "Version \(info?["CFBundleShortVersionString"] as? String ?? "0") (\(info?["CFBundleVersion"] as? String ?? "0"))"
    }

    private var description: String {
        let dextBuild = HD60SApp.launchArguments.value(after: "--preview-flavour").map { $0 == "dext" } ?? (Flavour.current == .driverKit)
        return dextBuild ? "DriverKit development build for\nthe HD60 S on Apple silicon Macs." : "Userspace driver for the\nHD60 S on Apple silicon Macs."
    }

    var body: some View {
        VStack(spacing: 0) {
            VStack(spacing: 0) {
                Image("Mark").resizable().scaledToFit().frame(width: 64, height: 77)
                Text(appName).font(.display(24)).tracking(-0.6).foregroundStyle(Color.canvas).padding(.top, 16)
                Text(version).font(.text(13)).monospacedDigit().foregroundStyle(Color.muted).padding(.top, 6)
            }
            .padding(.horizontal, 24).padding(.top, 48 - titleBarHeight).padding(.bottom, 26)
            .frame(maxWidth: .infinity)
            .background(Color.ink.ignoresSafeArea(edges: .top))
            rainbow.frame(height: 2)
            VStack(spacing: 0) {
                Text(description).font(.text(13)).foregroundStyle(Color.ink).lineSpacing(2.5).fixedSize(horizontal: false, vertical: true).padding(.bottom, 18.5)
                VStack(spacing: 2) {
                    AboutLink("An Applied Entropy project", siteURL).frame(height: 18)
                    HStack(spacing: 3.5) {
                        Text("MIT licence ·")
                        AboutLink("Source on GitHub", repoURL)
                    }
                    .frame(height: 18)
                    Text("Not affiliated with Elgato or Corsair.").frame(height: 18)
                }
                .font(.text(12)).foregroundStyle(Color.neutral)
            }
            .multilineTextAlignment(.center)
            .padding(.horizontal, 28).padding(.vertical, 24)
        }
        .frame(width: 320)
        .background(Color.canvas)
        .preferredColorScheme(.light)
    }
}

struct AboutLink: View {
    let label: String
    let url: URL
    init(_ label: String, _ url: URL) { (self.label, self.url) = (label, url) }

    var body: some View {
        Link(destination: url) {
            Text(label).font(.text(12)).foregroundStyle(Color.link)
                .padding(.bottom, 1)
                .overlay(alignment: .bottom) { Rectangle().fill(Color.linkUnderline).frame(height: 1) }
        }
        .buttonStyle(.plain)
    }
}

@main
struct HD60SApp: App {
    static let launchArguments = LaunchArguments(CommandLine.arguments)
    @Environment(\.openWindow) private var openWindow

    init() {
        let args = Self.launchArguments
        for name in ["RedHatDisplay", "RedHatText"] {
            if let url = Bundle.main.url(forResource: name, withExtension: "ttf") {
                CTFontManagerRegisterFontsForURL(url as CFURL, .process, nil)
            }
        }
        if let s = args.value(after: "--preview-state").flatMap(Status.preview) {
            Installer.shared.status = s
        } else if args.contains("--install") {
            Installer.shared.install()
        } else {
            Installer.shared.refresh()
        }
    }

    var body: some Scene {
        WindowGroup(appName) {
            MainWindow().onAppear {
                if Self.launchArguments.contains("--preview-about") { openWindow(id: "about") }
            }
        }
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentSize)
        .commands {
            CommandGroup(replacing: .appInfo) {
                Button("About \(appName)") { openWindow(id: "about") }
            }
        }
        Window("About \(appName)", id: "about") { AboutWindow() }
            .windowStyle(.hiddenTitleBar)
            .windowResizability(.contentSize)
    }
}

struct LaunchArguments {
    let all: [String]
    init(_ all: [String]) { self.all = all }
    func contains(_ flag: String) -> Bool { all.contains(flag) }
    func value(after flag: String) -> String? { all.firstIndex(of: flag).flatMap { $0 + 1 < all.count ? all[$0 + 1] : nil } }
}
