import Foundation

/// settings.json in the caller's own app group container.
enum SettingsFile {
    static func url() -> URL? { AppGroup.container?.appendingPathComponent("settings.json") }

    static func load() -> hd60s_settings? { url().flatMap(read) }

    static func read(_ url: URL) -> hd60s_settings? {
        (try? String(contentsOf: url, encoding: .utf8)).flatMap(parse)
    }

    static func write(_ s: hd60s_settings, to url: URL) throws {
        try Data(json(s).utf8).write(to: url, options: .atomic)
    }

    static func parse(_ text: String) -> hd60s_settings? {
        var s = hd60s_settings()
        return hd60s_settings_parse(text, &s) == 0 ? s : nil
    }

    static func json(_ s: hd60s_settings) -> String {
        var s = s
        var buf = [CChar](repeating: 0, count: 256)
        hd60s_settings_print(&s, &buf, buf.count)
        return String(cString: buf)
    }
}
