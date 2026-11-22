import CoreMedia
import Foundation

func check(_ ok: Bool, _ what: String, line: UInt = #line) {
    guard ok else { FileHandle.standardError.write(Data("FAIL line \(line): \(what)\n".utf8)); exit(1) }
}

@main enum Tests {
    static func main() {
        let d = FrameRate.all.map(\.duration)
        check(d == [CMTime(value: 1001, timescale: 60000), CMTime(value: 1, timescale: 50)], "59.94 first, 50 second")
        check(FrameRate.all[0] == .ntsc, "index 0 stays 59.94")
        check(FrameRate(index: 0) == .ntsc && FrameRate(index: 1) == .pal, "index maps")
        check(FrameRate(index: 2) == nil && FrameRate(index: -1) == nil, "bad index rejected")
        check(FrameRate(duration: CMTime(value: 1001, timescale: 60000)) == .ntsc, "exact 59.94")
        check(FrameRate(duration: CMTime(value: 1, timescale: 50)) == .pal, "exact 50")
        check(FrameRate(duration: CMTime(value: 1, timescale: 60)) == .ntsc, "60 snaps to 59.94")
        check(FrameRate(duration: CMTime(value: 1, timescale: 25)) == .pal, "25 snaps to 50")
        check(FrameRate(duration: CMTime(value: 1, timescale: 54)) == .pal, "nearest by fps")
        check(FrameRate(duration: .invalid) == nil && FrameRate(duration: .zero) == nil, "bad duration rejected")
        check(FrameRate.ntsc.index == 0 && FrameRate.pal.index == 1, "index round trip")
    }
}
