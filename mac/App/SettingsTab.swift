import SwiftUI

private struct SettingRow<Content: View>: View {
    let label: String
    var dimmed = false
    @ViewBuilder let content: Content

    var body: some View {
        HStack(spacing: 8) {
            Text(label).font(.text(13)).foregroundStyle(Color.neutral).frame(width: 70, alignment: .leading)
            content
        }
        .frame(minHeight: 28)
        .opacity(dimmed ? 0.4 : 1)
    }
}

private struct StackedSetting<Content: View>: View {
    let label: String
    @ViewBuilder let content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(label).font(.text(13)).foregroundStyle(Color.neutral)
            content
        }
        .padding(.bottom, 6)
    }
}

private struct LightSegmented<T: Hashable>: View {
    let label: String
    let options: [T]
    let title: (T) -> String
    @Binding var selection: T

    var body: some View {
        HStack(spacing: 2) {
            ForEach(options, id: \.self) { option in
                let on = option == selection
                Button { selection = option } label: {
                    Text(title(option)).font(.text(12, .semibold))
                        .foregroundStyle(on ? Color.ink : Color.neutral)
                        .padding(.horizontal, 7).frame(height: 22)
                        .background(on ? Color.white : .clear, in: RoundedRectangle(cornerRadius: 6))
                        .shadow(color: on ? Color.ink.opacity(0.12) : .clear, radius: 1, y: 1)
                        .contentShape(RoundedRectangle(cornerRadius: 6))
                }
                .buttonStyle(.plain)
                .accessibilityAddTraits(on ? .isSelected : [])
            }
        }
        .padding(2)
        .background(Color(hex: 0xEEF0F3), in: RoundedRectangle(cornerRadius: 8))
        .fixedSize()
        .accessibilityElement(children: .contain)
        .accessibilityLabel(label)
    }
}

private struct SettingSlider: View {
    let label: String
    let text: String
    @Binding var value: Int
    let range: ClosedRange<Int>
    var enabled = true

    private static let thumb: CGFloat = 14
    private static let readoutWidth: CGFloat = 56

    var body: some View {
        SettingRow(label: label, dimmed: !enabled) {
            HStack(spacing: 8) {
                track
                readout
            }
        }
        .disabled(!enabled)
    }

    private var readout: some View {
        Text(text).font(.text(12)).monospacedDigit().foregroundStyle(Color.neutral).lineLimit(1)
            .frame(width: Self.readoutWidth, alignment: .trailing)
    }

    private var track: some View {
        GeometryReader { g in
            let width = g.size.width, span = Double(range.upperBound - range.lowerBound)
            let p = Double(value - range.lowerBound) / span
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 2).fill(Color.hairline).frame(height: 4)
                    .overlay(alignment: .leading) { Rectangle().fill(Color.cyan).frame(width: width * p) }
                    .clipShape(RoundedRectangle(cornerRadius: 2))
                ZStack {
                    Circle().fill(Color.border).frame(width: Self.thumb + 2, height: Self.thumb + 2)
                    Circle().fill(Color.white).frame(width: Self.thumb, height: Self.thumb)
                }
                .shadow(color: Color.ink.opacity(0.2), radius: 1, y: 1)
                .offset(x: p * (width - Self.thumb) - 1)
            }
            .frame(height: g.size.height)
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0).onChanged { drag in
                let t = min(max((drag.location.x - Self.thumb / 2) / (width - Self.thumb), 0), 1)
                value = range.lowerBound + Int((t * span).rounded())
            })
        }
        .frame(height: 28)
        .accessibilityElement()
        .accessibilityLabel(label)
        .accessibilityValue(text)
        .accessibilityAdjustableAction { direction in
            switch direction {
            case .increment: value = min(value + 1, range.upperBound)
            case .decrement: value = max(value - 1, range.lowerBound)
            @unknown default: break
            }
        }
    }
}

private struct SwitchControl: View {
    let label: String
    @Binding var isOn: Bool

    var body: some View {
        Button { isOn.toggle() } label: {
            Capsule().fill(isOn ? Color.cyan : Color.border).frame(width: 32, height: 18)
                .overlay(alignment: isOn ? .trailing : .leading) {
                    Circle().fill(Color.white).frame(width: 14, height: 14).padding(2)
                        .shadow(color: Color.ink.opacity(0.2), radius: 1, y: 1)
                }
                .animation(.easeOut(duration: 0.15), value: isOn)
        }
        .buttonStyle(.plain)
        .offset(y: -1.5)
        .accessibilityLabel(label)
        .accessibilityValue(isOn ? "On" : "Off")
        .accessibilityAddTraits(.isToggle)
    }
}

struct SettingsTab: View {
    @ObservedObject var store: SettingsStore

    private func heading(_ text: String) -> some View {
        Text(text).font(.text(13, .semibold)).foregroundStyle(Color.ink).padding(.bottom, 10)
    }

    var body: some View {
        let s = $store.value
        VStack(alignment: .leading, spacing: 0) {
            HStack(alignment: .top, spacing: 28) {
                VStack(alignment: .leading, spacing: 0) {
                    heading("Picture")
                    StackedSetting(label: "Colour range") {
                        LightSegmented(label: "Colour range", options: PictureAudioSettings.Range.allCases, title: \.label, selection: s.range)
                    }
                    SettingSlider(label: "Brightness", text: store.value.brightnessText, value: s.brightness, range: PictureAudioSettings.percentRange)
                    SettingSlider(label: "Contrast", text: store.value.contrastText, value: s.contrast, range: PictureAudioSettings.percentRange)
                    SettingSlider(label: "Saturation", text: store.value.saturationText, value: s.saturation, range: PictureAudioSettings.percentRange)
                    SettingSlider(label: "Hue", text: store.value.hueText, value: s.hue, range: PictureAudioSettings.hueRange)
                }
                .frame(maxWidth: .infinity, alignment: .topLeading)
                VStack(alignment: .leading, spacing: 0) {
                    heading("Audio")
                    StackedSetting(label: "Source") {
                        LightSegmented(label: "Source", options: PictureAudioSettings.Source.allCases, title: \.label, selection: s.source)
                    }
                    SettingSlider(label: "Gain", text: store.value.gainText, value: s.gainHalfDb, range: PictureAudioSettings.gainRange,
                                  enabled: store.value.gainEnabled)
                    SettingRow(label: "Mute") { SwitchControl(label: "Mute", isOn: s.mute) }
                    Text("Gain applies to the 3.5 mm audio in.").font(.text(12)).foregroundStyle(Color.neutral).padding(.top, 6)
                }
                .frame(maxWidth: .infinity, alignment: .topLeading)
            }
            Spacer(minLength: 0)
            Button("Reset to defaults", action: store.reset)
                .buttonStyle(.plain).font(.text(12, .semibold)).foregroundStyle(Color.link)
                .padding(.bottom, 16)
        }
        .frame(maxHeight: .infinity)
        .layoutPriority(1)
        .onAppear { store.push() }
    }
}
