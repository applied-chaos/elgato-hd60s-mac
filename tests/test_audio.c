#include "../src/audio.h"

#include <assert.h>
#include <string.h>

static struct audio_regs cur = {.r04 = 0xff, .gain = {1, 2, 3, 4}, .pair = {0x11, 0x11, 0x13, 0x13}};

static void gain_code_matches_elgato_mapping(void)
{
    /* Elgato app: v = 2x + 123, library code = (v + 5) & 0x7f, so code = steps & 0x7f. */
    assert(audio_gain_code(0) == 0x00);
    assert(audio_gain_code(AUDIO_DEFAULT.gain_half_db) == 0x74);
    assert(audio_gain_code(-24) == 0x68);
    assert(audio_gain_code(24) == 0x18);
    assert(audio_gain_code(-1) == 0x7f);
    assert(audio_gain_code(-100) == 0x68 && audio_gain_code(100) == 0x18);
}

static void gain_converts_between_db_and_half_db_steps(void)
{
    assert(audio_gain_from_db(-6) == -12 && audio_gain_from_db(0.5) == 1 && audio_gain_from_db(3.4) == 7);
    assert(audio_gain_from_db(0.25) == 1 && audio_gain_from_db(-0.25) == -1 && audio_gain_from_db(-12.2) == -24);
    assert(audio_gain_to_db(7) == 3.5 && audio_gain_to_db(AUDIO_GAIN_MIN) == -12.0 && audio_gain_to_db(0) == 0.0);
}

static void default_is_hdmi_at_minus_6_db(void)
{
    struct audio_settings d = AUDIO_DEFAULT;
    assert(d.source == AUDIO_HDMI && d.gain_half_db == -12 && !d.mute);
}

static void parse_source_names(void)
{
    enum audio_source s;
    assert(audio_source_parse("hdmi", &s) && s == AUDIO_HDMI);
    assert(audio_source_parse("line", &s) && s == AUDIO_LINE);
    assert(audio_source_parse("mix", &s) && s == AUDIO_MIX);
    assert(!audio_source_parse("both", &s) && !audio_source_parse(NULL, &s));
}

static void hdmi_audio_without_hdmi_link_routes_hdmi_pair_only(void)
{
    struct audio_settings s = AUDIO_DEFAULT;
    struct audio_regs r;
    audio_plan(&s, 0, &cur, &r);
    assert(r.r03 == 0xa1 && r.r04 == 0xfc && r.r07 == 0);
    assert(r.pair[0] == (0x80 | 0x11) && r.pair[1] == (0x80 | 0x11)); /* line-in muted, gain kept */
    assert(r.pair[2] == 0x13 && r.pair[3] == 0x13);                   /* HDMI live, gain untouched */
    assert(!memcmp(r.gain, cur.gain, 4));
}

static void hdmi_audio_over_hdmi_link_keeps_hdmi_gain(void)
{
    struct audio_settings s = AUDIO_DEFAULT;
    struct audio_regs r;
    audio_plan(&s, 1, &cur, &r);
    assert(r.r03 == 0xa0 && r.r04 == 0xbe);
    assert(r.pair[0] == 0x91 && r.pair[1] == 0x91);
    assert(r.pair[2] == 0x13 && r.pair[3] == 0x13); /* gain is analogue only: HDMI stays at unity */
    assert(!r.gain[0] && !r.gain[1] && !r.gain[2] && !r.gain[3]);
}

static void line_in_over_hdmi_link_sets_line_pair_gain(void)
{
    struct audio_settings s = {AUDIO_LINE, 6, 0};
    struct audio_regs r;
    audio_plan(&s, 1, &cur, &r);
    assert(r.pair[0] == 0x06 && r.pair[1] == 0x06);
    assert(r.pair[2] == (0x80 | 0x13) && r.pair[3] == (0x80 | 0x13));
    assert(!r.gain[0] && !r.gain[2]);
}

static void mix_sets_gain_on_line_pair_only(void)
{
    struct audio_settings s = {AUDIO_MIX, -12, 0};
    struct audio_regs r;
    audio_plan(&s, 1, &cur, &r);
    assert(r.pair[0] == 0x74 && r.pair[1] == 0x74 && r.pair[2] == 0x13 && r.pair[3] == 0x13);
}

static void line_in_without_hdmi_link_uses_gain_registers(void)
{
    struct audio_regs r;
    struct audio_settings s = {AUDIO_LINE, 6, 0}; /* 0x7a < v: positive gain goes to a/b */
    audio_plan(&s, 0, &cur, &r);
    assert(r.r03 == 0xa1 && r.r04 == 0xfc);
    assert(r.gain[0] == 0x06 && r.gain[1] == 0x06 && r.gain[2] == 0 && r.gain[3] == 0);
    assert(r.pair[0] == 0x11 && r.pair[1] == 0x11); /* line pair live, own gain kept */
    s.gain_half_db = -12; /* attenuation goes to c/d */
    audio_plan(&s, 0, &cur, &r);
    assert(r.gain[0] == 0 && r.gain[1] == 0 && r.gain[2] == 0x74 && r.gain[3] == 0x74);
    s.gain_half_db = 0; /* v = 123 > 0x7a: a/b, code 0 */
    audio_plan(&s, 0, &cur, &r);
    assert(r.gain[2] == 0 && r.gain[3] == 0);
    s.gain_half_db = -1; /* v = 122 = 0x7a: c/d */
    audio_plan(&s, 0, &cur, &r);
    assert(r.gain[2] == 0x7f && r.gain[0] == 0);
}

static void mute_sets_bit7_on_every_live_pair(void)
{
    struct audio_settings s = {AUDIO_MIX, -12, 1};
    struct audio_regs r;
    audio_plan(&s, 1, &cur, &r);
    assert(r.pair[0] == (0x80 | 0x74) && r.pair[1] == (0x80 | 0x74));
    assert(r.pair[2] == (0x80 | 0x13) && r.pair[3] == (0x80 | 0x13));
    s.source = AUDIO_HDMI;
    audio_plan(&s, 0, &cur, &r);
    assert(r.pair[2] == (0x80 | 0x13) && r.pair[0] & 0x80);
}

static void r04_keeps_unrelated_bits(void)
{
    struct audio_settings s = AUDIO_DEFAULT;
    struct audio_regs r, c = cur;
    c.r04 = 0xff;
    audio_plan(&s, 0, &c, &r);
    assert(r.r04 == (0xbc | 0x40));
    audio_plan(&s, 1, &c, &r);
    assert(r.r04 == (0xbc | 0x02));
}

static void pref_zero_is_elgatos_default_and_round_trips(void)
{
    struct audio_pref zero = {0};
    struct audio_settings d = audio_from_pref(zero);
    assert(d.source == AUDIO_HDMI && d.gain_half_db == AUDIO_DEFAULT.gain_half_db && !d.mute);
    struct audio_settings s = {AUDIO_MIX, 7, 1};
    struct audio_pref p = audio_to_pref(s);
    assert(p.gain_offset == 7 - AUDIO_DEFAULT.gain_half_db);
    struct audio_settings back = audio_from_pref(p);
    assert(back.source == AUDIO_MIX && back.gain_half_db == 7 && back.mute == 1);
    s = (struct audio_settings){AUDIO_LINE, 99, 5};
    back = audio_from_pref(audio_to_pref(s));
    assert(back.gain_half_db == AUDIO_GAIN_MAX && back.mute == 1);
    back = audio_from_pref((struct audio_pref){AUDIO_LINE, 127, 0});
    assert(back.gain_half_db == AUDIO_GAIN_MAX);
    back = audio_from_pref((struct audio_pref){AUDIO_LINE, -128, 0});
    assert(back.gain_half_db == AUDIO_GAIN_MIN);
}

int main(void)
{
    pref_zero_is_elgatos_default_and_round_trips();
    gain_code_matches_elgato_mapping();
    gain_converts_between_db_and_half_db_steps();
    default_is_hdmi_at_minus_6_db();
    parse_source_names();
    hdmi_audio_without_hdmi_link_routes_hdmi_pair_only();
    hdmi_audio_over_hdmi_link_keeps_hdmi_gain();
    line_in_over_hdmi_link_sets_line_pair_gain();
    mix_sets_gain_on_line_pair_only();
    line_in_without_hdmi_link_uses_gain_registers();
    mute_sets_bit7_on_every_live_pair();
    r04_keeps_unrelated_bits();
    return 0;
}
