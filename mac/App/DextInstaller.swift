import Foundation
import SystemExtensions

// Driver extension requests run beside the camera's, with their own delegate and status.
final class DextInstaller: NSObject, ObservableObject, OSSystemExtensionRequestDelegate {
    static let identifier = "net.mintopia.hd60s.dext"
    @Published var status = DextStatus.notBundled
    private var installing = true
    var resendSettingsOnActivation: (() -> Void)?

    var bundled: Bool {
        let url = Bundle.main.bundleURL.appendingPathComponent("Contents/Library/SystemExtensions/\(Self.identifier).dext")
        return FileManager.default.fileExists(atPath: url.path)
    }

    func refresh() {
        guard bundled else { return status = .notBundled }
        submit(.propertiesRequest(forExtensionWithIdentifier: Self.identifier, queue: .main))
    }

    func activate() {
        guard bundled else { return }
        installing = true
        submit(.activationRequest(forExtensionWithIdentifier: Self.identifier, queue: .main))
    }

    func deactivate() {
        guard bundled else { return }
        installing = false
        submit(.deactivationRequest(forExtensionWithIdentifier: Self.identifier, queue: .main))
    }

    private func submit(_ request: OSSystemExtensionRequest) {
        status = .working
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    func request(_ request: OSSystemExtensionRequest, foundProperties properties: [OSSystemExtensionProperties]) {
        let live = properties.filter { !$0.isUninstalling }
        status = .found(enabled: live.contains { $0.isEnabled }, awaitingApproval: live.contains { $0.isAwaitingUserApproval })
        if status == .installed { status = activatedStatus() }
    }

    private func activatedStatus() -> DextStatus {
        let name = "\(Self.identifier).dext"
        let running = CameraProcess.runningPaths(of: name).compactMap { path in
            path.range(of: name).flatMap { NSDictionary(contentsOfFile: path[..<$0.upperBound] + "/Info.plist")?["CFBundleVersion"] as? String }
        }
        let bundled = Bundle(url: Bundle.main.bundleURL.appendingPathComponent("Contents/Library/SystemExtensions/\(name)"))?
            .object(forInfoDictionaryKey: "CFBundleVersion") as? String
        return .activated(runningBuilds: running, bundled: bundled)
    }

    func request(_ request: OSSystemExtensionRequest, actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        existing.bundleVersion == ext.bundleVersion ? .cancel : .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) { status = .needsApproval }

    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        status = installing ? activatedStatus() : .notInstalled
        if installing { resendSettingsOnActivation?() }
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        let code = (error as? OSSystemExtensionError)?.code
        if !installing, code == .extensionNotFound { return status = .notInstalled }
        // Cancelled replacing the same build, which is already active.
        if installing, code == .requestCanceled { return status = activatedStatus() }
        status = .failed(error.localizedDescription)
    }
}
