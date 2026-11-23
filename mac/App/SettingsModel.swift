import Foundation

struct PictureAudioSettings: Equatable {
    enum Range: CaseIterable {
        case auto, standard, expanded

        var label: String {
            switch self {
            case .auto: "Auto"
            case .standard: "Standard"
            case .expanded: "Expanded"
            }
        }

        fileprivate var c: hd60s_range {
            switch self {
            case .auto: RANGE_AUTO
            case .standard: RANGE_STANDARD
            case .expanded: RANGE_EXPANDED
            }
        }

        fileprivate init(_ c: hd60s_range) {
            self = Self.allCases.first { $0.c == c } ?? .auto
        }
    }

    enum Source: CaseIterable {
        case hdmi, line, mix

        var label: String {
            switch self {
            case .hdmi: "HDMI"
            case .line: "Line in"
            case .mix: "Both"
            }
        }

        fileprivate var c: audio_source {
            switch self {
            case .hdmi: AUDIO_HDMI
            case .line: AUDIO_LINE
            case .mix: AUDIO_MIX
            }
        }

        fileprivate init(_ c: audio_source) {
            self = Self.allCases.first { $0.c == c } ?? .hdmi
        }
    }

    static let percentRange = -100...100
    static let hueRange = -63...63
    static let gainRange = Int(AUDIO_GAIN_MIN)...Int(AUDIO_GAIN_MAX)
    private static let deviceMax = 127.0
    private static let hueStepsPerDegree = 2

    var range = Range.auto
    var brightness = 0, contrast = 0, saturation = 0
    var hue = 0
    var source = Source.hdmi
    var gainHalfDb = Int(audio_from_pref(audio_pref()).gain_half_db)
    var mute = false

    var gainEnabled: Bool { source != .hdmi }
    var brightnessText: String { Self.signed(brightness) }
    var contrastText: String { Self.signed(contrast) }
    var saturationText: String { Self.signed(saturation) }
    var hueText: String { Self.signed(hue) + "°" }
    var gainText: String {
        let db = Double(gainHalfDb) / 2
        return Self.signed(db == db.rounded() ? String(Int(abs(db))) : String(abs(db)), negative: db < 0, zero: db == 0) + " dB"
    }

    init() {}

    init?(json: String) {
        var c = hd60s_settings()
        guard hd60s_settings_parse(json, &c) == 0 else { return nil }
        self.init(c)
    }

    init(_ c: hd60s_settings) {
        range = Range(c.range)
        brightness = Self.percent(c.picture.brightness)
        contrast = Self.percent(c.picture.contrast)
        saturation = Self.percent(c.picture.saturation)
        hue = min(max(Int(c.picture.hue) / Self.hueStepsPerDegree, Self.hueRange.lowerBound), Self.hueRange.upperBound)
        let a = audio_from_pref(c.audio)
        source = Source(a.source)
        gainHalfDb = Int(a.gain_half_db)
        mute = a.mute != 0
    }

    var device: hd60s_settings {
        var c = hd60s_settings()
        c.range = range.c
        c.picture = hd60s_picture(brightness: Self.offset(brightness), contrast: Self.offset(contrast),
                                  saturation: Self.offset(saturation),
                                  hue: Int8(clamping: hue * Self.hueStepsPerDegree))
        c.audio = audio_to_pref(audio_settings(source: source.c, gain_half_db: Int32(gainHalfDb), mute: mute ? 1 : 0))
        return c
    }

    private static func percent(_ offset: Int8) -> Int {
        min(max(Int((Double(offset) * 100 / deviceMax).rounded()), percentRange.lowerBound), percentRange.upperBound)
    }

    private static func offset(_ percent: Int) -> Int8 {
        Int8(clamping: Int((Double(percent) * deviceMax / 100).rounded()))
    }

    private static func signed(_ n: Int) -> String {
        signed(String(abs(n)), negative: n < 0, zero: n == 0)
    }

    private static func signed(_ digits: String, negative: Bool, zero: Bool) -> String {
        zero ? digits : (negative ? "−" : "+") + digits
    }
}
