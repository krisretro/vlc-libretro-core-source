#include <stdint.h>
#include "vlc_core.h"

static void audio_play(void *data, const void *samples, unsigned count, int64_t pts)
{
    (void)data;
    (void)pts;                     // VLC is the master clock → we ignore PTS

    if (!samples || count == 0 || !audio_batch_cb)
        return;

    // === THIS IS THE ENTIRE AUDIO PATH NOW ===
    // No ring buffer. No accumulation. No silence. No mute_frames.
    // If RetroArch isn't consuming → samples are dropped (correct bridge behavior).
    audio_batch_cb((const int16_t *)samples, count);
}

void vlc_audio_setup_callbacks(libvlc_media_player_t *mp)
{
    libvlc_audio_set_callbacks(mp, audio_play, NULL, NULL, NULL, NULL, NULL);
    libvlc_audio_set_format(mp, "S16N", AUDIO_TARGET_RATE, 2);
}