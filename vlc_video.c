#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include "vlc_core.h"
static int stitching = 0;
static int frames_decoded = 0;  // incremented in lock_cb; guards stitch path at startup
static void *lock_cb(void *data, void **planes) {
    /* If a same-res stitch was in progress, VLC calling lock means the gap is over */
    if (stitching) {
        core.transitioning = false;
        stitching = 0;
    }
    frames_decoded++;  // we've had at least one real decode cycle

    pthread_mutex_lock(&core.mutex);   // lock — hold until unlock_cb

    if (!core.video_buffer || core.video_width == 0 || core.video_height == 0) {
        static uint32_t fallback_buffer[640 * 360] = {0};
        *planes = fallback_buffer;
        // DO NOT unlock here — unlock_cb will do it
        return NULL;
    }

    *planes = core.video_buffer;
    // DO NOT unlock here — unlock_cb will do it
    return NULL;
}

static void unlock_cb(void *data, void *id, void *const *planes) {
    pthread_mutex_unlock(&core.mutex);  // sole unlock point
}

static unsigned setup_format_cb(void **opaque, char *chroma, unsigned *width, unsigned *height, unsigned *pitches, unsigned *lines) {
 


    memcpy(chroma, "RV32", 4);

    // Clamp resolution BEFORE touching mutex
    if (core.max_width > 0 && *width > core.max_width) {
        float aspect = (float)*height / (float)*width;
        *width = core.max_width;
        *height = (unsigned)(core.max_width * aspect + 0.5f);
        if (*height > core.max_height) *height = core.max_height;
    }

    unsigned new_pitch = *width * 4;

    // *** Allocate OUTSIDE mutex — this is the slow part, don't block retro_run ***
    uint32_t *new_buf = (uint32_t*)calloc(1, (size_t)new_pitch * *height);
    if (!new_buf) {
        fprintf(stderr, "[VLC] CRITICAL: Failed to allocate %ux%u buffer!\n", *width, *height);
        core.transitioning = false;
        return 0;
    }
/* Ignore HLS stitch resets if resolution is identical */
if (core.video_buffer &&
    core.video_width  == *width &&
    core.video_height == *height &&
    core.video_pitch  == new_pitch)
{
    free(new_buf);   // not needed — existing buffer is reused

    *pitches = core.video_pitch;
    *lines   = core.video_height;

    /* Only gate audio if we've already been decoding — HLS calls setup_format_cb
       twice at startup before any frames arrive, which would otherwise lock forever */
    if (frames_decoded > 0) {
        stitching = 1;
        core.transitioning = true;
        fprintf(stderr, "[VLC] Stitch reset (%ux%u), audio gated\n", *width, *height);
    } else {
        fprintf(stderr, "[VLC] Ignoring early stitch reset (%ux%u), no frames yet\n", *width, *height);
    }
    return 1;
}

/* Normal buffer swap — reset frame counter for the new stream */
frames_decoded = 0;
   core.transitioning = true;
    // Fast pointer swap under mutex — retro_run blocks for microseconds only
    pthread_mutex_lock(&core.mutex);


    free(core.video_buffer);          // old buffer freed here — no null window
    core.video_buffer = new_buf;      // immediately valid, no black frame gap
    core.video_width  = *width;
    core.video_height = *height;
    core.video_pitch  = new_pitch;
	libvlc_audio_set_delay(core.mp, 0);
 		core.transitioning = false;
		
   pthread_mutex_unlock(&core.mutex);

    fprintf(stderr, "[VLC] setup_format_cb: %ux%u\n", *width, *height);

    *pitches = new_pitch;
    *lines   = *height;
    return 1;
}

static void video_display_cb(void *data, void *id) {
   
}

void vlc_video_setup_callbacks(libvlc_media_player_t *mp) {
    libvlc_video_set_callbacks(mp, lock_cb, unlock_cb, video_display_cb, NULL);
    libvlc_video_set_format_callbacks(mp, setup_format_cb, NULL);
}