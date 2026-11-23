import Foundation

func check(_ ok: Bool, _ what: String, line: UInt = #line) {
    guard ok else { FileHandle.standardError.write(Data("FAIL line \(line): \(what)\n".utf8)); exit(1) }
}

func bytes(_ s: hd60s_settings) -> [UInt8] { var s = s; return withUnsafeBytes(of: &s) { Array($0) } }

@main enum Tests {
    static func main() {
        let d = PictureAudioSettings()
        check(bytes(d.device) == bytes(hd60s_settings()), "defaults are the zero struct")
        check(d.range == .auto && d.source == .hdmi && d.gainHalfDb == -12 && !d.mute, "default fields")
        check(!d.gainEnabled, "gain dimmed on HDMI")
        check(d.brightnessText == "0" && d.contrastText == "0" && d.saturationText == "0" && d.hueText == "0°" && d.gainText == "−6 dB", "default text")

        var s = d
        s.source = .line
        check(s.gainEnabled, "gain on for line in")
        s.source = .mix
        check(s.gainEnabled, "gain on for both")

        s.brightness = 100; s.contrast = -100; s.saturation = 1; s.hue = 63
        let c = s.device
        check(c.picture.brightness == 127 && c.picture.contrast == -127 && c.picture.saturation == 1 && c.picture.hue == 126, "extremes \(c.picture)")
        check(s.brightnessText == "+100" && s.contrastText == "−100" && s.saturationText == "+1" && s.hueText == "+63°", "signed text")
        s.hue = -20
        check(s.hueText == "−20°", "negative hue text")

        for pct in -100...100 {
            var t = d
            t.brightness = pct; t.contrast = pct; t.saturation = pct
            let back = PictureAudioSettings(t.device)
            check(back.brightness == pct && back.contrast == pct && back.saturation == pct, "percent round trip \(pct)")
        }
        for deg in -63...63 {
            var t = d
            t.hue = deg
            check(PictureAudioSettings(t.device).hue == deg, "hue round trip \(deg)")
        }
        for half in -24...24 {
            var t = d
            t.gainHalfDb = half
            check(PictureAudioSettings(t.device).gainHalfDb == half, "gain round trip \(half)")
        }
        var g = d
        g.gainHalfDb = 0; check(g.gainText == "0 dB", "0 dB")
        g.gainHalfDb = -1; check(g.gainText == "−0.5 dB", "-0.5 dB")
        g.gainHalfDb = 7; check(g.gainText == "+3.5 dB", "+3.5 dB")
        g.gainHalfDb = 24; check(g.gainText == "+12 dB", "+12 dB")

        var raw = hd60s_settings()
        raw.picture = hd60s_picture(brightness: -128, contrast: 127, saturation: 0, hue: -128)
        let loaded = PictureAudioSettings(raw)
        check(loaded.brightness == -100 && loaded.contrast == 100 && loaded.hue == -63, "out of scale values clamp \(loaded)")

        for r in PictureAudioSettings.Range.allCases {
            for src in PictureAudioSettings.Source.allCases {
                var t = d
                t.range = r; t.source = src; t.mute = true; t.gainHalfDb = 5
                let back = PictureAudioSettings(t.device)
                check(back == t, "enum round trip \(r) \(src)")
            }
        }
        check(PictureAudioSettings.Range.allCases.map(\.label) == ["Auto", "Standard", "Expanded"], "range labels")
        check(PictureAudioSettings.Source.allCases.map(\.label) == ["HDMI", "Line in", "Both"], "source labels")

        var full = d
        full.range = .expanded; full.source = .mix; full.mute = true; full.gainHalfDb = 10
        full.brightness = 20; full.contrast = -30; full.saturation = 40; full.hue = -12
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("hd60s-settings-\(getpid())")
        try! FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let url = dir.appendingPathComponent("settings.json")
        check(SettingsFile.read(url) == nil, "missing file reads as nil")
        try! SettingsFile.write(full.device, to: url)
        check(PictureAudioSettings(SettingsFile.read(url)!) == full, "file round trip")
        try! SettingsFile.write(d.device, to: url)
        check(PictureAudioSettings(SettingsFile.read(url)!) == d, "overwrite with defaults")
        try! Data("{\"range\":\"auto\",\"gain\":99}".utf8).write(to: url)
        check(SettingsFile.read(url) == nil, "bad file rejected")
        try! Data("{\"range\":\"standard\"}".utf8).write(to: url)
        check(PictureAudioSettings(SettingsFile.read(url)!).range == .standard, "older file without audio keys")
        check(try! FileManager.default.contentsOfDirectory(atPath: dir.path) == ["settings.json"], "no temp files left behind")

        check(PictureAudioSettings(json: "{\"range\":\"expanded\",\"source\":\"line\"}")?.source == .line, "preview json")
        check(PictureAudioSettings(json: "nope") == nil, "preview json rejected")

        let store = dir.appendingPathComponent("store.json")
        let live = SettingsStore(url: store)
        check(live.value == d && !FileManager.default.fileExists(atPath: store.path), "fresh store holds defaults and writes nothing")
        live.value.brightness = 30
        live.value.mute = true
        check(PictureAudioSettings(SettingsFile.read(store)!) == live.value, "each change reaches the file")
        let reopened = SettingsStore(url: store)
        check(reopened.value == live.value, "settings persist across launches")
        reopened.reset()
        check(reopened.value == d && PictureAudioSettings(SettingsFile.read(store)!) == d, "reset writes defaults")
        let pinned = SettingsStore(url: store, preview: full)
        pinned.value.hue = 5
        check(pinned.value.hue == 5 && PictureAudioSettings(SettingsFile.read(store)!) == d, "preview store never writes")
        print("settings model tests passed")
    }
}
