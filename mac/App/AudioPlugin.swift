import Foundation

enum AudioStatus: Equatable {
    case notInstalled, installed, outdated

    var label: String {
        switch self {
        case .notInstalled: "not installed"
        case .installed: "installed"
        case .outdated: "update pending, click Install"
        }
    }
}

enum InstallAction: Equatable {
    case install, uninstall

    static func offered(camera: Bool, audio: AudioStatus, dext: DextStatus) -> InstallAction {
        let dextIn = dext == .installed || dext == .needsApproval
        return camera || audio != .notInstalled || dextIn ? .uninstall : .install
    }
}

enum AdminOutcome: Equatable { case done, cancelled, failed(String) }

enum AudioPlugin {
    static let name = "HD60SAudio.driver"
    static let halDirectory = "/Library/Audio/Plug-Ins/HAL"
    static let restartCoreAudio = "killall coreaudiod"

    static func status(installed: String?, bundled: String) -> AudioStatus {
        guard let installed else { return .notInstalled }
        var (a, b) = (components(installed), components(bundled))
        let n = max(a.count, b.count)
        a += Array(repeating: 0, count: n - a.count)
        b += Array(repeating: 0, count: n - b.count)
        return a.lexicographicallyPrecedes(b) ? .outdated : .installed
    }

    // osascript reports a dismissed admin prompt as error -128.
    static func outcome(exitCode: Int32, stderr: String) -> AdminOutcome {
        if exitCode == 0 { return .done }
        if stderr.contains("(-128)") { return .cancelled }
        let message = stderr.trimmingCharacters(in: .whitespacesAndNewlines)
        return .failed(message.isEmpty ? "The administrator step failed." : message)
    }

    // Copy beside the target, then swap, so a failure never leaves a half-written plug-in.
    static func installScript(source: String, directory: String = halDirectory,
                              restart: String = restartCoreAudio, owner: String? = "root:wheel") -> String {
        let target = quote(directory + "/" + name), staged = quote(directory + "/" + name + ".new")
        return ["set -e", "mkdir -p \(quote(directory))", "rm -rf \(staged)", "cp -R \(quote(source)) \(staged)",
                owner.map { "chown -R \($0) \(staged)" }, "rm -rf \(target)", "mv \(staged) \(target)", restart]
            .compactMap { $0 }.joined(separator: "\n")
    }

    static func uninstallScript(directory: String = halDirectory, restart: String = restartCoreAudio) -> String {
        "set -e\nrm -rf \(quote(directory + "/" + name))\n\(restart)"
    }

    static func appleScript(for shell: String) -> String {
        let escaped = shell.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"")
        return "do shell script \"\(escaped)\" with administrator privileges"
    }

    private static func quote(_ s: String) -> String { "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }

    private static func components(_ version: String) -> [Int] { version.split(separator: ".").map { Int($0) ?? 0 } }
}
