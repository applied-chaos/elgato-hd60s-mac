# hd60s

Userspace driver for the Elgato Game Capture HD60 S (USB `0fd9:0074`) on Apple silicon Macs. Elgato never shipped an Apple silicon driver; this one talks to the device directly over libusb, so no kernel or system extension is needed.

Status: 1080p60 HDMI capture works (tested with a PS5). Output is raw YUY2 video and 48 kHz PCM audio. See `docs/PROTOCOL.md` for the protocol and what is still unverified.

## Build

Needs Homebrew `libusb` and `pkg-config`, plus your own copy of Elgato's `libUB530.dylib` from Game Capture HD 2.11.14 for macOS. A few register tables are read from it at build time and never committed.

```sh
brew install libusb pkg-config
make LIBUB530="/path/to/Game Capture HD.app/Contents/Frameworks/libUB530.dylib"
make test
```

`tools/extract-tables.py` checks the dylib's sha256 and writes `build/ub530_tables.h`.

## Use

```sh
build/hd60s info                                  # MCU firmware, measured resolution and frame rate
build/hd60s capture -n 300 -o out.yuv -a out.pcm  # 300 frames, then stop
build/hd60s capture -o - | ffplay -f rawvideo -pixel_format yuyv422 -video_size 1920x1080 -framerate 60 -
ffmpeg -f s16le -ar 48000 -ac 2 -i out.pcm out.wav
```

Ctrl-C stops cleanly and powers the board down. Close any other app holding the device first.

Consoles with HDCP on (the PS5 default) send no capturable picture: turn off HDCP in the console's HDMI settings.

## Safety

The driver only sends the requests Elgato's own driver sends on every start. It never calls Elgato's firmware update, EDID write or FX3 RAM paths, all of which write flash. Don't add them.

## Not done yet

- Modes other than 1080p60 are untested. SD modes need the BT.601 matrix.
- HDMI passthrough (the IT66121 transmitter) is not initialised.
- Audio routing is unverified because the test source was silent.
- No macOS camera extension yet. Pipe into ffmpeg or OBS's media source for now.
