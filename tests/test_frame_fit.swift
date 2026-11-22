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
        check(fitRect(width: 640, height: 480) == Rect(x: 240, y: 0, w: 1440, h: 1080), "640x480 pillarboxed 4:3")
        check(fitRect(width: 640, height: 480, wide: true) == Rect(x: 0, y: 0, w: 1920, h: 1080), "640x480 flagged 16:9 fills")
        // PC modes are square pixels: the source's own aspect, the AVI flag is ignored
        for (w, h) in [(800, 600), (1024, 768), (1152, 864), (1280, 768), (1280, 800), (1280, 960), (1280, 1024),
                       (1360, 768), (1400, 1050), (1440, 900)] {
            let r = fitRect(width: w, height: h)
            check(r != nil, "\(w)x\(h) known")
            check(r == fitRect(width: w, height: h, wide: true), "\(w)x\(h) ignores the flag")
            check(r!.w <= 1920 && r!.h <= 1080 && r!.w % 2 == 0 && r!.x % 2 == 0, "\(w)x\(h) inside output")
            check(r!.w == 1920 || r!.h == 1080, "\(w)x\(h) fills one axis")
            check(abs(r!.w * h - r!.h * w) <= 2 * max(w, h), "\(w)x\(h) keeps aspect")
        }
        check(fitRect(width: 1280, height: 1024) == Rect(x: 284, y: 0, w: 1350, h: 1080), "5:4 pillarboxed")
        check(fitRect(width: 1440, height: 900) == Rect(x: 96, y: 0, w: 1728, h: 1080), "16:10 pillarboxed")
        var watch = UnsupportedWatch()
        check(!watch.note(width: 1366, height: 700, known: false), "one frame is not stable")
        for _ in 0..<UnsupportedWatch.stableFrames { _ = watch.note(width: 1366, height: 700, known: false) }
        check(watch.note(width: 1366, height: 700, known: false), "stable unknown size")
        check(!watch.note(width: 1920, height: 1080, known: true), "known frame clears")
        for i in 0..<(UnsupportedWatch.stableFrames * 2) {
            check(!watch.note(width: 1280, height: 12 + i, known: false), "varying runts never stable")
        }
        // runt frames seen during a mode switch
        for (w, h) in [(1280, 12), (1280, 703), (1920, 1045), (720, 1080), (0, 0)] {
            check(fitRect(width: w, height: h) == nil, "\(w)x\(h) rejected")
        }
        print("frame fit tests passed")
    }
}
