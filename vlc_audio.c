#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "vlc_core.h"

static void audio_play(void *data, const void *samples, unsigned count, int64_t pts) {
    if (!samples || count == 0) return;

    pthread_mutex_lock(&core.mutex);
    core.last_audio_pts = pts;                    // ← ALWAYS update PTS

    if (core.audio_mute_frames > 0) {
        core.audio_mute_frames--;
        pthread_mutex_unlock(&core.mutex);
        return;                                   // just silence the ring, PTS still moves
    }

    // normal copy to ring...
    const int16_t *src = (const int16_t *)samples;
    size_t total = count * 2;
    for (size_t i = 0; i < total; i++) {
        core.audio_ring[core.audio_write_pos] = src[i];
        core.audio_write_pos = (core.audio_write_pos + 1) % AUDIO_BUFFER_SIZE;
        if (core.audio_write_pos == core.audio_read_pos)
            core.audio_read_pos = (core.audio_read_pos + 1) % AUDIO_BUFFER_SIZE;
    }
    pthread_mutex_unlock(&core.mutex);
}

void vlc_audio_flush(void) {
    pthread_mutex_lock(&core.mutex);
    core.audio_read_pos = 0;
    core.audio_write_pos = 0;
    memset(core.audio_ring, 0, sizeof(core.audio_ring));
    core.last_audio_pts = 0;
    pthread_mutex_unlock(&core.mutex);
}

void vlc_audio_setup_callbacks(libvlc_media_player_t *mp) {
    libvlc_audio_set_callbacks(mp, audio_play, NULL, NULL, NULL, NULL, NULL);
    libvlc_audio_set_format(mp, "S16N", AUDIO_TARGET_RATE, 2);
}