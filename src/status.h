#ifndef HD60S_STATUS_H
#define HD60S_STATUS_H

#include <stdint.h>
#include <signal.h>
#include <stdio.h>

#include "device.h"
#include "parser.h"

/* Every enum ends in a _COUNT sentinel; status.c checks its name tables against it at compile time. */
enum hd60s_speed { SPEED_UNKNOWN, SPEED_LOW, SPEED_FULL, SPEED_HIGH, SPEED_SUPER, SPEED_SUPER_PLUS, SPEED_COUNT };
enum hd60s_access { ACCESS_NOT_TRIED, ACCESS_OK, ACCESS_BUSY, ACCESS_FAILED, ACCESS_COUNT };
enum hd60s_owner { OWNER_NONE, OWNER_CLI, OWNER_CAMERA, OWNER_DRIVER, OWNER_OTHER, OWNER_UNKNOWN, OWNER_COUNT };
enum hd60s_signal { SIGNAL_UNKNOWN, SIGNAL_NONE, SIGNAL_ACTIVE, SIGNAL_UNSUPPORTED, SIGNAL_HDCP, SIGNAL_COUNT };
enum hd60s_link_type { SOURCE_UNKNOWN, SOURCE_HDMI, SOURCE_DVI, SOURCE_COUNT };
enum hd60s_field_order { FIELD_NONE, FIELD_TOP_FIRST, FIELD_COUNT };
enum hd60s_warning { WARN_NO_DEVICE, WARN_NOT_SUPERSPEED, WARN_BUSY, WARN_OPEN_FAILED, WARN_COUNT };

/* Firmware seen on the dev unit; any other MCU version is reported as untested. */
#define HD60S_DEV_MCU 0x110311

/* The status report shared by the CLI and the app (the app reads the JSON form). */
struct hd60s_status {
    int present;
    char port_path[32]; /* "bus-port.port...", empty if libusb gave none */
    enum hd60s_speed speed;
    enum hd60s_access access;
    enum hd60s_owner owner;
    int owner_pid;
    char owner_name[64];
    int fw_valid;
    uint32_t mcu_version;

    enum hd60s_signal signal;
    enum hd60s_link_type source; /* known while SIGNAL_ACTIVE or SIGNAL_HDCP */
    int width, height, interlaced;
    enum hd60s_field_order field_order;
    double fps; /* frame rate; interlaced counts frames, not fields */
    enum hd60s_colour colour;
    enum hd60s_input_format input_format;
    enum hd60s_range range; /* the setting in force, not what the source sent */
    int audio_rate, audio_channels; /* stream format, 0 = no audio */
    int audio_source_rate;          /* rate the source sends before the device resamples to 48 kHz, 0 = unknown */

    unsigned long frames, discarded, runts, long_lines;
};

enum hd60s_speed hd60s_speed_from_libusb(int libusb_speed);
enum hd60s_access hd60s_access_from_error(int libusb_error);
enum hd60s_owner hd60s_owner_from_path(const char *exe_path);
int hd60s_parse_owner_pid(const char *usb_exclusive_owner, int *pid);
int hd60s_size_standard(int width, int height);

/* Source type from the link flags; unknown if there is no signal to describe. */
enum hd60s_link_type hd60s_link_type_from_link(struct hd60s_link l, enum hd60s_signal signal);

/* Firmware read but not in the known list: informational, not a warning. */
int hd60s_firmware_untested(const struct hd60s_status *s);
uint32_t hd60s_warnings(const struct hd60s_status *s); /* bit per enum hd60s_warning */
void hd60s_warning_message(enum hd60s_warning w, const struct hd60s_status *s, char *buf, size_t n);
const char *hd60s_warning_code(enum hd60s_warning w);

void hd60s_print_text(const struct hd60s_status *s, FILE *out);
void hd60s_print_json(const struct hd60s_status *s, FILE *out);

/* Field or frame rate the MST3367 measured: tenths of Hz, and whether the 1000/1001 divisor applies. */
struct hd60s_rate {
    int tenths, ntsc;
};
/* FUN_11f22: counter is MST3367 bank 0 regs 0x59:0x5a & 0x3fff, 8 us ticks per vsync period. tenths is 0 for no reading. */
struct hd60s_rate hd60s_rate_from_vsync(unsigned counter);
double hd60s_rate_hz(struct hd60s_rate r);
/* Frame rate for the status report: the exact rate if it agrees with the measured fps, else measured. */
double hd60s_exact_fps(double measured, struct hd60s_rate r, int interlaced);

/* Fills presence, speed, port and owner without opening the device. */
void hd60s_probe_usb(struct hd60s_status *s);

/* Probes, then opens the device and watches the stream for `seconds` (device must be free). */
void hd60s_sample(struct hd60s_status *out, double seconds, struct hd60s_settings settings, volatile sig_atomic_t *stop);

#endif
