import Darwin
import Foundation

// A running extension can't be replaced in place: launchd rejects the new job while the old one is still
// being removed ("Operation already in progress"), leaving no extension until reboot. So deactivate first,
// wait for the old process to exit, then activate.
enum CameraStep: Equatable {
    case deactivate, awaitExit, activate

    static func install(cameraActive: Bool) -> [CameraStep] {
        cameraActive ? [.deactivate, .awaitExit, .activate] : [.activate]
    }

    static let uninstall: [CameraStep] = [.deactivate]
}

enum CameraProcess {
    static func matches(path: String) -> Bool { path.contains("net.mintopia.hd60s.camera.systemextension") }

    static func isRunning() -> Bool {
        let count = proc_listallpids(nil, 0)
        guard count > 0 else { return false }
        var pids = [pid_t](repeating: 0, count: Int(count) + 16)
        let filled = Int(proc_listallpids(&pids, Int32(pids.count * MemoryLayout<pid_t>.stride)))
        var buffer = [CChar](repeating: 0, count: 4096)
        return pids.prefix(max(filled, 0)).contains { pid in
            pid > 0 && proc_pidpath(pid, &buffer, UInt32(buffer.count)) > 0 && matches(path: String(cString: buffer))
        }
    }

    // Polls on a background queue; false on timeout.
    static func waitForExit(timeout: TimeInterval = 10, isRunning: @escaping () -> Bool = isRunning) async -> Bool {
        await Task.detached {
            let deadline = Date().addingTimeInterval(timeout)
            while isRunning() {
                if Date() >= deadline { return false }
                try? await Task.sleep(nanoseconds: 100_000_000)
            }
            return true
        }.value
    }
}
