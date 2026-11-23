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

enum CameraState: Equatable {
    case absent, current, outdated

    init(active: String?, bundled: String?) {
        self = active == nil ? .absent : active == bundled ? .current : .outdated
    }
}

enum InstallAction: Equatable {
    case install, uninstall

    static func offered(flavour: Flavour, camera: CameraState, audio: AudioStatus, dext: DextStatus) -> InstallAction {
        let complete = switch flavour {
        case .userspace: audio == .installed
        case .driverKit: audio == .notInstalled && [.installed, .needsReplug, .needsApproval].contains(dext)
        }
        return camera == .current && complete ? .uninstall : .install
    }
}

enum AdminOutcome: Equatable { case done, cancelled, failed(String) }

enum InstallStep: Equatable { case none, admin(String), missingPlugIn }

enum Flavour: String {
    case userspace, driverKit = "driverkit"

    static var current: Flavour { Flavour(infoValue: Bundle.main.object(forInfoDictionaryKey: "HD60SFlavour")) }

    init(infoValue: Any?) { self = (infoValue as? String).flatMap(Flavour.init(rawValue:)) ?? .userspace }

    var showsAudioPlugIn: Bool { self == .userspace }

    func installStep(audio: AudioStatus, plugInSource: String?) -> InstallStep {
        switch self {
        case .userspace: plugInSource.map { .admin(AudioPlugin.installScript(source: $0)) } ?? .missingPlugIn
        case .driverKit: uninstallStep(audio: audio)
        }
    }

    func uninstallStep(audio: AudioStatus) -> InstallStep {
        self == .driverKit && audio == .notInstalled ? .none : .admin(AudioPlugin.uninstallScript())
    }
}

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
