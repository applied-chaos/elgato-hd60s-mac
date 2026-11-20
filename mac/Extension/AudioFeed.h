#ifndef HD60S_AUDIO_FEED_H
#define HD60S_AUDIO_FEED_H

#include <stddef.h>
#include <stdint.h>

/* Plays captured audio into the HD60S audio plug-in's hidden feed device. No-op if the plug-in is not installed. */
void audio_feed_start(void);
void audio_feed_push(const uint8_t *pcm, size_t len);
void audio_feed_stop(void);

#endif
