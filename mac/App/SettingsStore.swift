import Combine
import Foundation

final class SettingsStore: ObservableObject {
    @Published var value: PictureAudioSettings {
        didSet { if value != oldValue { persist() } }
    }
    private let url: URL?
    private let pinned: Bool
    private let send: (hd60s_settings) -> Void

    init(url: URL?, preview: PictureAudioSettings? = nil, send: @escaping (hd60s_settings) -> Void = { _ in }) {
        self.url = preview == nil ? url : nil
        pinned = preview != nil
        self.send = send
        value = preview ?? url.flatMap(SettingsFile.read).map(PictureAudioSettings.init) ?? PictureAudioSettings()
        sendToCamera()
    }

    func reset() { value = PictureAudioSettings() }

    func sendToCamera() {
        guard !pinned else { return }
        send(value.device)
    }

    private func persist() {
        guard !pinned else { return }
        if let url { try? SettingsFile.write(value.device, to: url) }
        sendToCamera()
    }
}
