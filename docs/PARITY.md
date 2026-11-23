# HD60 S parity audit: our driver vs Elgato (Game Capture HD 2.11.14 + libUB530)

Our state: `integration/tickets` at 650c25e (includes #7–#11, #13, #14 CLI). Elgato: libUB530 decompile (`docs/re/libUB530_ghidra.c`), `build/libUB530.dylib` vtables, and the 2.11.14 app . No hardware touched, no issues created.

Status key: **done**, **partial**, **missing**, **n/a** (app-only feature OBS covers, device can't do it, or permanently out of scope: flash/EDID/firmware writes).
Test key: **Pi** = Raspberry Pi 4 source is enough; **TV** = needs a TV/monitor on HDMI out; **console** = needs an HDCP or YCbCr-only source (PS5/Xbox/Switch).

## Where Elgato's per-device features live

`UB530_Public` vtable 0x2bce0 (read from the dylib):

| slot | impl | API | what it does on the HD60 S |
|---|---|---|---|
| +0x40 | 0x9184 | GetAudioSampleRate | returns `fmt+0x10e8+4`, which is forced to 48000 on the HD60 S |
| +0x48 | 0x8d36 | SetAudioVolumeProperty | `dev+0xb0` → `FUN_cad3` → `FUN_1ce18` (codec 0x94 gain) |
| +0x50 | 0x91b6 | GetHDCPEnabled | `dev+0x20 != 0` |
| +0x58/+0x70/+0x68/+0x60 | 0x8d4a/0x8d8c/0x8d60/0x8d76 | Brightness/Contrast/Saturation/Hue | `dev+8/9/0xa/0xb` → `FUN_cb40` → `FUN_103ea` |
| +0x78 | 0x91d0 | SetHDMIColorRange | `dev+0x7c` = 0x80000000 (Standard) or 0x80000111 (Expanded), re-runs `FUN_10b6b` |
| +0x80 | 0x9224 | GetHDMIColorRange | reads source AVI quantisation, MST b2:0x4A / 0x48. **The app never calls it** |
| +0x88/+0x90 | 0x8da2/0x8db0 | SetPathToAnimationsFolder / PlayAnimation | LED animation, see G4 |
| +0xd8 | 0x8ce2 | SetInput(video, audio) | `dev+0x10` = video input, `dev+0x4170` = (audio == analogue) |

App wrapper `BeddoRTBoard` (app:0x10034503e–0x1003453c0):
- Calls SetBrightness/Contrast/Saturation/Hue, SetAudioVolume (float slider mapped to an int, clamped 0..0x93), SetHDMIColorRange (`"extended"` → 1, anything else → 0) and SetInput.
- Polls DetectVideoFormat and GetHDCPEnabled, then drives PlayAnimation from signal and HDCP changes.

App settings UI (EGCBeddoSettings.strings):
- Picture tab: Brightness, Contrast, Saturation, Hue, Reset Defaults.
- HDMI Color Range: Standard / Expanded, with per-console defaults in EGCInitialDefaults.plist (`EGCXbox*_ColorRange = 1`, all others 0).
- Audio: "with Analog Audio", and Analog Audio Gain from -12 to +12 dB.
- USB Transfer Mode: Bulk / Isochronous.
- Also: Stretch Standard Definition Input, Convert SD to 640x480, Enable Interlaced Video, Crop, Firmware Version + Update.

App status messages: "No Signal", "Protected with HDCP", "USB 3 is required for this device".

## A. Input modes and EDID

| # | Item | Status | Notes |
|---|---|---|---|
| A1 | CEA modes 480i–1080p60 (all 20 Pi timings) captured and shown in the camera | done | #7 survey, #8, #9 (deinterlace), #11 (50 fps) |
| A2 | 640x480p60 (VIC 1, code 0x12) | done | #21. `parser_is_sd` and `fitRect` accept it; 640 wide is SD/601, 4:3 unless the AVI flag says 16:9 |
| A3 | PC/DMT timings (codes 0x13–0x3A, 800x600 … 1440x900) and 'B' (0x42, unknown timing) | done | #21. Ten PC sizes (800x600 … 1440x900, HD matrix, own square-pixel aspect) are fitted; any other stable size gives the "Unsupported mode" slate and `status` state `unsupported`. PC modes do not get VIC 32 on passthrough (not changed) |
| A4 | Exact 1001 flag (59.94 vs 60, 29.97 vs 30) | done | #22 reads MST3367 0x59:0x5a (`hd60s_rate_from_vsync`); `status` reports it. Was: Elgato: `FUN_12a2f` → `FUN_11f22` gives `fmt+0x3c`, and fps = rate×(1001−flag)/1001. Ours measures from the stream, ±0.1 fps, and can't tell them apart |
| A5 | 24/25/30p sources: nominal camera rate | done | #22 adds 30, 25 and 24 fps formats (camera check pending reboot). Was: The camera advertises only 59.94 and 50. Frames arrive at the source rate, but the format duration is wrong for 24/25/30p |
| A6 | EDID presets advertised to sources | n/a (write) | The EDID lives in SPI flash (`EDID_1080p_TDMS_200.bin` per GameCapture_CaptureDevices.xml). `WriteEDIDContent` (+0xa8) and `ReadEDIDContent` (+0xb0) both go through the `dev+0x41a8` MCU/flash helper, and the app calls neither. Permanently out of scope. What the device advertises can be read safely from the Pi side (`/sys/class/drm/card1-HDMI-A-1/edid`) |
| A7 | Document the advertised EDID | missing | Decode the EDID the Pi reads (`edid-decode`) into PROTOCOL.md, so supported modes, audio SADs and the YCbCr/RGB capability bits are on record |

## B. Colour range and space

| # | Item | Status | Notes |
|---|---|---|---|
| B1 | YCbCr input detection | missing | See detail below. **High impact**: PS4/PS5 "YUV422", Xbox, Switch and Apple TV commonly send YCbCr |
| B2 | HDMI colour range Standard/Expanded | done (#18) | `hd60s -r`, `settings.json`, dext user client. Pi measured: Full source Standard Y 2..255, Expanded 18..235; Limited source Standard 17..235 |
| B3 | Automatic colour range | done (#18), the default | libUB530 supports a nibble per source Q: `dev+0x7c = 0x80000100` compresses only when the AVI says full range (Q=2). The Pi sends Q=0 for Broadcast RGB Full (the Elgato EDID has no QS bit), so Auto behaves as Standard there; use Expanded |
| B4 | BT.601 matrix for SD | done | #10 (650c25e) re-runs `colour_convert` on SD/HD changes. Differences from Elgato: 1440x480i gets 601 here (Elgato 709, width test 640/720/768 only); 640 is covered (#21) |
| B5 | Source colorimetry for YCbCr input (AVI C1C0, MST b2:0x49 >> 6) | missing | Elgato reads it into `fmt+0x54` but never uses it for the matrix. With B1, the device passes YCbCr through untouched, so the camera tag must follow the source's colorimetry, falling back to 601 for SD and 709 for HD |
| B6 | `status` colour field | partial (bug) | `src/main.c:148` still hard-codes `COLOUR_BT709` with a stale comment. Since #10, SD is 601 |

**B1, YCbCr input detection.**
- What Elgato does: on every detection it reads the source AVI colour space and re-runs the CSC when it changes. At the end of `FUN_12a2f`, if the AVI InfoFrame is present (`FUN_12995` bit 3 = MST b2:0x0E bit3), it reads MST b2:0x48 & 0x60: 0x00 = RGB, 0x20 = 4:2:2, 0x40 = 4:4:4. That goes into `fmt+0x10e0`, and `FUN_10b6b` runs again on change.
- With YCbCr in, `FUN_10b6b` writes mode 0x62 (`b0:0x92`) and the pass-through table (`DAT_28730`, our `ub530_csc_rgb`) instead of the RGB→YCbCr matrix.
- Ours: `colour_convert(d, sd, 0)` always applies the RGB matrix. A YCbCr source gets a second, wrong conversion, which would show as strong colour casts.
- The `input_is_yuv` branch already exists in `src/device.c`. It needs the AVI read and a re-run on change.
- Test: Pi, if its vc4 kernel exposes a YCbCr output format (the lead says it does); otherwise a console set to YUV.
- Ticket: **Detect YCbCr HDMI sources and switch the receiver's colour matrix**

**B2, HDMI colour range Standard/Expanded.**
- What Elgato does: the user picks Standard or Expanded, with per-console defaults (Xbox = Expanded).
- How: `SetHDMIColorRange` → `dev+0x7c`. In `FUN_10b6b`, the nibble for the source's AVI Q (b2:0x4A bits 3:2: 2 → >>8, 1 → >>4, 0 → none) chooses the scaling: bit0 scales the nine matrix coefficients by 0xDC0/0x1000 (219/255, full → limited); bit1 by 0x129E/0x1000 (expand, never used by the app).
- Standard = no scaling: the table maps limited RGB to limited YCbCr, matching our measured SMPTE values from the Pi's default limited CEA output.
- Ours: always Standard.
- Test: Pi, using the vc4 `Broadcast RGB` connector property (Automatic / Full / Limited 16:235).
- Ticket: **Add HDMI colour range setting (Standard/Expanded) matching Game Capture HD**

## C. Picture adjustments (in hardware, MST3367 CSC)

| # | Item | Status | Notes |
|---|---|---|---|
| C1 | Brightness, contrast, saturation, hue | done (#23; hardware measurement in the ticket notes) | `FUN_103ea` ported in `apply_picture`; `--brightness/--contrast/--saturation/--hue`, settings.json keys, dext method 0 scalars 1-4. App sliders are #24 |
| C2 | SD aspect / stretch | done (better than Elgato) | #10 reads AVI DB2 picture aspect (b2:0x49 bits 5:4) and pillarboxes or fills automatically. Elgato only has an app checkbox, "Stretch Standard Definition Input". A manual override is optional; no ticket needed unless asked |

**C1, picture adjustments.**
- What Elgato does: four sliders, default 0.5 each (`beddo-Brig/Cont/Satu/Hue_`). `dev+8..0xb` defaults are 0x80, 0x80, 0x80, 0x81.
- How: each setter stores its byte and calls `FUN_cb40`. For HDMI (`dev+0x10` not 4/5/6) that runs `FUN_103ea(dev, bri, con, sat, hue, dev+0xc >> 5)`, which rewrites MST3367 b0:0x93–0xAA (the same coefficient block as `FUN_10b6b`). `FUN_10b6b` also calls `FUN_103ea` after loading the matrix, so the settings survive a matrix change.
- Ours (#23): `hd60s_csc_plan` runs the ported maths over the loaded matrix, so the controls survive range and input changes. Settings hold offsets from Elgato's bytes (brightness 0x81, the rest 0x80), so a zeroed struct is neutral. Brightness adds to the Y offset word (word 10), contrast scales words 1, 4, 7, saturation words 0, 2, 3, 5, 6, 8, and hue rotates the chroma pairs by +-62 degrees using Elgato's sine and cosine tables (extracted from the dylib, not committed).
- App UI is #24. CMIO has no standard picture controls, so they stay app-only settings.
- Test: Pi (SMPTE bars, measure Y/Cb/Cr shifts).
- Ticket: **Port hardware brightness/contrast/saturation/hue controls**

## D. Audio

| # | Item | Status | Notes |
|---|---|---|---|
| D1 | HDMI LPCM stereo 48 kHz capture | done | — |
| D2 | Source sample rate | done (#25: device resamples to 48 kHz; status reports the source rate) | See detail below |
| D3 | Analogue line-in (3.5 mm, Chat Link) | done (#20; line-in capture unverified, no cable on the test rig) | `hd60s capture -s line` or `-s mix`; see detail below |
| D4 | Analogue gain (−12…+12 dB) and hardware mute | done (#20) | `-g dB`, `-m`; steps measured, see detail below |
| D5 | HDMI vs DVI re-check of audio routing on every mode change | done (#20: re-run when the receiver's DVI flag changes; blank-start then enable now gives audio) | Elgato runs `FUN_1caf4` from `FUN_1d10a` on each new mode (step 3.5), so DVI→HDMI or no-signal→HDMI picks the right branch (MST b1:0x01 bit2). Ours runs `select_hdmi_audio` once in `hd60s_start`, so starting before the source is up, or a DVI-to-HDMI switch, can leave the wrong routing. Pi (start with the Pi blanked, then enable). Ticket: **Re-run HDMI audio routing when the source mode changes** |
| D6 | Multichannel / bitstream audio | n/a | The device is stereo LPCM only, and its EDID governs what sources send (see A7) |
| D7 | A/V offset and clock drift | partial | Tickets #5/#6 already exist. Elgato has an AV sync statistics view; no device-side latency control |

**D2, source sample rate.**
- What Elgato does: `FUN_12a2f` reads MST b2:0x9B & 0xF0 (0 → 44.1 kHz, 0xC0 → 32 kHz, else 48 kHz), then, for the HD60 S family (`FUN_17a0c`), **jumps to force 48000**. So Elgato always reports 48 kHz; we match.
- Measured (#25): the stream is 48 kHz for 32, 44.1 and 48 kHz sources, pitch correct, so nothing is passed on to consumers; `status` reports the source rate (N/CTS decode, matches b2:0x9B).
- Test: Pi, `speaker-test -r 44100` or a sine at 44.1 kHz, then measure pitch and frame rate. If it isn't resampled, add rate detection from b2:0x9B and report it to Core Audio.
- Ticket: **Verify 32/44.1 kHz HDMI audio capture and report the real rate**

**D3, analogue line-in.**
- What Elgato does: "with Analog Audio" checkbox, using HDMI for video and the 3.5 mm input for audio, one at a time.
- How: `SetInput(video, audio=1)` → `dev+0x4170 = 1`. The audio source byte at `fmt+0x1114` comes from `FUN_1ce00`; `FUN_1caf4`/`FUN_1ce18` then mute codec 0x94 regs 0x10/0x11 (HDMI pair) and unmute 0x0e/0x0f (line-in). With `dev+0x4170` set, `FUN_12a2f` skips HDMI rate detection.
- Ours: hard-coded source 0. Sources 1 (line-in) and 3 (mixed) were measured to work in the #12 notes. Mixed (3) goes beyond Elgato's UI.
- Test: Pi 4's own 3.5 mm analogue out into the HD60 S line-in.
- Ticket: **Select analogue line-in or HDMI+line-in mix as the audio source**

**D4, analogue gain and hardware mute.**
- How: `SetAudioVolumeProperty` → `dev+0xb0` → `FUN_cad3` → `FUN_1ce18(dev, v)`. The gain code is `clamp(v, 0x14, 0x93) + 5` (7 bits) into codec 0x94 regs 0x0e/0x0f (line-in), 0x10/0x11 (HDMI) or both, by source. Bit 7 of each is mute. Regs 0x0a–0x0d are set to 0 on this branch.
- App mapping (app:0x100345076): `v = 2·x + c`, clamped to 0 / 0x93. Elgato defaults are `beddo-Gain = -6` and `EGC-digital-audio-gain = 0`, the latter being software gain.
- Ours: keeps whatever gain is already in the registers (`& 0x7f`), so absolute level may differ from Elgato.
- Test: Pi (tone at a known level, measure dBFS per step).
- Ticket: **Expose hardware audio gain and mute (codec 0x94) with Elgato's default level**

## E. HDMI passthrough

| # | Item | Status | Notes |
|---|---|---|---|
| E1 | Video passthrough while capturing | partial | #13 merged. The state machine was hardware-checked with HPD forced and a fake EDID, but never on a real TV: the DDC EDID burst read and TV lock are unproven (PROTOCOL §6.10). **TV**. Ticket: **Run the #13 passthrough TV test plan on a real TV** |
| E2 | Passthrough audio | missing | Ticket #16 exists (PROTOCOL §6.7: N/CTS b1:0x33–0x35, regs 0xE0–0xE4, channel status b1:0x91–0x99, audio InfoFrame b1:0x68–0x6D/0xCE). **TV** |
| E3 | HDCP passthrough | missing | See detail below. **Console + TV** |
| E4 | Passthrough with no host app / after our stop | missing (unknown) | See detail below. **TV** (Pi source is fine) |
| E5 | Per-mode MST3367 writes and unmute colour bits | missing | Elgato's `FUN_1d10a` step 3 (`FUN_20b4c`, `FUN_20c02` b0:0x03, `FUN_20cdc` b0:0x0F/0x12/0x13/0x17, b0:0x60) plus state-5 unmute b0:0xB2 bits 0–1 from b0:0x55 or b0:0x14. Skipped in #13. Capture doesn't need them (#7). The TV might, in SD/720p. **TV**. Ticket: **Port Elgato's per-mode MST3367 writes if the TV test needs them** (conditional on E1) |
| E6 | Passthrough colour for RGB-only sinks and SD colorimetry | partial | `tx_avi` hard-codes DB2 colorimetry 0x88 (BT.709); Elgato sends 0x40 (601) for non-HD rows. The RGB-out path uses Elgato's BT.601 limited table (`DAT_33070`) for every mode. Now that capture switches to 601 for SD (#10), the AVI comment in `tx.c` is stale. **TV or DVI monitor**. Ticket: **Send BT.601 colorimetry in the passthrough AVI InfoFrame for SD** |
| E7 | Hotplug and mode-change wake-up | done (equivalent) | Elgato: replay program (0xC6, wIndex=1) watching 0x9A reg 0x0E bit7, plus an EP 0x81 interrupt. Ours polls every 250 ms. See H2 |

**E3, HDCP passthrough.**
- What Elgato does: when the source is encrypted (`dev+0x20`) and the TV is HDMI, the IT66121 authenticates with the TV using its internal keys, so HDCP content plays on the TV (capture stays blank).
- How: `FUN_20dc0` reads BCaps/BStatus/BKSV over DDC (slave 0x74), writes An and the auth regs 0x1F–0x46, and polls 0x46 bit7 / 0x07 bit0. `FUN_2109e` turns HDCP off.
- Safety: these are transmitter registers only, plus read-only DDC to the TV. The current allow-list (`tx.c:96`) excludes 0x1F–0x46 and would need widening.
- Ticket: **Authenticate HDCP on HDMI out for encrypted sources**

**E4, passthrough with no host app or after our stop.**
- What Elgato does: Elgato's help says a USB power adapter alone gives passthrough without recording. Our read-only probe found the IT66121 already set up at power-up (0x09–0x0B = B2 F8 37), so the MCU firmware likely runs passthrough standalone.
- On stop, both Elgato (`FUN_cf6c` → `FUN_1d10a(0x44)`, AV mute) and ours (`tx_blank`) mute the output and drop GPIO 52.
- Need to check: TV picture with the device plugged in and nothing open; after `hd60s capture` exits; after the camera closes. If ours leaves the TV black where firmware alone would show a picture, hand the transmitter back on stop (skip `tx_blank`, or restore the firmware state).
- Ticket: **Verify and preserve standalone passthrough when no app is capturing**

## F. HDCP detection and reporting

| # | Item | Status | Notes |
|---|---|---|---|
| F1 | Detect an HDCP-encrypted source and tell the user | done (console check pending) | #19. `status` state `hdcp`, "HDCP protected" camera slate and Status tab. The Pi can't send HDCP, so detection is unit-tested only; **Console** (PS5 with HDCP on) still to run |

**F1, HDCP detection.**
- What Elgato does: shows "Protected with HDCP" instead of a picture, and plays LED animation 5 or 6 (`PlayAnimation(6 − hdcp)`, app:0x1003451d7).
- How: `GetHDCPEnabled` → `dev+0x20`, set in `FUN_12a2f` from `FUN_125f7` bit1: MST b1:0x34 bit7 set, or b1:0x01 bit0 (when not overridden by the `FUN_17a0c`/`dev+0x28` checks). Bit 0 of `FUN_125f7` is b1:0x01 bit2, HDMI vs DVI.
- Ours (#19): `status` reports state `hdcp` whenever the flag is set, the camera shows the "HDCP protected" slate instead of the picture (as Elgato does), and the Status tab says "HDCP protected". Flag read once a second in `hd60s_policy_tick` (`hd60s_read_link`), also through the dext frame ring (v3). The app lists the source row only for DVI.

## G. Device-side status

| # | Item | Status | Notes |
|---|---|---|---|
| G1 | USB 3 check | partial | The CLI has it (#14: `not_superspeed` warning). Elgato shows "USB 3 is required for this device" in its UI; the app side of #14 is still open |
| G2 | Firmware version | done | Firmware updates (`UpdateMCUFirmware` +0xa0 → `FUN_177c8`) are n/a, permanently out of scope |
| G3 | Signal / no signal / mode | done | Slate, `status`, and an "Unsupported mode" slate and status (#21) |
| G4 | Status LED animations | missing (blocked on safety) | See detail below. Ticket: **Investigate the HD60 S LED controller at I2C 0xA4 (read-only)** |
| G5 | HDMI vs DVI source type in status | done | #19. `status` shows `source` hdmi or dvi from MST b1:0x01 bit2 (set = HDMI). Checked on the Pi with and without an HDMI VSDB in the EDID (debugfs `edid_override`) |
| G6 | ResetDevice (vtable +0x28 → `FUN_e56e` → `FUN_179b6`) | n/a for now | It goes through the MCU helper. The app doesn't call it in the paths traced. Not worth porting without a reason |

**G4, status LED animations.**
- What Elgato does: animations for signal found, lost or changed (`PlayAnimation(2/3/4)`), HDCP (5/6), and `13 ^ x` (probably recording on/off).
- How: `PlayAnimation(n)` → `FUN_176c6` → `FUN_18b34`, n 1..26. That stops the current animation with bridge write `[A4 07]` (`FUN_21dc4`), then writes `[A4 06 n]` (`FUN_21d92`) through the MCU helper `FUN_e1a` (proxy 0x5066), to I2C 0xA4 (7-bit 0x52).
- The `.ani` files in `Resources/Animations` (26 text frame tables) are parsed host-side (`FUN_188ea`) and appear never to be uploaded.
- **0x52 is in the 24Cxx EEPROM address range**, so `[06 n]` could be an EEPROM byte write. Do not implement until a read-only probe (does 0xA4 ACK; is reg 6 readable and does it change?) shows it's an LED MCU and not EEPROM.
- Also unconfirmed: whether the plain HD60 S even has a controllable LED.

## H. Transport and latency

| # | Item | Status | Notes |
|---|---|---|---|
| H1 | Isochronous transfer mode | done | Elgato offers "USB Transfer Mode: Bulk/Isochronous" (default bulk, `BeddoRTUseBulk`): iface 0 alt 1–3, EP 0x83, 4 buffers × 160 × bytes-per-interval. Useful for flaky hubs. Mac only, no source needed. Ticket: **Offer isochronous USB transfer as a fallback**. Done: `hd60s capture --iso`; bulk stays default. The dext is bulk only |
| H2 | Firmware change notification (replay program + EP 0x81 interrupt) | investigated, not adopted | Ours polls every 250 ms. #31 prototyped the watch program: -36% control transfers but unreliable wakes and a slower fallback, so polling stays (see PROTOCOL.md). Elgato uploads 10 I2C watch entries via 0xC6 wValue=100 wIndex=1 and arms EP 0x81, which gives faster mode and hotplug reaction with less bus traffic. Pi. Ticket: **Use the firmware's I2C watch program for signal-change wake-ups** |
| H3 | Latency modes | n/a | There is no device-side latency mode. "Instant Gameview" is the app's preview path. `dev+0xa8` (0x10 or 1 by frame rate) has an unknown purpose and no public setter |

## I. App-only features (n/a: OBS or the OS covers them)

- Recording, streaming, Flashback, profiles, crop, chroma key, Stream Command.
- "Convert SD to 640x480", "Allow 60 fps preview", "Enable Interlaced Video" (we already deinterlace on the host, #9, like Elgato's `deinterlaceOnHost`).
- Not in scope: TV/Component Compatibility sliders (older Fujitsu-chip HD only), firmware update, EDID write, FX3 RAM, 0xD4 (not fitted).

## Windows driver / 4K Capture Utility

- I found no primary-source detail on HD60 S-specific controls in the Windows driver or 4K Capture Utility, and won't guess.
- Elgato help confirms the user-facing set already covered above: HDMI colour range (Elgato Studio: Auto/Expand/Shrink), analogue line-in via Chat Link ("only one input, HDMI or analog, at a time"), the USB 3 requirement, and passthrough on USB power alone.
- Treat the mac app as the reference. If someone has a Windows box, capturing the DirectShow property page would settle it.

## Hardware test matrix

- **Pi only:** A2, A3, A4, A5, A7, B1 (if the vc4 YCbCr output format is available), B2, B3, B5, C1, D2, D3 (Pi 3.5 mm out to line-in), D4, D5, G5, H1, H2.
- **TV (Pi source fine):** E1, E2, E4, E5, E6.
- **Console:** B1 if the Pi can't send YCbCr, F1 (PS5 with HDCP on), E3 (console + TV).
- **Read-only probe only:** G4.

## Count

- **Missing (11):** A7, B1, B2, B3, B5, C1, E2, E3, E4, E5, G4.
  - E2 is existing ticket #16. E5 is conditional on E1.
- **Partial (6):** B6, D7, E1, E6, G1, H2.
  - D7 maps to existing #5/#6. G1 is the app side of #14. E1 is the TV run of #13.

Highest value first: B1 (YCbCr sources likely mis-coloured today), B2/B3 (colour range), F1 (HDCP message), D3/D4 (line-in and gain), then the TV-dependent E items.
