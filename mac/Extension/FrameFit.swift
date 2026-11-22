import Foundation

let outputWidth = 1920
let outputHeight = 1080

struct Rect: Equatable {
    var x, y, w, h: Int
}

// Display aspect per source size. SD is assumed 4:3 (the HDMI AVI aspect flag is not read).
private let displayAspects: [[Int]: (Int, Int)] = [
    [1920, 1080]: (16, 9),
    [1280, 720]: (16, 9),
    [720, 576]: (4, 3),
    [720, 480]: (4, 3),
    [1440, 480]: (4, 3),
]

// Where a w x h source lands in the 1920x1080 output, aspect preserved. Nil for sizes that are not a
// known mode (runt frames while the source switches mode).
func fitRect(width: Int, height: Int) -> Rect? {
    guard let (n, d) = displayAspects[[width, height]] else { return nil }
    let w = min(outputWidth, outputHeight * n / d) & ~1
    let h = min(outputHeight, outputWidth * d / n) & ~1
    return Rect(x: (outputWidth - w) / 2 & ~1, y: (outputHeight - h) / 2, w: w, h: h)
}
