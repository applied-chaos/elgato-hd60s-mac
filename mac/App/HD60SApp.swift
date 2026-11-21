import SwiftUI
import CoreText
import SystemExtensions

private let extensionID = "net.mintopia.hd60s.camera"

enum Status: Equatable {
    case notInstalled, working, needsApproval, installed, needsRestart, failed(String)

    var title: String {
        switch self {
        case .notInstalled: "Not installed"
        case .working: "Working"
        case .needsApproval: "Approval needed"
        case .installed: "Installed"
        case .needsRestart: "Restart required"
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
        case .failed(let message): message
        }
    }

    var colour: Color {
        switch self {
        case .notInstalled, .working: Color(hex: 0x6B7280)
        case .needsApproval, .needsRestart: Color(hex: 0xFBBF24)
        case .installed: Color(hex: 0x4ADE80)
        case .failed: Color(hex: 0xF87171)
        }
    }

    var canInstall: Bool {
        switch self {
        case .notInstalled, .failed: true
        case .working, .needsApproval, .installed, .needsRestart: false
        }
    }

    var canUninstall: Bool {
        switch self {
        case .needsApproval, .installed, .failed: true
        case .notInstalled, .working, .needsRestart: false
        }
    }

    static func preview(_ name: String) -> Status? {
        switch name {
        case "notInstalled": .notInstalled
        case "working": .working
        case "needsApproval": .needsApproval
        case "installed": .installed
        case "needsRestart": .needsRestart
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

    private var installing = true
    private var query: OSSystemExtensionRequest?

    func refresh() {
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

    func install() {
        installing = true
        submit(.activationRequest(forExtensionWithIdentifier: extensionID, queue: .main))
    }

    func uninstall() {
        installing = false
        submit(.deactivationRequest(forExtensionWithIdentifier: extensionID, queue: .main))
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
        status = .failed(error.localizedDescription)
    }
}

@main
struct HD60SApp: App {
    @StateObject private var installer = Installer.shared

    init() {
        let args = CommandLine.arguments
        for name in ["RedHatDisplay", "RedHatText"] {
            if let url = Bundle.main.url(forResource: name, withExtension: "ttf") {
                CTFontManagerRegisterFontsForURL(url as CFURL, .process, nil)
            }
        }
        if let i = args.firstIndex(of: "--preview-state"), i + 1 < args.count, let s = Status.preview(args[i + 1]) {
            Installer.shared.status = s
        } else if args.contains("--install") {
            Installer.shared.install()
        } else {
            Installer.shared.refresh()
        }
    }

    var body: some Scene {
        WindowGroup("HD60 S Camera") {
            VStack(alignment: .leading, spacing: 0) {
                HStack(spacing: 20) {
                    Image("Mark").resizable().scaledToFit().frame(width: 56, height: 56 * 1.2)
                    VStack(alignment: .leading, spacing: 4) {
                        Text("HD60 S Camera").font(.display(30)).tracking(-0.7).foregroundStyle(Color.canvas)
                        Text("Use your Elgato HD60 S as a camera and microphone.")
                            .font(.text(14)).foregroundStyle(Color.muted)
                    }
                }
                .padding(.horizontal, 36).padding(.top, 44).padding(.bottom, 30)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Color.ink)
                LinearGradient(colors: [0xC21CF0, 0xFF2D8A, 0xFF5A2D, 0xFF9A00, 0xFFE000, 0x3EE05A, 0x00C8FF, 0x3D5AFE, 0x8E2DE2].map { Color(hex: $0) },
                               startPoint: .leading, endPoint: .trailing)
                    .frame(height: 2).padding(.bottom, 28)

                VStack(alignment: .leading, spacing: 0) {
                let status = installer.status
                HStack(alignment: .top, spacing: 12) {
                    Circle().fill(status.colour).frame(width: 10, height: 10).padding(.top, 5)
                    VStack(alignment: .leading, spacing: 4) {
                        Text(status.title).font(.text(16, .semibold)).foregroundStyle(Color.ink)
                        Text(status.detail).font(.text(13)).foregroundStyle(Color.neutral)
                            .lineLimit(3)
                    }
                    Spacer(minLength: 0)
                }
                .padding(.vertical, 2)
                .frame(maxWidth: .infinity, minHeight: 80, maxHeight: 80, alignment: .topLeading)
                .accessibilityElement(children: .combine)

                HStack(spacing: 12) {
                    Button("Install", action: installer.install).buttonStyle(BrandButton(primary: true))
                        .keyboardShortcut(.defaultAction).disabled(!status.canInstall)
                    Button("Uninstall", action: installer.uninstall).buttonStyle(BrandButton(primary: false))
                        .disabled(!status.canUninstall)
                }
                .padding(.top, 22)

                Spacer(minLength: 0)
                HStack(spacing: 8) {
                    Image("MarkSmall").resizable().scaledToFit().frame(height: 18)
                    Text("An Applied Entropy project").font(.text(12)).foregroundStyle(Color.neutral)
                }
                }
                .padding(.horizontal, 36).padding(.bottom, 28)
            }
            .frame(width: 560, height: 400, alignment: .topLeading)
            .background(Color.canvas)
            .preferredColorScheme(.light)
        }
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentSize)
    }
}
