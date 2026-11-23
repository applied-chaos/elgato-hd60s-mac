#include "audio.h"

#include <string.h>

int audio_gain_clamp(int g)
{
    return g < AUDIO_GAIN_MIN ? AUDIO_GAIN_MIN : g > AUDIO_GAIN_MAX ? AUDIO_GAIN_MAX : g;
}

struct audio_settings audio_from_pref(struct audio_pref p)
{
    return (struct audio_settings){p.source, audio_gain_clamp(AUDIO_DEFAULT.gain_half_db + p.gain_offset), p.mute != 0};
}

struct audio_pref audio_to_pref(struct audio_settings s)
{
    return (struct audio_pref){s.source, (int8_t)(audio_gain_clamp(s.gain_half_db) - AUDIO_DEFAULT.gain_half_db), s.mute != 0};
}

uint8_t audio_gain_code(int gain_half_db)
{
    return (uint8_t)(audio_gain_clamp(gain_half_db) & 0x7f);
}

static const struct {
    const char *name;
    enum audio_source source;
} source_names[] = {{"hdmi", AUDIO_HDMI}, {"line", AUDIO_LINE}, {"mix", AUDIO_MIX}};

const char *audio_source_name(enum audio_source s)
{
    for (size_t i = 0; i < sizeof source_names / sizeof *source_names; i++)
        if (source_names[i].source == s)
            return source_names[i].name;
    return source_names[0].name;
}

int audio_source_parse(const char *name, enum audio_source *out)
{
    for (size_t i = 0; name && i < sizeof source_names / sizeof *source_names; i++) {
        if (!strcmp(name, source_names[i].name)) {
            *out = source_names[i].source;
            return 1;
        }
    }
    return 0;
}

void audio_plan(const struct audio_settings *s, int dvi, const struct audio_regs *cur, struct audio_regs *out)
{
    int steps = audio_gain_clamp(s->gain_half_db);
    uint8_t code = audio_gain_code(steps);
    int live[2] = {s->source == AUDIO_LINE || s->source == AUDIO_MIX, s->source == AUDIO_HDMI || s->source == AUDIO_MIX};

    *out = *cur;
    out->r03 = dvi ? 0xa0 : 0xa1;
    out->r04 = (uint8_t)((cur->r04 & 0xbc) | (dvi ? 0x02 : 0x40));
    out->r07 = 0;
    for (int p = 0; p < 2; p++) {
        uint8_t gain = dvi && p == 0 && live[p] ? code : (uint8_t)(cur->pair[2 * p] & 0x7f);
        uint8_t mute = !live[p] || s->mute ? 0x80 : 0;
        out->pair[2 * p] = out->pair[2 * p + 1] = (uint8_t)(gain | mute);
    }
    if (dvi) {
        memset(out->gain, 0, sizeof out->gain);
    } else if (s->source != AUDIO_HDMI) {
        int positive = 123 + steps > 0x7a; /* Elgato's v = 2x + 123 */
        out->gain[0] = out->gain[1] = positive ? code : 0;
        out->gain[2] = out->gain[3] = positive ? 0 : code;
    }
}
