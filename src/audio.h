#ifndef HD60S_AUDIO_H
#define HD60S_AUDIO_H

#include <stdint.h>

/* Audio source and gain for the codec at I2C 0x94 (docs/PROTOCOL.md). Source values are Elgato's SetInput audio codes. */
enum audio_source { AUDIO_HDMI = 0, AUDIO_LINE = 1, AUDIO_MIX = 3 };

enum { AUDIO_GAIN_MIN = -24, AUDIO_GAIN_MAX = 24 }; /* half dB steps: -12 to +12 dB */

struct audio_settings {
    enum audio_source source;
    int gain_half_db;
    int mute;
};

/* Elgato's defaults: HDMI audio, beddo-Gain -6 dB. */
#define AUDIO_DEFAULT ((struct audio_settings){AUDIO_HDMI, -12, 0})

/* Stored form of audio_settings: gain is a half-dB offset from AUDIO_DEFAULT, so a zeroed struct is Elgato's default. */
struct audio_pref {
    enum audio_source source;
    int8_t gain_offset;
    int8_t mute;
};

struct audio_settings audio_from_pref(struct audio_pref p);
struct audio_pref audio_to_pref(struct audio_settings s);

/* Codec registers this feature owns: 0x03, 0x04, 0x07, 0x0a-0x0d (gain), 0x0e-0x11 (input pairs, bit 7 = mute). */
struct audio_regs {
    uint8_t r03, r04, r07, gain[4], pair[4];
};

int audio_gain_clamp(int gain_half_db);
/* Gain code Elgato writes (FUN_1ce18): 0.5 dB steps, 7-bit two's complement. Steps are clamped to +-12 dB. */
uint8_t audio_gain_code(int gain_half_db);
/* "hdmi", "line" or "mix". */
const char *audio_source_name(enum audio_source s);
int audio_source_parse(const char *name, enum audio_source *out);
/* FUN_1caf4 + FUN_1ce18. dvi is the receiver's DVI flag; cur is what the codec holds now (r04, gain and pair gain bits are kept where Elgato keeps them). */
void audio_plan(const struct audio_settings *s, int dvi, const struct audio_regs *cur, struct audio_regs *out);

#endif
