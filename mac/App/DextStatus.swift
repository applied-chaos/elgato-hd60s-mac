enum DextStatus: Equatable {
    case notBundled, working, needsApproval, installed, needsReplug, notInstalled, failed(String)

    // nil hides the line: release builds do not bundle the driver extension.
    var label: String? {
        switch self {
        case .notBundled: nil
        case .working: "working"
        case .needsApproval: "approval needed"
        case .installed: "installed"
        case .needsReplug: "unplug and replug the HD60 S to finish"
        case .notInstalled: "not installed"
        case .failed(let message): message
        }
    }

    static func found(enabled: Bool, awaitingApproval: Bool) -> DextStatus {
        enabled ? .installed : awaitingApproval ? .needsApproval : .notInstalled
    }

    // macOS swaps a dext driving a device only when the device detaches, so an older build keeps running until then.
    static func activated(runningBuilds: [String], bundled: String?) -> DextStatus {
        runningBuilds.contains { $0 != bundled } ? .needsReplug : .installed
    }
}
