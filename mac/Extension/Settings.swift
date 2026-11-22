import Foundation

/// settings.json in the app group container, written by the app, read by whoever owns the device.
enum SettingsFile {
    static func url() -> URL? {
        let mach = Bundle.main.object(forInfoDictionaryKey: "CMIOExtension") as? [String: Any]
        guard let name = mach?["CMIOExtensionMachServiceName"] as? String, name.hasSuffix(".camera") else { return nil }
        let group = String(name.dropLast(".camera".count))
        return FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: group)?.appendingPathComponent("settings.json")
    }

    static func load() -> hd60s_settings? {
        guard let url = url(), let text = try? String(contentsOf: url, encoding: .utf8) else { return nil }
        var s = hd60s_settings()
        return hd60s_settings_parse(text, &s) == 0 ? s : nil
    }
}
