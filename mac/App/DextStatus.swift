enum DextStatus: Equatable {
    case notBundled, working, needsApproval, installed, notInstalled, failed(String)

    // nil hides the line: release builds do not bundle the driver extension.
    var label: String? {
        switch self {
        case .notBundled: nil
        case .working: "working"
        case .needsApproval: "approval needed"
        case .installed: "installed"
        case .notInstalled: "not installed"
        case .failed(let message): message
        }
    }

    static func found(enabled: Bool, awaitingApproval: Bool) -> DextStatus {
        enabled ? .installed : awaitingApproval ? .needsApproval : .notInstalled
    }
}
