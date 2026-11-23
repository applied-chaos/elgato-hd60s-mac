import CoreMedia
import Foundation

func check(_ ok: Bool, _ what: String, line: UInt = #line) {
    guard ok else { FileHandle.standardError.write(Data("FAIL line \(line): \(what)\n".utf8)); exit(1) }
}

@main enum Tests {
    static func main() {
        let d = FrameRate.all.map(\.duration)
        check(d == [CMTime(value: 1001, timescale: 60000), CMTime(value: 1, timescale: 50), CMTime(value: 1, timescale: 30),
                    CMTime(value: 1, timescale: 25), CMTime(value: 1, timescale: 24)], "59.94, 50 first, then 30, 25, 24")
        check(FrameRate.all[0] == .ntsc, "index 0 stays 59.94")
        check(FrameRate(index: 0) == .ntsc && FrameRate(index: 1) == .pal, "index maps")
        check(FrameRate(index: 2) == .p30 && FrameRate(index: 3) == .p25 && FrameRate(index: 4) == .p24, "film and video rates")
        check(FrameRate(index: 5) == nil && FrameRate(index: -1) == nil, "bad index rejected")
        check(FrameRate(duration: CMTime(value: 1001, timescale: 60000)) == .ntsc, "exact 59.94")
        check(FrameRate(duration: CMTime(value: 1, timescale: 50)) == .pal, "exact 50")
        check(FrameRate(duration: CMTime(value: 1, timescale: 60)) == .ntsc, "60 snaps to 59.94")
        check(FrameRate(duration: CMTime(value: 1, timescale: 25)) == .p25, "exact 25")
        check(FrameRate(duration: CMTime(value: 1, timescale: 24)) == .p24, "exact 24")
        check(FrameRate(duration: CMTime(value: 1, timescale: 30)) == .p30, "exact 30")
        check(FrameRate(duration: CMTime(value: 1001, timescale: 30000)) == .p30, "29.97 snaps to 30")
        check(FrameRate(duration: CMTime(value: 1001, timescale: 24000)) == .p24, "23.976 snaps to 24")
        check(FrameRate(duration: CMTime(value: 1, timescale: 54)) == .pal, "nearest by fps")
        check(FrameRate(duration: CMTime(value: 1, timescale: 100)) == .ntsc, "far above snaps to 59.94")
        check(FrameRate(duration: .invalid) == nil && FrameRate(duration: .zero) == nil, "bad duration rejected")
        check(FrameRate.ntsc.index == 0 && FrameRate.pal.index == 1, "index round trip")
        print("frame rate tests passed")
    }
}
