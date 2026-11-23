import Darwin
import Foundation

// Replacing a running extension races in launchd: the new job is refused while the old one is still being
// removed ("Operation already in progress") and never runs until reboot. sysextd won't let us deactivate a
// build other than our own. The app's CMIO link dies with the old process and never reattaches, hence the relaunch.
enum CameraStep: Equatable {
    case requestQuit, awaitExit, activate, verify, relaunch, deactivate

    static func install(cameraActive: Bool) -> [CameraStep] {
        cameraActive ? [.requestQuit, .awaitExit, .activate, .verify, .relaunch] : [.activate, .verify]
    }

    static let uninstall: [CameraStep] = [.deactivate]

    static func remaining(_ rest: [CameraStep], quitRefused: Bool) -> [CameraStep] {
        quitRefused ? rest.filter { $0 != .awaitExit } : rest
    }
}

enum CameraProcess {
    private static let bundleName = "net.mintopia.hd60s.camera.systemextension"

    static func matches(path: String) -> Bool { path.contains(bundleName) }

    static func installedBundle(ofExecutable path: String) -> String? {
        guard path.hasPrefix("/Library/SystemExtensions/"), let r = path.range(of: bundleName) else { return nil }
        return String(path[..<r.upperBound])
    }

    static func runningPaths() -> [String] {
        let count = proc_listallpids(nil, 0)
        guard count > 0 else { return [] }
        var pids = [pid_t](repeating: 0, count: Int(count) + 16)
        let filled = Int(proc_listallpids(&pids, Int32(pids.count * MemoryLayout<pid_t>.stride)))
        var buffer = [CChar](repeating: 0, count: 4096)
        return pids.prefix(max(filled, 0)).compactMap { pid in
            guard pid > 0, proc_pidpath(pid, &buffer, UInt32(buffer.count)) > 0 else { return nil }
            let path = String(cString: buffer)
            return matches(path: path) ? path : nil
        }
    }

    static func isRunning() -> Bool { !runningPaths().isEmpty }

    static func build(ofBundle path: String) -> String? {
        NSDictionary(contentsOfFile: path + "/Contents/Info.plist")?["CFBundleVersion"] as? String
    }

    static func runningBuilds() -> [String] { runningPaths().compactMap(installedBundle(ofExecutable:)).compactMap(build(ofBundle:)) }

    static func pollUntil(timeout: TimeInterval, _ condition: @escaping () -> Bool) async -> Bool {
        await Task.detached {
            let deadline = Date().addingTimeInterval(timeout)
            while !condition() {
                if Date() >= deadline { return false }
                try? await Task.sleep(nanoseconds: 100_000_000)
            }
            return true
        }.value
    }
}
