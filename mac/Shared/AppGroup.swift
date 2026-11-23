import Foundation

enum AppGroup {
    static var container: URL? {
        guard let group = Bundle.main.object(forInfoDictionaryKey: "HD60SGroup") as? String, !group.isEmpty else { return nil }
        return FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: group)
    }
}
