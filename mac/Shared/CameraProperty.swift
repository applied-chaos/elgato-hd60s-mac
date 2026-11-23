import Foundation

// Custom CoreMediaIO device properties, the app's channel to the camera extension (docs/decisions/0001).
enum CameraProperty: String, CaseIterable {
    case settings = "hset", status = "hsta", quit = "hqit"

    var key: String { "4cc_\(rawValue)_glob_0000" }

    var selector: UInt32 { rawValue.utf8.reduce(0) { $0 << 8 | UInt32($1) } }
}
