#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include "vlc_core.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* ── Configuration ───────────────────────────────────────────────────────── */
#define AUDIO_RING_PAIRS_DVD   96000
#define AUDIO_RING_PAIRS_OTHER 288000

#define AUDIO_TAIL_KEEP_PAIRS_DVD    4800
#define AUDIO_TAIL_KEEP_PAIRS_OTHER   5760

#define AUDIO_TRANSITION_GRACE_US_DVD   100000
#define AUDIO_TRANSITION_GRACE_US_OTHER  120000

/* ── Time helper ─────────────────────────────────────────────────────────── */
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

/* ── Ring Buffer ─────────────────────────────────────────────────────────── */
typedef struct {
    int16_t *data;
    size_t   size;
    size_t   w;
    size_t   r;
    size_t   fill;
} audio_ring_t;

static audio_ring_t rb[2];
static int audio_write_buf = 0;
static int audio_read_buf  = 0;

static pthread_mutex_t rb_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Audio output state */
static bool audio_output_enabled = true;

/* ── Transition state ── */
typedef enum {
    AUDIO_STATE_PLAYING = 0,
    AUDIO_STATE_DRAINING
} audio_state_t;

static audio_state_t audio_state = AUDIO_STATE_PLAYING;
static bool audio_transition_seen_new = false;
static int64_t audio_transition_deadline_us = 0;
static size_t audio_tail_keep_pairs = AUDIO_TAIL_KEEP_PAIRS_OTHER;

/* Timing state */
static int64_t audio_last_pts = 0;

/* ── size helpers ── */
static size_t get_ring_size(void)
{
    return core.isDVD ? AUDIO_RING_PAIRS_DVD : AUDIO_RING_PAIRS_OTHER;
}

static size_t get_tail_keep_pairs(void)
{
    return core.isDVD ? AUDIO_TAIL_KEEP_PAIRS_DVD : AUDIO_TAIL_KEEP_PAIRS_OTHER;
}

static int64_t get_transition_grace_us(void)
{
    return core.isDVD ? AUDIO_TRANSITION_GRACE_US_DVD : AUDIO_TRANSITION_GRACE_US_OTHER;
}

/* ── allocate ── */
static bool allocate_rings(void)
{
    size_t size = get_ring_size();

    for (int i = 0; i < 2; i++) {
        free(rb[i].data);
        rb[i].data = (int16_t*)calloc(size * 2, sizeof(int16_t));
        if (!rb[i].data) {
            fprintf(stderr, "[VLC-AUDIO] Out of memory allocating ring\n");
            return false;
        }

        rb[i].size = size;
        rb[i].w = rb[i].r = rb[i].fill = 0;
    }

    return true;
}

static void reset_ring(int i)
{
    rb[i].w = rb[i].r = rb[i].fill = 0;
}

/* ── transition start ── */
static void start_transition_locked(void)
{
    if (audio_state == AUDIO_STATE_DRAINING) {
        fprintf(stderr, "[VLC-AUDIO] Transition already active — ignoring\n");
        return;
    }

    int staging = 1 - audio_read_buf;

    reset_ring(staging);

    audio_write_buf = staging;
    audio_state = AUDIO_STATE_DRAINING;
    audio_transition_seen_new = false;
    audio_transition_deadline_us = get_time_us() + get_transition_grace_us();
    audio_tail_keep_pairs = get_tail_keep_pairs();

    fprintf(stderr,
        "[VLC-AUDIO] Transition begin: read=%d write=%d grace=%lldus\n",
        audio_read_buf, audio_write_buf,
        (long long)get_transition_grace_us());
}
/* ── commit switch ── */
static void maybe_commit_switch_locked(void)
{
    if (audio_state != AUDIO_STATE_DRAINING) {
        return;
    }

    audio_ring_t *oldbuf = &rb[audio_read_buf];
    int64_t now_us = get_time_us();

    bool deadline_hit = (now_us >= audio_transition_deadline_us);
    bool near_end = (oldbuf->fill <= audio_tail_keep_pairs);

    fprintf(stderr,
        "[VLC-AUDIO] Drain check: old_fill=%zu new_seen=%d near_end=%d deadline=%d\n",
        oldbuf->fill,
        audio_transition_seen_new ? 1 : 0,
        near_end ? 1 : 0,
        deadline_hit ? 1 : 0);

    if ((audio_transition_seen_new && near_end) || deadline_hit) {
        audio_read_buf = audio_write_buf;
        audio_state = AUDIO_STATE_PLAYING;
        audio_transition_seen_new = false;

        fprintf(stderr, "[VLC-AUDIO] Transition commit: reads -> buffer %d\n", audio_read_buf);
    }
}

/* ── write audio ── */
static void audio_ring_write_locked(const int16_t *src, size_t pairs)
{
    audio_ring_t *wbuf = &rb[audio_write_buf];

    if (!wbuf->data || wbuf->size == 0) {
        return;
    }

    if (pairs > wbuf->size) {
        src += (pairs - wbuf->size) * 2;
        pairs = wbuf->size;
    }

    if (wbuf->fill + pairs > wbuf->size) {
        size_t drop = (wbuf->fill + pairs) - wbuf->size;
        wbuf->r = (wbuf->r + drop) % wbuf->size;
        wbuf->fill -= drop;
        fprintf(stderr, "[VLC-AUDIO] Overflow in buf %d — dropped %zu pairs (fill=%zu size=%zu)\n",
                audio_write_buf, drop, wbuf->fill, wbuf->size);
    }

    for (size_t i = 0; i < pairs; i++) {
        wbuf->data[wbuf->w * 2]     = src[i * 2];
        wbuf->data[wbuf->w * 2 + 1] = src[i * 2 + 1];
        wbuf->w = (wbuf->w + 1) % wbuf->size;
    }

    wbuf->fill += pairs;
}

/* ── audio callback ── */
static void audio_play(void *data, const void *samples, unsigned count, int64_t pts)
{
    (void)data;
    if (!samples || count == 0) return;

    pthread_mutex_lock(&rb_mtx);

    /* Track audio continuity */
    if (audio_last_pts != 0 && llabs(pts - audio_last_pts) > 2000000) {
        fprintf(stderr, "[VLC-AUDIO] Detected PTS jump %lld us\n",
                (long long)(pts - audio_last_pts));

        pthread_mutex_lock(&core.mutex);

        /* For DVD, PTS jumps are normal during menu→chapter navigation.
         * Previously we suppressed stitches here, but now DVD transitions
         * are handled via audio_flush, so we still suppress here. */
        bool suppressed = core.isDVD
                       || core.stitch_switch_pending
                       || core.suppress_next_stitch_event;

        if (core.suppress_next_stitch_event) {
            core.suppress_next_stitch_event = false;
            fprintf(stderr, "[VLC-AUDIO] PTS jump suppressed (suppress flag)\n");
        }

        if (!suppressed) {
            fprintf(stderr, "[VLC-AUDIO] Requesting stitch from PTS jump\n");
            core.stitch_switch_pending = true;
            core.stitch_resync_pending = true;
            vlc_stitch_begin();
            vlc_video_stitch_and_flush();
        }

        pthread_mutex_unlock(&core.mutex);
    }
    audio_last_pts = pts;

    if (audio_state == AUDIO_STATE_DRAINING && !audio_transition_seen_new) {
        audio_transition_seen_new = true;
        fprintf(stderr, "[VLC-AUDIO] First new audio packet (PTS=%lld)\n", (long long)pts);
    }

    audio_ring_write_locked((const int16_t*)samples, count);
    maybe_commit_switch_locked();

    pthread_mutex_unlock(&rb_mtx);
}

/* ── read audio ── */
void vlc_audio_ring_read(int16_t *dst, size_t pairs)
{
    pthread_mutex_lock(&rb_mtx);

    if (!audio_output_enabled) {
        memset(dst, 0, pairs * sizeof(int16_t) * 2);
        pthread_mutex_unlock(&rb_mtx);
        return;
    }

    audio_ring_t *r = &rb[audio_read_buf];
    size_t avail = r->fill < pairs ? r->fill : pairs;

    for (size_t i = 0; i < avail; i++) {
        dst[i * 2]     = r->data[r->r * 2];
        dst[i * 2 + 1] = r->data[r->r * 2 + 1];
        r->r = (r->r + 1) % r->size;
    }

    r->fill -= avail;

    if (avail < pairs) {
        memset(dst + avail * 2, 0, (pairs - avail) * sizeof(int16_t) * 2);
    }

    maybe_commit_switch_locked();

    pthread_mutex_unlock(&rb_mtx);
}

/* ── REQUIRED EXPORTS ── */
size_t vlc_audio_read_buf_fill(void)
{
    pthread_mutex_lock(&rb_mtx);
    size_t f = rb[audio_read_buf].fill;
    pthread_mutex_unlock(&rb_mtx);
    return f;
}

void vlc_stitch_commit_audio(void)
{
    pthread_mutex_lock(&rb_mtx);
    maybe_commit_switch_locked();
    pthread_mutex_unlock(&rb_mtx);
}
/* ── disable ── */
void vlc_audio_disable(void)
{
    pthread_mutex_lock(&rb_mtx);
    audio_output_enabled = false;
    pthread_mutex_unlock(&rb_mtx);

    fprintf(stderr, "[VLC-AUDIO] Output disabled (start gate closed)\n");
}
/* ── enable ── */
void vlc_audio_enable(void)
{
    pthread_mutex_lock(&rb_mtx);
    audio_output_enabled = true;
    pthread_mutex_unlock(&rb_mtx);

    fprintf(stderr, "[VLC-AUDIO] Output enabled\n");
}

/* ── transition ── */
void vlc_stitch_begin(void)
{
    pthread_mutex_lock(&rb_mtx);
    start_transition_locked();
    pthread_mutex_unlock(&rb_mtx);
}

/* ── flush ── */
static void audio_flush(void *data, int64_t pts)
{
    (void)data;
    (void)pts;

    if (core.isDVD) {
        pthread_mutex_lock(&rb_mtx);

        bool already = (audio_state == AUDIO_STATE_DRAINING);

        pthread_mutex_unlock(&rb_mtx);

        if (!already) {
            fprintf(stderr, "[VLC-AUDIO] DVD flush → starting audio stitch (video continues naturally)\n");

            pthread_mutex_lock(&core.mutex);
            core.stitch_switch_pending = true;
            core.stitch_resync_pending = true;
			core.audio_wait_for_sync = true;
            pthread_mutex_unlock(&core.mutex);

            vlc_stitch_begin();
			vlc_video_stitch_and_flush();
            /* Do NOT call vlc_video_stitch_and_flush() here.
             *
             * On a DVD VTS_CHANGE the audio ring can hold up to ~2 s of
             * buffered outtro audio while the video ring only holds ~240 ms
             * (RING_SIZE=6 at 25 fps).  If we arm the video stitch here,
             * video writes are immediately redirected to the staging buffer
             * and the read buffer (buf 0) empties within a few frames,
             * causing a frozen frame for the entire audio drain period.
             * The result is that outtro video cuts off far too early while
             * audio plays the transition in full.
             *
             * For DVD, the VTS boundary is a clean editorial cut — there is
             * no dirty pipeline residual in the video path that needs to be
             * hidden behind a dual-buffer swap.  We let video keep writing
             * and reading from buf 0 so the outtro continues to display
             * uninterrupted.  vlc_video_old_buffer_drained() returns true
             * immediately when no video stitch is armed, so the core commit
             * fires as soon as the audio drain condition is satisfied. */
        } else {
            fprintf(stderr, "[VLC-AUDIO] DVD flush ignored (already stitching)\n");
        }

        return;
    }

    pthread_mutex_lock(&rb_mtx);
    start_transition_locked();
    pthread_mutex_unlock(&rb_mtx);
}
/* ── reset ── */
void vlc_audio_ring_reset(void)
{
    pthread_mutex_lock(&rb_mtx);

    reset_ring(0);
    reset_ring(1);

    audio_read_buf = 0;
    audio_write_buf = 0;

    audio_state = AUDIO_STATE_PLAYING;
    audio_transition_seen_new = false;

    audio_output_enabled = true;
    audio_last_pts = 0;

    pthread_mutex_unlock(&rb_mtx);

    fprintf(stderr, "[VLC-AUDIO] Ring reset (external call)\n");
}

/* ── setup ── */
void vlc_audio_setup_callbacks(libvlc_media_player_t *mp)
{
    if (!allocate_rings()) exit(1);

    audio_output_enabled = true;

    libvlc_audio_set_callbacks(mp,
        audio_play,
        NULL,
        NULL,
        audio_flush,
        NULL,
        NULL);

    libvlc_audio_set_format(mp, "S16N", AUDIO_TARGET_RATE, 2);

    fprintf(stderr, "[VLC-AUDIO] Audio callbacks ready\n");
}