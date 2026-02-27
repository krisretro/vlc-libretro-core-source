#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include "vlc_core.h"

static void *lock_cb(void *data, void **planes) {
    pthread_mutex_lock(&core.mutex);

    if (!core.video_buffer || core.video_width == 0 || core.video_height == 0) {
        static uint32_t fallback_buffer[640 * 360] = {0};   // black fallback
        *planes = fallback_buffer;
        pthread_mutex_unlock(&core.mutex);
        return NULL;
    }

    *planes = core.video_buffer;
    pthread_mutex_unlock(&core.mutex);
    return NULL;
}

static void unlock_cb(void *data, void *id, void *const *planes) {
    pthread_mutex_unlock(&core.mutex);
}

static unsigned setup_format_cb(void **opaque, char *chroma, unsigned *width, unsigned *height, unsigned *pitches, unsigned *lines) {
    pthread_mutex_lock(&core.mutex);

    fprintf(stderr, "[VLC] setup_format_cb: %ux%u (was %ux%u)\n",
            *width, *height, core.video_width, core.video_height);

    // Always free old buffer first
    if (core.video_buffer) {
        free(core.video_buffer);
        core.video_buffer = NULL;
    }

    memcpy(chroma, "RV32", 4);

   // Clamp to user-selected max resolution
    if (core.max_width > 0 && *width > core.max_width) {
        float aspect = (float)*height / (float)*width;
        *width = core.max_width;
        *height = (unsigned)(core.max_width * aspect + 0.5f);
        if (*height > core.max_height) *height = core.max_height;
    }

    core.video_width = *width;
    core.video_height = *height;
    core.video_pitch = *width * 4;

    core.video_buffer = (uint32_t*)calloc(1, (size_t)core.video_pitch * core.video_height);

    if (!core.video_buffer) {
        fprintf(stderr, "[VLC] CRITICAL: Failed to allocate %ux%u buffer!\n", core.video_width, core.video_height);
        core.video_width = core.video_height = 0;
        pthread_mutex_unlock(&core.mutex);
        return 0;
    }

    core.transitioning = false;   // we are ready

    *pitches = core.video_pitch;
    *lines = core.video_height;

    pthread_mutex_unlock(&core.mutex);
    return 1;
}

static void video_display_cb(void *data, void *id) {
    // Called when a video frame is displayed
}

void vlc_video_setup_callbacks(libvlc_media_player_t *mp) {
    libvlc_video_set_callbacks(mp, lock_cb, unlock_cb, video_display_cb, NULL);
    libvlc_video_set_format_callbacks(mp, setup_format_cb, NULL);
}