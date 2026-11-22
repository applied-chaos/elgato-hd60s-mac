import Foundation

func check(_ ok: Bool, _ what: String, line: UInt = #line) {
    guard ok else { FileHandle.standardError.write(Data("FAIL line \(line): \(what)\n".utf8)); exit(1) }
}

@main enum Tests {
    static func main() {
        check(fitRect(width: 1920, height: 1080) == Rect(x: 0, y: 0, w: 1920, h: 1080), "1080p fills")
        check(fitRect(width: 1280, height: 720) == Rect(x: 0, y: 0, w: 1920, h: 1080), "720p fills")
        for (w, h) in [(720, 576), (720, 480), (1440, 480)] {
            check(fitRect(width: w, height: h) == Rect(x: 240, y: 0, w: 1440, h: 1080), "\(w)x\(h) pillarboxed 4:3")
        }
        for (w, h) in [(720, 576), (720, 480), (1440, 480)] {
            check(fitRect(width: w, height: h, wide: true) == Rect(x: 0, y: 0, w: 1920, h: 1080), "\(w)x\(h) flagged 16:9 fills")
        }
        check(fitRect(width: 1920, height: 1080, wide: false) == Rect(x: 0, y: 0, w: 1920, h: 1080), "HD ignores the flag")
        // runt frames seen during a mode switch
        for (w, h) in [(1280, 12), (1280, 703), (1920, 1045), (720, 1080), (0, 0)] {
            check(fitRect(width: w, height: h) == nil, "\(w)x\(h) rejected")
        }
        print("frame fit tests passed")
    }
}
