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

/* ── Ring buffer ─────────────────────────────────────────────────────────── */

#define AUDIO_RING_PAIRS 288000   /* 6 s at 48 kHz stereo */

typedef struct {
    int16_t data[AUDIO_RING_PAIRS * 2];
    size_t  w;
    size_t  r;
    size_t  fill;
} audio_ring_t;

/* ── State ───────────────────────────────────────────────────────────────── */

static audio_ring_t    rb[2];                        /* rb[0] and rb[1]              */
static int             audio_write_buf = 0;          /* VLC writes here              */
static int             audio_read_buf  = 0;          /* retro_run reads here         */
static bool            audio_output_enabled = false; /* gated until first video frame */
static pthread_mutex_t rb_mtx = PTHREAD_MUTEX_INITIALIZER;
static int64_t         fill_start_us = 0;            /* when current write buf began */
static bool startup_sync_done = false;
static int64_t startup_video_pts = 0;
static int64_t startup_audio_pts = 0;
static bool startup_video_seen = false;

/*
 * audio_last_pts — lifted out of audio_play to module level so that
 * vlc_audio_ring_reset() (called by audio_flush on every seek/stop) can zero
 * it.  Without this, the first audio packet after a seek is compared against
 * the last packet from before the seek, producing a large PTS delta that
 * incorrectly fires the stitch machinery.
 */
static int64_t audio_last_pts = 0;
static int audio_settle_packets = 0;
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

/* ── Internal write (VLC audio thread) ──────────────────────────────────── */

static void audio_ring_write(const int16_t *src, size_t pairs)
{
    audio_ring_t *r = &rb[audio_write_buf];

    /* Overflow: advance read pointer to make room, dropping oldest samples. */
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

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * vlc_audio_ring_read
 *
 * Called by retro_run once per frame.  Drains rb[audio_read_buf].
 *
 * Audio is held silent until vlc_audio_enable() is called (which happens the
 * moment the first video frame of a stream arrives).  This ensures audio and
 * video start together even if VLC's audio decoder gets a head-start.
 *
 * During a dual-buffer stitch the old read buffer simply keeps draining here
 * with no extra gating; silence arrives naturally when it runs dry, and
 * vlc_stitch_try_commit() flips read_buf to the staged buffer once that happens.
 */
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

    /* Zero-pad the tail if the buffer ran dry (natural silence). */
    if (avail < pairs)
        memset(dst + avail * 2, 0, (pairs - avail) * sizeof(int16_t) * 2);

    pthread_mutex_unlock(&rb_mtx);
}

/*
 * vlc_audio_enable
 *
 * Called by retro_run the instant the first video frame of a (new) stream is
 * rendered.  Opens the audio output gate so audio and video start together
 * even if VLC's audio decoder got a head-start.
 *
 * On a stitch the gate is already open (we never re-close it mid-stream), so
 * this is a no-op in that path.
 */
void vlc_audio_enable(void)
{
    pthread_mutex_lock(&rb_mtx);

    if (!audio_output_enabled) {
		

        audio_output_enabled = true;
        fprintf(stderr, "[VLC-AUDIO] Output enabled — audio + video now locked.\n");
    }

    pthread_mutex_unlock(&rb_mtx);
}

/*
 * vlc_stitch_begin
 *
 * Called when a stitch is detected (PTS jump or hard discontinuity).
 * Arms the staging buffer (the one not currently being read) and redirects
 * all VLC audio writes there.  The active read buffer is untouched and
 * continues to drain normally through vlc_audio_ring_read().
 */
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

/*
 * vlc_stitch_commit_audio
 *
 * Called by vlc_stitch_try_commit() once the old read buffer is empty.
 * Flips audio_read_buf to the now-full staging buffer.
 * audio_write_buf and audio_read_buf are equal after this call (normal state).
 */
void vlc_stitch_commit_audio(void)
{
    pthread_mutex_lock(&rb_mtx);
    audio_read_buf = audio_write_buf;
    pthread_mutex_unlock(&rb_mtx);

    fprintf(stderr, "[VLC-AUDIO] Stitch commit: reads -> buf %d\n", audio_read_buf);
}

/*
 * vlc_audio_ring_reset
 *
 * Hard reset.  Both buffers cleared, indices zeroed, output gate closed.
 * Called on VLC flush (seek / media stop) and on Path-B realloc.
 * The gate re-opens on the next vlc_audio_enable() call.
 *
 * Zeroing audio_last_pts here is critical: VLC calls audio_flush (which calls
 * this function) on every seek and stop.  If last_pts were not cleared, the
 * first audio packet after the seek would be compared against the pre-seek
 * PTS, producing a large delta that incorrectly triggers the stitch machinery.
 */
void vlc_audio_ring_reset(void)
{
    pthread_mutex_lock(&rb_mtx);

    rb[0].w = rb[0].r = rb[0].fill = 0;
    rb[1].w = rb[1].r = rb[1].fill = 0;

    audio_write_buf      = 0;
    audio_read_buf       = 0;
    audio_output_enabled = false;
    fill_start_us        = get_time_us();
    audio_last_pts       = 0;   /* ← prevents false stitch on post-seek first packet */
audio_settle_packets = 120; 
    pthread_mutex_unlock(&rb_mtx);

    fprintf(stderr, "[VLC-AUDIO] Hard reset — output gated until next video frame.\n");
}

/* Returns how many sample pairs remain in the active read buffer.
 * Used by vlc_stitch_try_commit() to detect drain completion. */
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

/* ── VLC callbacks ───────────────────────────────────────────────────────── */

static void audio_play(void *data, const void *samples, unsigned count, int64_t pts)
{
    (void)data;
    if (!samples || count == 0) return;

    /*
     * Large PTS jump → real stream discontinuity (ad break / content switch).
     * Arm the dual-buffer stitch so old audio keeps playing while new audio
     * pre-fills the staging buffer.
     *
     * Threshold is 2 seconds (2,000,000 µs).
     *
     * The old 200 ms threshold was far too tight for HLS/IPTV: normal segment
     * boundary jitter easily exceeds 200 ms and was triggering a stitch — and
     * a full pipeline disruption — at every segment seam, causing IPTV streams
     * to stutter or fail to play.  Real content discontinuities (ad breaks,
     * channel switches) are always multiple seconds apart in PTS, so 2 s is a
     * safe floor that catches real events and ignores segment jitter.
     *
     * Seeks cannot trigger a false stitch here because audio_last_pts is a
     * module-level variable (not a static local), so vlc_audio_ring_reset() —
     * called by audio_flush on every seek — zeroes it before the next packet
     * arrives.  When audio_last_pts == 0 the comparison below is skipped.
     */
if (audio_settle_packets > 0) {
    audio_settle_packets--;
} else if (audio_last_pts != 0 && llabs(pts - audio_last_pts) > 2000000) {

    if (core.suppress_next_stitch_event) {
        core.suppress_next_stitch_event = false;
        fprintf(stderr, "[VLC-AUDIO] Suppressed PTS jump stitch (post-seek)\n");
        // do NOT arm stitch
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

    /*
     * audio_write_buf is always correct:
     *   • Before a stitch:  points to the active buffer (same as read_buf).
     *   • After stitch_begin: points to staging.
     * No branching needed here.
     */
    audio_ring_write((const int16_t *)samples, count);
}

static void audio_flush(void *data, int64_t pts)
{
    (void)data; (void)pts;
    /*
     * VLC calls this on every seek and stop.
     * vlc_audio_ring_reset() clears audio_last_pts so the next audio_play
     * call after the seek does not see a false PTS jump and trigger a stitch.
     */
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