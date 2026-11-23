import Foundation

/// settings.json in the app group container, written by the app, read by whoever owns the device.
enum SettingsFile {
    static func url() -> URL? { AppGroup.container?.appendingPathComponent("settings.json") }

    static func load() -> hd60s_settings? { url().flatMap(read) }

    static func read(_ url: URL) -> hd60s_settings? {
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return nil }
        var s = hd60s_settings()
        return hd60s_settings_parse(text, &s) == 0 ? s : nil
    }

    static func write(_ s: hd60s_settings, to url: URL) throws {
        var s = s
        var buf = [CChar](repeating: 0, count: 256)
        hd60s_settings_print(&s, &buf, buf.count)
        try Data(String(cString: buf).utf8).write(to: url, options: .atomic)
    }
}
