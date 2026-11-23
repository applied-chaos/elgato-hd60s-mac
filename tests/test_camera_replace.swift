import Foundation

func check(_ ok: Bool, _ what: String, line: UInt = #line) {
    guard ok else { FileHandle.standardError.write(Data("FAIL line \(line): \(what)\n".utf8)); exit(1) }
}

@main enum Tests {
    static func main() async {
        check(CameraStep.install(cameraActive: true) == [.deactivate, .awaitExit, .activate], "replace deactivates, waits, activates")
        check(CameraStep.install(cameraActive: false) == [.activate], "fresh install only activates")
        check(CameraStep.uninstall == [.deactivate], "uninstall deactivates")
        let path = "/Library/SystemExtensions/ABC/net.mintopia.hd60s.camera.systemextension/Contents/MacOS/net.mintopia.hd60s.camera"
        check(CameraProcess.matches(path: path), "extension binary matches")
        check(!CameraProcess.matches(path: "/Applications/HD60S.app/Contents/MacOS/HD60S"), "app does not match")
        check(!CameraProcess.matches(path: "/usr/bin/true"), "other does not match")
        var polls = 0
        let exited = await CameraProcess.waitForExit(timeout: 2) { polls += 1; return polls < 3 }
        check(exited && polls == 3, "returns once process gone")
        let stuck = await CameraProcess.waitForExit(timeout: 0.3) { true }
        check(!stuck, "times out while still running")
        print("camera replace tests passed")
    }
}
