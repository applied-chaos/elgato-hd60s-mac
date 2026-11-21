# elgato-hd60s-mac

A driver for the Elgato Game Capture HD60 S (USB `0fd9:0074`) on Apple silicon Macs, which Elgato never supported. It runs in userspace over libusb, so no kernel extension is needed.

It shows up as a normal camera and microphone called "Elgato HD60 S" in OBS, QuickTime, FaceTime, Zoom and browsers. When there's no HDMI signal, the camera shows a "No signal" slate.

Status: 1080p60 works, tested with a PS5. Other modes are untested. See [docs/PROTOCOL.md](docs/PROTOCOL.md) for how the device works and what's still unverified.

## Install

Needs macOS 26 on Apple silicon.

1. Download the latest `HD60S-x.y.z.dmg` from [Releases](https://github.com/applied-chaos/elgato-hd60s-mac/releases).
2. Drag `HD60S.app` to `/Applications`, open it and click Install.
3. Approve it in System Settings > General > Login Items & Extensions > Camera Extensions.
4. For audio, copy `HD60SAudio.driver` to `/Library/Audio/Plug-Ins/HAL`, then run `sudo killall coreaudiod`.

To remove it, click Uninstall in the app and delete the driver from `/Library/Audio/Plug-Ins/HAL`.

## Use

Pick "Elgato HD60 S" as the camera. In OBS, add it as a Video Capture Device, and add "Elgato HD60 S" as an Audio Input Capture.

- Consoles with HDCP on send no picture. On a PS5, turn off HDCP in Settings > System > HDMI.
- Audio only flows while something is using the camera, and runs about 30 ms behind the picture.
- Only one thing can use the device at a time.

## Build from source

Needs Xcode 26 and Homebrew.

```sh
brew install libusb pkg-config xcodegen
make            # command-line tool, build/hd60s
make test
make install-app    # builds the app and camera extension, installs to /Applications
make install-audio  # builds the audio driver, installs it with sudo
```

The build downloads Elgato's Game Capture HD 2.11.14 for macOS and reads a few register tables from its `libUB530.dylib`. The tables aren't in this repo. The app builds need signing into Xcode with your own team; change `DEVELOPMENT_TEAM` in `mac/project.yml`.

The command-line tool captures raw video and audio:

```sh
build/hd60s info                                  # firmware, resolution and frame rate
build/hd60s capture -n 300 -o out.yuv -a out.pcm  # 300 frames of YUY2 and 48 kHz s16le
build/hd60s capture -o - | ffplay -f rawvideo -pixel_format yuyv422 -video_size 1920x1080 -framerate 60 -
```

## Releasing

Work happens on `develop`; `main` holds releases. Pushing a `vX.Y.Z` tag builds, signs and notarises the DMG and publishes a GitHub release. The release workflow needs these repository secrets:

| Secret | What |
|---|---|
| `MACOS_CERTIFICATE_P12` | Developer ID Application certificate and key, base64 `.p12` |
| `MACOS_CERTIFICATE_PASSWORD` | Its password |
| `MACOS_APP_PROFILE` | Developer ID provisioning profile for `net.mintopia.hd60s`, base64 |
| `MACOS_CAMERA_PROFILE` | Developer ID provisioning profile for `net.mintopia.hd60s.camera`, base64 |
| `ASC_KEY_P8` | App Store Connect API key, base64 `.p8`, for notarisation |
| `ASC_KEY_ID` | Its key ID |
| `ASC_ISSUER_ID` | Its issuer ID |

## Safety

The driver only sends the requests Elgato's own driver sends on every start. It never touches the firmware update, EDID write or FX3 RAM paths, which write to flash.

## Not done yet

- Modes other than 1080p60. SD modes also need the BT.601 matrix.
- HDMI passthrough.
- Rare tiny audio glitches as the HDMI and Mac clocks drift.

## Licence

MIT. Not affiliated with or endorsed by Elgato or Corsair; Elgato and HD60 S are their trademarks.

An [Applied Entropy](https://mintopia.net) project.
