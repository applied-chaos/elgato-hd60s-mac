#ifndef HD60S_STATUS_H
#define HD60S_STATUS_H

#include <stdint.h>
#include <stdio.h>

#include "parser.h"

enum hd60s_speed { SPEED_UNKNOWN, SPEED_LOW, SPEED_FULL, SPEED_HIGH, SPEED_SUPER, SPEED_SUPER_PLUS };
enum hd60s_access { ACCESS_NOT_TRIED, ACCESS_OK, ACCESS_BUSY, ACCESS_FAILED };
enum hd60s_owner { OWNER_NONE, OWNER_CLI, OWNER_CAMERA, OWNER_DRIVER, OWNER_OTHER, OWNER_UNKNOWN };
enum hd60s_signal { SIGNAL_UNKNOWN, SIGNAL_NONE, SIGNAL_ACTIVE };
enum hd60s_field_order { FIELD_NONE, FIELD_TOP_FIRST };
enum hd60s_warning { WARN_NO_DEVICE, WARN_NOT_SUPERSPEED, WARN_BUSY, WARN_OPEN_FAILED, WARN_FIRMWARE };

/* MCU versions seen on working devices (b0<<16 | b1<<8 | b2). */
extern const uint32_t HD60S_KNOWN_MCU[];
extern const int HD60S_KNOWN_MCU_COUNT;

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
    int width, height, interlaced;
    enum hd60s_field_order field_order;
    double fps; /* frame rate; interlaced counts frames, not fields */
    enum hd60s_colour colour; /* matrix of the frames the host receives */
    enum hd60s_input_format input_format;
    int audio_rate, audio_channels; /* 0 = no audio */

    unsigned long frames, discarded, runts, long_lines;
};

enum hd60s_speed hd60s_speed_from_libusb(int libusb_speed);
enum hd60s_access hd60s_access_from_error(int libusb_error);
enum hd60s_owner hd60s_owner_from_path(const char *exe_path);
int hd60s_parse_owner_pid(const char *usb_exclusive_owner, int *pid);
int hd60s_size_standard(int width, int height);

uint32_t hd60s_warnings(const struct hd60s_status *s); /* bit per enum hd60s_warning */
void hd60s_warning_message(enum hd60s_warning w, const struct hd60s_status *s, char *buf, size_t n);
const char *hd60s_warning_code(enum hd60s_warning w);

void hd60s_print_text(const struct hd60s_status *s, FILE *out);
void hd60s_print_json(const struct hd60s_status *s, FILE *out);

/* Fills presence, speed, port and owner without opening the device. */
void hd60s_probe_usb(struct hd60s_status *s);

#endif
