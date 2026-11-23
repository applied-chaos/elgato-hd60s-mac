import Foundation

func check(_ ok: Bool, _ what: String, line: UInt = #line) {
    guard ok else { FileHandle.standardError.write(Data("FAIL line \(line): \(what)\n".utf8)); exit(1) }
}

@main enum Tests {
    static func main() {
        let p = StatusPublisher()
        var seen: [String] = []
        p.onChange = { seen.append($0) }
        check(p.factsJSON == nil, "nothing before the first publish")
        p.frame(width: 1920, height: 1080, interlaced: false, topFirst: false, bt601: false, nowNs: 1_000_000_000)
        p.publish(frames: 1, discarded: 0, longLines: 0, nowNs: 1_500_000_000)
        check(seen.count == 1 && seen[0] == p.factsJSON, "publish pushes the facts to clients")
        check(seen[0].contains("1920"), "facts carry the signal")
        p.clear()
        check(seen.count == 2 && seen[1] == "" && p.factsJSON == nil, "stop pushes empty facts")
        print("status publisher tests passed")
    }
}
