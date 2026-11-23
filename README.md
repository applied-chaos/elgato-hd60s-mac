# elgato-hd60s-mac

A driver for the Elgato Game Capture HD60 S (USB `0fd9:0074`) on Apple silicon Macs, which Elgato never supported. It runs in userspace over libusb, so no kernel extension is needed.

It shows up as a normal camera and microphone called "Elgato HD60 S" in OBS, QuickTime, FaceTime, Zoom and browsers. When there's no HDMI signal, the camera shows a "No signal" slate.

Status: 1080p60 works, tested with a PS5. Other modes are untested. See [docs/PROTOCOL.md](docs/PROTOCOL.md) for how the device works and what's still unverified.

## Install

Needs macOS 26 on Apple silicon.

1. Download the latest `HD60S-x.y.z.dmg` from [Releases](https://github.com/applied-chaos/elgato-hd60s-mac/releases).
2. Drag `HD60S.app` to `/Applications`, open it and click Install.
3. Enter your administrator password when asked. This installs the audio plug-in and restarts Core Audio.
4. Approve the camera extension in System Settings > General > Login Items & Extensions > Camera Extensions.

Installing again updates the audio plug-in to the version in the app. To remove everything, click Uninstall in the app and enter your password once.

## Use

Pick "Elgato HD60 S" as the camera. In OBS, add it as a Video Capture Device, and add "Elgato HD60 S" as an Audio Input Capture.

- Consoles with HDCP on send no picture. The camera shows an "HDCP protected" slate and `hd60s status` and the app say so. On a PS5, turn off HDCP in Settings > System > HDMI.
- Audio only flows while something is using the camera, and runs about 30 ms behind the picture (userspace DMG).
- Only one thing can use the device at a time.
- The camera offers 59.94, 50, 30, 25 and 24 fps. Frames are delivered at the source's own rate whichever you pick, so a 50 Hz source never drops or repeats frames; pick the rate that matches your source for correct nominal timing and "No signal" slate pacing.

## Check the input

The app's Status tab shows whether the HDMI source is coming through (resolution, frame rate, scan, colour, audio), the USB link (a USB 2 port or hub is flagged) and how many frames were dropped. It also works while another app is using the camera.

`hd60s status` prints the same facts in a terminal. It opens the device for about two seconds, so it reports "in use" if the camera or another tool has it.

```sh
build/hd60s status
build/hd60s status --json    # one line, for scripts
```

```
Device:   present, USB 3.0 SuperSpeed (5 Gb/s), port 2-2
Firmware: MCU 11.03.11
Owner:    this tool
Signal:   1920x1080p 59.95 fps
Colour:   BT.709
Audio:    48000 Hz, 2 channels
Parser:   118 frames, 0 discarded, 0 runts, 0 long lines
Warnings: none
```

The JSON has `device` (`present`, `port`, `speed`, `access`, `firmware`, `owner`), `signal` (`state` of `unknown`, `none` or `active`, plus `width`, `height`, `interlaced`, `field_order`, `fps` and `colour` when active), `audio`, `parser`, `warnings` (problems such as `not_superspeed` or `busy`) and `notes` (information such as `untested_firmware`).

## Build from source

Needs Xcode 26 and Homebrew.

```sh
brew install libusb pkg-config xcodegen
make            # command-line tool, build/hd60s
make test
make install-app    # builds the app, camera extension and audio plug-in, installs to /Applications
make dist           # notarisable userspace DMG, build/HD60S-x.y.z.dmg
make dist-dext      # local-only DriverKit DMG, build/HD60S-DriverKit-x.y.z.dmg
```

The build downloads Elgato's Game Capture HD 2.11.14 for macOS and reads a few register tables from its `libUB530.dylib`. The tables aren't in this repo. The app builds need signing into Xcode with your own team; change `DEVELOPMENT_TEAM` in `mac/project.yml`.

The command-line tool captures raw video and audio:

```sh
build/hd60s info                                  # firmware, resolution and frame rate
build/hd60s status [--json] [-r range] [picture]   # device, USB link, owner, signal, audio (stream and source rate), warnings
build/hd60s capture -n 300 -o out.yuv -a out.pcm  # 300 frames of YUY2 and 48 kHz s16le
build/hd60s capture -s line -g 3 -a out.pcm        # audio from the 3.5 mm line-in (hdmi, line or mix) at +3 dB; -m mutes
build/hd60s capture --iso -n 300 -o out.yuv             # isochronous USB transfer, a fallback for flaky hubs (bulk is the default)
build/hd60s capture -o - | ffplay -f rawvideo -pixel_format yuyv422 -video_size 1920x1080 -framerate 60 -
```

`-r auto|standard|expanded` sets the HDMI colour range (default `auto`). `expanded` compresses full-range RGB to limited, for sources that send 0-255. `auto` does the same only when the source's AVI InfoFrame says full range (some sources, such as the Raspberry Pi with this EDID, send full range without saying so; use `expanded` for them). The camera extension reads `{"range":"auto|standard|expanded"}` from `settings.json` in the app group container about once a second; the DriverKit flavour takes it through its user client (external method 0, five scalars: range as 0 auto, 1 standard, 2 expanded, then brightness, contrast, saturation and hue as signed offsets).

Picture controls (`--brightness N`, `--contrast N`, `--saturation N`, `--hue N` on `status` and `capture`) are Elgato's own, done in the receiver's colour matrix. Each is an integer from -128 to 127 away from Elgato's default, so 0 changes nothing. In `settings.json` they are the same-named integers beside `range`; leave them out for 0.

## Releasing

Work happens on `develop`; `main` holds releases. Pushing a `vX.Y.Z` tag builds, signs and notarises the DMG and publishes a GitHub release. The release workflow needs these repository secrets. `scripts/setup-signing.sh` walks you through creating each one and sets them with `gh`:

| Secret | What |
|---|---|
| `MACOS_CERTIFICATE_P12` | Developer ID Application certificate and key, base64 `.p12` |
| `MACOS_CERTIFICATE_PASSWORD` | Its password |
| `MACOS_APP_PROFILE` | Developer ID provisioning profile for `net.mintopia.hd60s`, base64 |
| `MACOS_CAMERA_PROFILE` | Developer ID provisioning profile for `net.mintopia.hd60s.camera`, base64 |
| `ASC_KEY_P8` | App Store Connect API key, base64 `.p8`, for notarisation |
| `ASC_KEY_ID` | Its key ID |
| `ASC_ISSUER_ID` | Its issuer ID |

### DriverKit DMG

`make dist-dext` builds a DMG where a driver extension owns the device: it runs the capture core over USB, is the "Elgato HD60 S" audio input whenever the device is plugged in, and hands video to the camera extension through shared memory. The app bundles no HAL plug-in, feed device or jitter buffer, and installing it removes a HAL plug-in left by the userspace DMG (one administrator prompt) so only one "Elgato HD60 S" input shows. It is built locally with Xcode automatic signing, never by CI, and isn't published. It runs only on Macs in the team's development provisioning profile.

## Safety

The driver only sends the requests Elgato's own driver sends on every start. It never touches the firmware update, EDID write or FX3 RAM paths, which write to flash. HDMI passthrough (`src/tx.c`) writes only HDMI-out transmitter registers, behind an allow-list that `make test` exercises; it reads the TV's EDID over DDC and never writes to it.

## Not done yet

- Modes other than 1080p60. SD modes also need the BT.601 matrix.
- HDMI passthrough audio and HDCP. Video passthrough is implemented but not yet checked on a TV.
- Userspace DMG: rare tiny audio glitches as the HDMI and Mac clocks drift.

## Licence

MIT. Not affiliated with or endorsed by Elgato or Corsair; Elgato and HD60 S are their trademarks.

An [Applied Entropy](https://mintopia.net) project.
