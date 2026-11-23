import Foundation

func check(_ ok: Bool, _ what: String, line: UInt = #line) {
    guard ok else { FileHandle.standardError.write(Data("FAIL line \(line): \(what)\n".utf8)); exit(1) }
}

@main enum Tests {
    static func main() {
        // status: installed vs bundled version
        check(AudioPlugin.status(installed: nil, bundled: "12") == .notInstalled, "missing")
        check(AudioPlugin.status(installed: "12", bundled: "12") == .installed, "same")
        check(AudioPlugin.status(installed: "9", bundled: "12") == .outdated, "numeric, not lexical")
        check(AudioPlugin.status(installed: "1.10.0", bundled: "1.9") == .installed, "newer installed")
        check(AudioPlugin.status(installed: "1.2", bundled: "1.2.1") == .outdated, "missing component is zero")
        // driver extension status from system extension properties
        check(DextStatus.found(enabled: true, awaitingApproval: false) == .installed, "enabled")
        check(DextStatus.found(enabled: false, awaitingApproval: true) == .needsApproval, "approval")
        check(DextStatus.found(enabled: true, awaitingApproval: true) == .installed, "enabled wins")
        check(DextStatus.found(enabled: false, awaitingApproval: false) == .notInstalled, "none")
        check(DextStatus.notBundled.label == nil && DextStatus.installed.label == "installed", "hidden unless bundled")
        check(DextStatus.activated(runningBuilds: ["185"], bundled: "187") == .needsReplug, "old build still driving the device")
        check(DextStatus.activated(runningBuilds: ["187"], bundled: "187") == .installed, "bundled build running")
        check(DextStatus.activated(runningBuilds: [], bundled: "187") == .installed, "no device attached: nothing to replace")
        check(DextStatus.needsReplug.label == "unplug and replug the HD60 S to finish", "replug label")
        check(Flavour(infoValue: "driverkit") == .driverKit && Flavour(infoValue: "userspace") == .userspace, "flavour parse")
        check(Flavour(infoValue: nil) == .userspace && Flavour(infoValue: "other") == .userspace, "flavour default")
        let plug = "/app/HD60SAudio.driver"
        let uninstall = AudioPlugin.uninstallScript()
        check(Flavour.userspace.installStep(audio: .notInstalled, plugInSource: plug) == .admin(AudioPlugin.installScript(source: plug)), "userspace installs")
        check(Flavour.userspace.installStep(audio: .outdated, plugInSource: plug) == .admin(AudioPlugin.installScript(source: plug)), "userspace updates")
        check(Flavour.userspace.installStep(audio: .notInstalled, plugInSource: nil) == .missingPlugIn, "userspace needs the bundled plug-in")
        check(Flavour.driverKit.installStep(audio: .notInstalled, plugInSource: nil) == .none, "driverkit: nothing to remove")
        check(Flavour.driverKit.installStep(audio: .installed, plugInSource: nil) == .admin(uninstall), "driverkit removes plug-in")
        check(Flavour.userspace.uninstallStep(audio: .notInstalled) == .admin(uninstall), "userspace uninstall always runs")
        check(Flavour.driverKit.uninstallStep(audio: .notInstalled) == .none && Flavour.driverKit.uninstallStep(audio: .installed) == .admin(uninstall), "driverkit uninstall prompts only for a plug-in")
        check(Flavour.driverKit.installStep(audio: .outdated, plugInSource: nil) == .admin(uninstall), "driverkit removes old plug-in")
        check(Flavour.userspace.showsAudioPlugIn && !Flavour.driverKit.showsAudioPlugIn, "plug-in line only in userspace")
        // admin prompt outcomes
        check(AudioPlugin.outcome(exitCode: 0, stderr: "") == .done, "ok")
        check(AudioPlugin.outcome(exitCode: 1, stderr: "execution error: User canceled. (-128)") == .cancelled, "cancel")
        check(AudioPlugin.outcome(exitCode: 1, stderr: "cp: No such file\n") == .failed("cp: No such file"), "failure")
        check(AudioPlugin.outcome(exitCode: 1, stderr: "") == .failed("The administrator step failed."), "silent failure")
        // scripts, run for real against a temp directory
        let tmp = NSTemporaryDirectory() + "audio test 'q' \(getpid())"
        let src = tmp + "/src/HD60SAudio.driver", hal = tmp + "/hal"
        try! FileManager.default.createDirectory(atPath: src, withIntermediateDirectories: true)
        try! "new".write(toFile: src + "/v", atomically: true, encoding: .utf8)
        try! FileManager.default.createDirectory(atPath: hal + "/HD60SAudio.driver", withIntermediateDirectories: true)
        try! "old".write(toFile: hal + "/HD60SAudio.driver/stale", atomically: true, encoding: .utf8)

        let ran = { (script: String) -> Int32 in
            let p = Process(); p.executableURL = URL(fileURLWithPath: "/bin/sh"); p.arguments = ["-c", script]
            try! p.run(); p.waitUntilExit(); return p.terminationStatus
        }
        let fm = FileManager.default
        check(ran(AudioPlugin.installScript(source: src, directory: hal, restart: "true", owner: nil)) == 0, "install runs")
        check(fm.contents(atPath: hal + "/HD60SAudio.driver/v") != nil, "new copy in place")
        check(!fm.fileExists(atPath: hal + "/HD60SAudio.driver/stale"), "old copy replaced")
        check(!fm.fileExists(atPath: hal + "/HD60SAudio.driver.new"), "no temp copy left")
        check(ran(AudioPlugin.installScript(source: src, directory: hal, restart: "false", owner: nil)) != 0, "restart failure fails script")
        check(ran(AudioPlugin.uninstallScript(directory: hal, restart: "true")) == 0, "uninstall runs")
        check(!fm.fileExists(atPath: hal + "/HD60SAudio.driver"), "removed")
        check(ran(AudioPlugin.uninstallScript(directory: hal, restart: "true")) == 0, "uninstall idempotent")
        try? fm.removeItem(atPath: tmp)

        check(AudioPlugin.appleScript(for: #"echo "a\b""#) == #"do shell script "echo \"a\\b\"" with administrator privileges"#, "applescript escaping")
        // Uninstall only when every component is in and current; anything missing or outdated offers Install.
        let user = { InstallAction.offered(flavour: .userspace, camera: $0, audio: $1, dext: .notBundled) }
        check(user(.absent, .notInstalled) == .install, "userspace: nothing installed")
        check(user(.current, .installed) == .uninstall, "userspace: all in")
        check(user(.current, .notInstalled) == .install, "userspace: plug-in removed, camera left")
        check(user(.current, .outdated) == .install, "userspace: plug-in outdated")
        check(user(.outdated, .installed) == .install, "userspace: camera outdated")
        check(user(.absent, .installed) == .install, "userspace: orphaned plug-in")
        let dk = { InstallAction.offered(flavour: .driverKit, camera: $0, audio: $1, dext: $2) }
        check(dk(.current, .notInstalled, .installed) == .uninstall, "driverkit: all in")
        check(dk(.current, .notInstalled, .needsReplug) == .uninstall, "driverkit: dext waiting for replug is in")
        check(dk(.current, .notInstalled, .needsApproval) == .uninstall, "driverkit: dext awaiting approval is in")
        check(dk(.current, .installed, .installed) == .install, "driverkit: stale plug-in")
        check(dk(.current, .notInstalled, .notInstalled) == .install, "driverkit: dext missing")
        check(dk(.current, .notInstalled, .failed("x")) == .install, "driverkit: failed dext")
        check(dk(.outdated, .notInstalled, .installed) == .install, "driverkit: camera outdated")
        check(dk(.absent, .notInstalled, .installed) == .install, "driverkit: camera missing")
        check(CameraState(active: nil, bundled: "189") == .absent, "no camera")
        check(CameraState(active: "188", bundled: "189") == .outdated, "older camera")
        check(CameraState(active: "189", bundled: "189") == .current, "same camera")
        print("audio plug-in tests passed")
    }
}
