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

#define AUDIO_RING_PAIRS 288000   /* 6 s at 48 kHz stereo */

typedef struct {
    int16_t data[AUDIO_RING_PAIRS * 2];
    size_t  w;
    size_t  r;
    size_t  fill;
} audio_ring_t;

static audio_ring_t    rb[2];
static int             audio_write_buf = 0;
static int             audio_read_buf  = 0;
static bool            audio_output_enabled = false;
static pthread_mutex_t rb_mtx = PTHREAD_MUTEX_INITIALIZER;
static int64_t         fill_start_us = 0;
static bool startup_sync_done = false;
static int64_t startup_video_pts = 0;
static int64_t startup_audio_pts = 0;
static bool startup_video_seen = false;
static int64_t audio_last_pts = 0;
static int audio_settle_packets = 0;

static int64_t get_time_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (counter.QuadPart * 1000000LL) / freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
#endif
}

static void audio_ring_write(const int16_t *src, size_t pairs)
{
    audio_ring_t *r = &rb[audio_write_buf];

    if (r->fill + pairs > AUDIO_RING_PAIRS) {
        size_t drop = (r->fill + pairs) - AUDIO_RING_PAIRS;
        r->r    = (r->r + drop) % AUDIO_RING_PAIRS;
        r->fill -= drop;
        fprintf(stderr, "[VLC-AUDIO] Overflow in buf %d — dropped %zu pairs\n",
                audio_write_buf, drop);
    }

    for (size_t i = 0; i < pairs; i++) {
        r->data[r->w * 2]     = src[i * 2];
        r->data[r->w * 2 + 1] = src[i * 2 + 1];
        r->w = (r->w + 1) % AUDIO_RING_PAIRS;
    }
    r->fill += pairs;
}

void vlc_audio_ring_read(int16_t *dst, size_t pairs)
{
    pthread_mutex_lock(&rb_mtx);

    if (!audio_output_enabled) {
        memset(dst, 0, pairs * sizeof(int16_t) * 2);
        pthread_mutex_unlock(&rb_mtx);
        return;
    }

    audio_ring_t *abuf = &rb[audio_read_buf];
    size_t avail = (abuf->fill < pairs) ? abuf->fill : pairs;

    for (size_t i = 0; i < avail; i++) {
        dst[i * 2]     = abuf->data[abuf->r * 2];
        dst[i * 2 + 1] = abuf->data[abuf->r * 2 + 1];
        abuf->r = (abuf->r + 1) % AUDIO_RING_PAIRS;
    }
    abuf->fill -= avail;

    if (avail < pairs)
        memset(dst + avail * 2, 0, (pairs - avail) * sizeof(int16_t) * 2);

    pthread_mutex_unlock(&rb_mtx);
}

void vlc_audio_enable(void)
{
 
    if (!audio_output_enabled) {
        audio_output_enabled = true;
        fprintf(stderr, "[VLC-AUDIO] Output enabled — audio + video now locked.\n");
    }

 
}

void vlc_stitch_begin(void)
{
    pthread_mutex_lock(&rb_mtx);

    int staging = 1 - audio_read_buf;
    rb[staging].w    = 0;
    rb[staging].r    = 0;
    rb[staging].fill = 0;
    audio_write_buf  = staging;
    fill_start_us    = get_time_us();

    pthread_mutex_unlock(&rb_mtx);

    fprintf(stderr,
            "[VLC-AUDIO] Stitch begin: writes -> buf %d  |  draining buf %d\n",
            1 - audio_read_buf, audio_read_buf);
}

void vlc_stitch_commit_audio(void)
{
    pthread_mutex_lock(&rb_mtx);
    audio_read_buf = audio_write_buf;
    pthread_mutex_unlock(&rb_mtx);

    fprintf(stderr, "[VLC-AUDIO] Stitch commit: reads -> buf %d\n", audio_read_buf);
}

void vlc_audio_ring_reset(void)
{
    pthread_mutex_lock(&rb_mtx);

    rb[0].w = rb[0].r = rb[0].fill = 0;
    rb[1].w = rb[1].r = rb[1].fill = 0;

    audio_write_buf      = 0;
    audio_read_buf       = 0;
    audio_output_enabled = false;
    fill_start_us        = get_time_us();
    audio_last_pts       = 0;
    audio_settle_packets = 120;

    pthread_mutex_unlock(&rb_mtx);

    fprintf(stderr, "[VLC-AUDIO] Hard reset — output gated until next video frame.\n");
}

size_t vlc_audio_read_buf_fill(void)
{
    pthread_mutex_lock(&rb_mtx);
    size_t f = rb[audio_read_buf].fill;
    pthread_mutex_unlock(&rb_mtx);
    return f;
}

bool vlc_audio_is_output_enabled(void)
{
    pthread_mutex_lock(&rb_mtx);
    bool e = audio_output_enabled;
    pthread_mutex_unlock(&rb_mtx);
    return e;
}

static void audio_play(void *data, const void *samples, unsigned count, int64_t pts)
{
    (void)data;
    if (!samples || count == 0) return;

    if (audio_settle_packets > 0) {
        audio_settle_packets--;
    } else if (audio_last_pts != 0 && llabs(pts - audio_last_pts) > 2000000) {
        if (core.suppress_next_stitch_event) {
            core.suppress_next_stitch_event = false;
            fprintf(stderr, "[VLC-AUDIO] Suppressed PTS jump stitch (post-seek)\n");
        } else {
            fprintf(stderr, "[VLC-AUDIO] PTS jump %lld us — stitch triggered\n",
                    (long long)(pts - audio_last_pts));

            pthread_mutex_lock(&core.mutex);
            if (!core.stitch_switch_pending) {
                core.stitch_switch_pending = true;
                core.stitch_resync_pending = true;
                vlc_stitch_begin();
                vlc_video_stitch_and_flush();
            }
            pthread_mutex_unlock(&core.mutex);
        }
    }
    audio_last_pts = pts;

    audio_ring_write((const int16_t *)samples, count);
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