import Foundation

func check(_ ok: Bool, _ what: String, line: UInt = #line) {
    guard ok else { FileHandle.standardError.write(Data("FAIL line \(line): \(what)\n".utf8)); exit(1) }
}

@main enum Tests {
    static func main() {
        check(CameraProperty.settings.key == "4cc_hset_glob_0000", "extension key form")
        check(CameraProperty.settings.selector == 0x6873_6574, "selector is the big-endian FourCC")
        check(CameraProperty.quit.selector == 0x6871_6974 && CameraProperty.status.selector == 0x6873_7461, "other selectors")
        check(CameraProperty.videoOffset.selector == 0x6876_6f66, "video offset selector")
        check(Set(CameraProperty.allCases.map(\.selector)).count == CameraProperty.allCases.count, "selectors distinct")
        check(CameraProperty.allCases.allSatisfy { $0.rawValue.utf8.count == 4 }, "four characters each")
        print("camera property tests passed")
    }
}
