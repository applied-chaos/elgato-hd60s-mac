import Foundation

func check(_ ok: Bool, _ what: String, line: UInt = #line) {
    guard ok else { FileHandle.standardError.write(Data("FAIL line \(line): \(what)\n".utf8)); exit(1) }
}

@main enum Tests {
    static func main() async {
        let replace = CameraStep.install(cameraActive: true)
        check(replace == [.requestQuit, .awaitExit, .activate, .verify], "replace asks the old one to quit, waits, activates, verifies")
        check(CameraStep.install(cameraActive: false) == [.activate, .verify], "fresh install activates and verifies")
        check(CameraStep.uninstall == [.deactivate], "uninstall only deactivates")
        check(!replace.contains(.deactivate), "install never deactivates")
        check(CameraStep.remaining(Array(replace.dropFirst()), quitRefused: false) == [.awaitExit, .activate, .verify], "quit accepted waits")
        check(CameraStep.remaining(Array(replace.dropFirst()), quitRefused: true) == [.activate, .verify], "older build without quit skips the wait")
        check(CameraStep.remaining([.activate, .verify], quitRefused: false) == [.activate, .verify], "exit timeout still activates")

        let exe = "/Library/SystemExtensions/ABC/net.mintopia.hd60s.camera.systemextension/Contents/MacOS/net.mintopia.hd60s.camera"
        check(CameraProcess.matches(path: exe), "extension binary matches")
        check(!CameraProcess.matches(path: "/Applications/HD60S.app/Contents/MacOS/HD60S"), "app does not match")
        check(!CameraProcess.matches(path: "/usr/bin/true"), "other does not match")
        check(CameraProcess.installedBundle(ofExecutable: exe) == "/Library/SystemExtensions/ABC/net.mintopia.hd60s.camera.systemextension",
              "bundle from executable")
        check(CameraProcess.installedBundle(ofExecutable: "/Applications/HD60S.app/Contents/Library/SystemExtensions/net.mintopia.hd60s.camera.systemextension/Contents/MacOS/x") == nil,
              "copy inside the app is not the installed one")

        var polls = 0
        let exited = await CameraProcess.pollUntil(timeout: 2) { polls += 1; return polls >= 3 }
        check(exited && polls == 3, "returns once the condition holds")
        let stuck = await CameraProcess.pollUntil(timeout: 0.3) { false }
        check(!stuck, "times out while it never holds")
        print("camera replace tests passed")
    }
}
