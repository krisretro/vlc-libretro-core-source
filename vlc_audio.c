#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include "vlc_core.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define AUDIO_RING_PAIRS 288000   /* 6 seconds at 48 kHz stereo */

static int16_t         aring[AUDIO_RING_PAIRS * 2];
static size_t          aring_w    = 0;
static size_t          aring_r    = 0;
static size_t          aring_fill = 0;
static bool            audio_output_enabled = false;
static pthread_mutex_t aring_mtx  = PTHREAD_MUTEX_INITIALIZER;
static int64_t         reset_time_us = 0;

static int64_t get_time_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (counter.QuadPart * 1000000) / freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}

static void audio_ring_write(const int16_t *src, size_t pairs)
{
    if (aring_fill + pairs > AUDIO_RING_PAIRS) {
        size_t drop = (aring_fill + pairs) - AUDIO_RING_PAIRS;
        aring_r    = (aring_r + drop) % AUDIO_RING_PAIRS;
        aring_fill -= drop;
        fprintf(stderr, "[VLC-AUDIO] Ring overflow — dropped %zu pairs\n", drop);
    }
    for (size_t i = 0; i < pairs; i++) {
        aring[aring_w * 2]     = src[i * 2];
        aring[aring_w * 2 + 1] = src[i * 2 + 1];
        aring_w = (aring_w + 1) % AUDIO_RING_PAIRS;
    }
    aring_fill += pairs;
}

void vlc_audio_ring_read(int16_t *dst, size_t pairs)
{
    pthread_mutex_lock(&aring_mtx);

    // This is the "Gate". If a stitch is happening OR audio was explicitly disabled,
    // we send silence. This keeps the frontend clock running while the video catches up.
    if (core.stitch_resync_pending || !audio_output_enabled)
    {
        memset(dst, 0, pairs * sizeof(int16_t) * 2);
        pthread_mutex_unlock(&aring_mtx);
        return;
    }

    size_t avail = (aring_fill < pairs) ? aring_fill : pairs;
    for (size_t i = 0; i < avail; i++) {
        dst[i * 2]     = aring[aring_r * 2];
        dst[i * 2 + 1] = aring[aring_r * 2 + 1];
        aring_r = (aring_r + 1) % AUDIO_RING_PAIRS;
    }
    aring_fill -= avail;

    if (avail < pairs)
        memset(dst + avail * 2, 0, (pairs - avail) * sizeof(int16_t) * 2);

    pthread_mutex_unlock(&aring_mtx);
}

void vlc_audio_ring_reset(void)
{
    pthread_mutex_lock(&aring_mtx);
    aring_w = aring_r = aring_fill = 0;
    
    // CHANGE: Keep this true. 
    // We use stitch_resync_pending to handle the silence gap now.
    audio_output_enabled = true; 
    
    reset_time_us = get_time_us();
    pthread_mutex_unlock(&aring_mtx);
    fprintf(stderr, "[VLC-AUDIO] Ring reset, ready for data.\n");
}
void vlc_audio_sync_and_enable(int64_t elapsed_us)
{
  
    audio_output_enabled = true;


}

bool vlc_audio_is_output_enabled(void)
{
    pthread_mutex_lock(&aring_mtx);
    bool e = audio_output_enabled;
    pthread_mutex_unlock(&aring_mtx);
    return e;
}

int64_t vlc_audio_get_reset_time_us(void)
{
    pthread_mutex_lock(&aring_mtx);
    int64_t t = reset_time_us;
    reset_time_us = 0;
    pthread_mutex_unlock(&aring_mtx);
    return t;
}

static void audio_play(void *data, const void *samples, unsigned count, int64_t pts)
{
    (void)data; (void)pts;
    if (!samples || count == 0) return;

    // We write to the ring buffer regardless of whether output is enabled.
    // This builds the "pre-delay" necessary to match the video pipeline.
    audio_ring_write(samples, count);
}

static void audio_flush(void *data, int64_t pts)
{
    (void)data; (void)pts;
    vlc_audio_ring_reset();
}

void vlc_audio_setup_callbacks(libvlc_media_player_t *mp)
{
    libvlc_audio_set_callbacks(mp,
        audio_play,
        NULL,
        NULL,
        audio_flush,
        NULL,
        NULL);
    libvlc_audio_set_format(mp, "S16N", AUDIO_TARGET_RATE, 2);
}