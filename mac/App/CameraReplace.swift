import Darwin
import Foundation

// registerassistantservice submits the new launchd job without waiting for the old one's removal to finish; if the
// old process is still alive the submit fails ("Operation already in progress"), is taken as success, and the
// extension never runs until reboot. It also respawns a quit extension within milliseconds, so quitting first only
// sometimes wins.
// sysextd won't let us deactivate a build other than our own. The app's CMIO link dies with the old process and
// never reattaches, hence the relaunch.
enum CameraStep: Equatable {
    case requestQuit, awaitExit, activate, verify, repair, relaunch, deactivate

    static func install(active: String?, bundled: String?) -> [CameraStep] {
        guard let active else { return [.activate, .verify] }
        return active == bundled ? [] : [.requestQuit, .awaitExit, .activate, .verify, .relaunch]
    }

    static let uninstall: [CameraStep] = [.deactivate]

    static func remaining(_ rest: [CameraStep], quitRefused: Bool) -> [CameraStep] {
        quitRefused ? rest.filter { $0 != .awaitExit } : rest
    }

    static func afterVerifyFailed(_ rest: [CameraStep], repaired: Bool) -> [CameraStep]? {
        repaired ? nil : [.repair, .verify] + rest
    }

    static func replaces(existing: String, with new: String, repairing: Bool) -> Bool {
        existing != new || repairing
    }
}

enum CameraProcess {
    private static let bundleName = "net.mintopia.hd60s.camera.systemextension"

    static func matches(path: String) -> Bool { path.contains(bundleName) }

    static func installedBundle(ofExecutable path: String) -> String? {
        guard path.hasPrefix("/Library/SystemExtensions/"), let r = path.range(of: bundleName) else { return nil }
        return String(path[..<r.upperBound])
    }

    static func running(_ bundle: String = bundleName) -> [(pid: pid_t, path: String)] {
        let count = proc_listallpids(nil, 0)
        guard count > 0 else { return [] }
        var pids = [pid_t](repeating: 0, count: Int(count) + 16)
        let filled = Int(proc_listallpids(&pids, Int32(pids.count * MemoryLayout<pid_t>.stride)))
        var buffer = [CChar](repeating: 0, count: 4096)
        return pids.prefix(max(filled, 0)).compactMap { pid in
            guard pid > 0, proc_pidpath(pid, &buffer, UInt32(buffer.count)) > 0 else { return nil }
            let path = String(cString: buffer)
            return path.contains(bundle) ? (pid, path) : nil
        }
    }

    static func runningPaths(of bundle: String = bundleName) -> [String] { running(bundle).map(\.path) }

    static func pids(ofBuild want: String?) -> [pid_t] {
        running().filter { installedBundle(ofExecutable: $0.path).flatMap(build(ofBundle:)) == want }.map(\.pid)
    }

    static func notRespawned(first: [pid_t], later: [pid_t]) -> Bool { first.contains { later.contains($0) } }

    static func isRunning() -> Bool { !runningPaths().isEmpty }

    static func build(ofBundle path: String) -> String? {
        NSDictionary(contentsOfFile: path + "/Contents/Info.plist")?["CFBundleVersion"] as? String
    }

    static func runningBuilds() -> [String] { runningPaths().compactMap(installedBundle(ofExecutable:)).compactMap(build(ofBundle:)) }

    static func pollUntil(timeout: TimeInterval, every interval: TimeInterval = 0.1, _ condition: @escaping () -> Bool) async -> Bool {
        await Task.detached {
            let deadline = Date().addingTimeInterval(timeout)
            while !condition() {
                if Date() >= deadline { return false }
                try? await Task.sleep(nanoseconds: UInt64(interval * 1e9))
            }
            return true
        }.value
    }
}
