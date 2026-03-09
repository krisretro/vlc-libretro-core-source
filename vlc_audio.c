#include <stdint.h>
#include "vlc_core.h"

static void audio_play(void *data, const void *samples, unsigned count, int64_t pts)
{
    (void)data; (void)pts;
    if (!samples || count == 0 || !audio_batch_cb) return;
if (core.transitioning) return;
    const int16_t *buf = (const int16_t *)samples;
    size_t remaining = count;
    while (remaining > 0) {
        size_t written = audio_batch_cb(buf, remaining);
        if (written == 0) break;   // RetroArch refusing all samples — give up
        buf       += written * 2;  // stereo, so *2 channels
        remaining -= written;
    }
}
void vlc_audio_setup_callbacks(libvlc_media_player_t *mp)
{
    libvlc_audio_set_callbacks(mp, audio_play, NULL, NULL, NULL, NULL, NULL);
    libvlc_audio_set_format(mp, "S16N", AUDIO_TARGET_RATE, 2);
}