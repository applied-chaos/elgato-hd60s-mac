import Foundation

enum StatusSnapshot {
    static let fileName = "status.json"
    static let maxAge: TimeInterval = 3

    static func url() -> URL? {
        guard let group = Bundle.main.object(forInfoDictionaryKey: "HD60SGroup") as? String, !group.isEmpty else { return nil }
        return FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: group)?.appendingPathComponent(fileName)
    }
}

func factsData(_ st: hd60s_status) -> Data {
    var st = st
    var buffer: UnsafeMutablePointer<CChar>?
    var size = 0
    guard let file = open_memstream(&buffer, &size) else { return Data() }
    hd60s_print_json(&st, file)
    fclose(file)
    defer { free(buffer) }
    return buffer.map { Data(bytes: $0, count: size) } ?? Data()
}
