import Combine
import Foundation

final class SettingsStore: ObservableObject {
    @Published var value: PictureAudioSettings {
        didSet { if value != oldValue { persist() } }
    }
    private let url: URL?
    private let pinned: Bool

        init(url: URL?, preview: PictureAudioSettings? = nil) {
        self.url = preview == nil ? url : nil
        pinned = preview != nil
        value = preview ?? url.flatMap(SettingsFile.read).map(PictureAudioSettings.init) ?? PictureAudioSettings()
        pushToDriver()
    }

    func reset() { value = PictureAudioSettings() }

    func pushToDriver() {
        #if DEXT
        guard !pinned else { return }
        var c = value.device
        _ = dext_send_settings(&c)
        #endif
    }

    private func persist() {
        guard !pinned else { return }
        if let url { try? SettingsFile.write(value.device, to: url) }
        pushToDriver()
    }
}
