import SwiftUI
import SystemExtensions

private let extensionID = "net.mintopia.hd60s.camera"

final class Installer: NSObject, ObservableObject, OSSystemExtensionRequestDelegate {
    @Published var status = "Run from /Applications, then click Install."

    func submit(_ request: OSSystemExtensionRequest) {
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    func install() {
        submit(.activationRequest(forExtensionWithIdentifier: extensionID, queue: .main))
    }

    func uninstall() {
        submit(.deactivationRequest(forExtensionWithIdentifier: extensionID, queue: .main))
    }

    func request(_ request: OSSystemExtensionRequest, actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        status = "Approve it in System Settings > General > Login Items & Extensions > Camera Extensions."
    }

    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        status = result == .completed ? "Done." : "Done; restart to finish."
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        status = "Failed: \(error.localizedDescription)"
    }
}

@main
struct HD60SApp: App {
    @StateObject private var installer = Installer()

    var body: some Scene {
        WindowGroup("HD60S") {
            VStack(spacing: 12) {
                Text("Elgato HD60 S camera")
                    .font(.headline)
                HStack {
                    Button("Install", action: installer.install)
                    Button("Uninstall", action: installer.uninstall)
                }
                Text(installer.status)
                    .font(.callout)
                    .multilineTextAlignment(.center)
            }
            .padding(24)
            .frame(minWidth: 420)
        }
    }
}
