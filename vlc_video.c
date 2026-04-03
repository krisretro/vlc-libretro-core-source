#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include "vlc_core.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* ── Ring buffer ─────────────────────────────────────────────────────────── */

#define RING_SIZE 3

typedef struct {
    uint32_t *buf;
    unsigned  width;
    unsigned  height;
    unsigned  pitch;
    bool      ready;
    unsigned  generation;
} ring_frame_t;

/*
 * Dual-buffer layout
 * ─────────────────
 *  ring[0][0..2]  buffer A (active or staging)
 *  ring[1][0..2]  buffer B (staging or active)
 *
 *  video_write_buf: VLC decode callbacks write here
 *  video_read_buf : retro_run reads from here
 *
 *  Normal state  : write == read  (same buffer)
 *  During stitch : write != read  (staging vs draining)
 *  After commit  : write == read  (new buffer)
 */
static ring_frame_t    ring[2][RING_SIZE];
static int             write_slot[2]   = {0,  0};
static int             display_slot[2] = {-1, -1};
static int             video_write_buf = 0;
static int             video_read_buf  = 0;

static unsigned        current_gen = 0;
static unsigned        expected_gen = 0;
static bool            waiting_for_real_frame = false;

static unsigned        ring_alloc_width  = 0;
static unsigned        ring_alloc_height = 0;
static unsigned        ring_alloc_pitch  = 0;
static unsigned        ring_width  = 0;
static unsigned        ring_height = 0;
static unsigned        ring_pitch  = 0;

static bool            pending_release     = false;
static int64_t         first_frame_time_us = 0;

static pthread_mutex_t ring_mtx = PTHREAD_MUTEX_INITIALIZER;

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

/* ── Allocation helpers ──────────────────────────────────────────────────── */

static void ring_free_all(void)
{
    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < RING_SIZE; i++) {
            free(ring[b][i].buf);
            ring[b][i].buf   = NULL;
            ring[b][i].ready = false;
        }
        write_slot[b]   = 0;
        display_slot[b] = -1;
    }
}

static bool ring_alloc(unsigned max_w, unsigned max_h)
{
    ring_free_all();
    unsigned pitch = max_w * 4;

    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < RING_SIZE; i++) {
            ring[b][i].buf = (uint32_t *)calloc(1, (size_t)pitch * max_h);
            if (!ring[b][i].buf) {
                fprintf(stderr, "[VLC] ring_alloc OOM slot %d buf %d (%ux%u)\n",
                        i, b, max_w, max_h);
                ring_free_all();
                return false;
            }
            ring[b][i].ready = false;
        }
    }

    ring_alloc_width  = max_w;
    ring_alloc_height = max_h;
    ring_alloc_pitch  = pitch;
    return true;
}

/* ── VLC video callbacks ─────────────────────────────────────────────────── */

static void *lock_cb(void *data, void **planes)
{
    (void)data;
    pthread_mutex_lock(&ring_mtx);

    if (!ring[video_write_buf][write_slot[video_write_buf]].buf) {
        /* Fallback scratch buffer (should not normally happen). */
        static uint32_t scratch[640 * 360];
        *planes = scratch;
        return NULL;
    }

    ring[video_write_buf][write_slot[video_write_buf]].generation = current_gen;
    *planes = ring[video_write_buf][write_slot[video_write_buf]].buf;
    return (void *)(intptr_t)write_slot[video_write_buf];
}

static void unlock_cb(void *data, void *id, void *const *planes)
{
    (void)data; (void)id; (void)planes;
    pthread_mutex_unlock(&ring_mtx);
}

static void display_cb(void *data, void *id)
{
    (void)data;
    pthread_mutex_lock(&ring_mtx);

    int slot = (int)(intptr_t)id;
    if (slot < 0 || slot >= RING_SIZE) {
        pthread_mutex_unlock(&ring_mtx);
        return;
    }

    /* Discard frames that belong to a previous generation (pre-stitch debris). */
    if (ring[video_write_buf][slot].generation != current_gen) {
        pthread_mutex_unlock(&ring_mtx);
        return;
    }

    if (waiting_for_real_frame) {
		fprintf(stderr, "[VIDEO] display_cb called\n");
		fprintf(stderr, "[VIDEO] display_cb called\n");
		fprintf(stderr, "[VIDEO] display_cb called\n");
		fprintf(stderr, "[VIDEO] display_cb called\n");
		fprintf(stderr, "[VIDEO] display_cb called\n");
        waiting_for_real_frame = false;
        pending_release        = true;
 vlc_audio_ring_reset(); 
		  vlc_audio_enable();
        first_frame_time_us    = get_time_us();
        fprintf(stderr, "[VLC] First real frame of gen %u\n", expected_gen);
    }

    ring[video_write_buf][slot].width  = ring_width;
    ring[video_write_buf][slot].height = ring_height;
    ring[video_write_buf][slot].pitch  = ring_pitch;
    ring[video_write_buf][slot].ready  = true;
    display_slot[video_write_buf]      = slot;

    pthread_mutex_unlock(&ring_mtx);
}

/* ── Public stitch helpers ───────────────────────────────────────────────── */

/*
 * vlc_video_flush_display
 *
 * Invalidate every slot so no stale pre-stitch frame leaks through.
 * Called on hard reset / media stop.
 */
void vlc_video_flush_display(void)
{
    pthread_mutex_lock(&ring_mtx);
    display_slot[0] = -1;
    display_slot[1] = -1;
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < RING_SIZE; i++)
            ring[b][i].ready = false;
    pthread_mutex_unlock(&ring_mtx);

    fprintf(stderr, "[VLC] Video ring flushed — stale frames discarded.\n");
}

/*
 * vlc_video_stitch_and_flush
 *
 * Arms the staging video buffer: bumps the generation counter and redirects
 * decode writes to buf 1-read_buf.  Old frames already in read_buf are still
 * visible through vlc_video_get_frame() until they are consumed.
 *
 * Called from the audio PTS-jump path (while core.mutex is held by caller).
 */
void vlc_video_stitch_and_flush(void)
{
    pthread_mutex_lock(&ring_mtx);

    current_gen++;
    expected_gen           = current_gen;
    waiting_for_real_frame = true;

    int staging        = 1 - video_read_buf;
    video_write_buf    = staging;
    write_slot[staging]   = 0;
    display_slot[staging] = -1;
    for (int i = 0; i < RING_SIZE; i++)
        ring[staging][i].ready = false;

    pthread_mutex_unlock(&ring_mtx);

    fprintf(stderr, "[VLC] Video stitch armed: writes -> buf %d  |  reading buf %d (gen %u)\n",
            staging, video_read_buf, current_gen);
}

/*
 * vlc_video_stitch_commit
 *
 * Flips video_read_buf to the staging buffer (now full of new-stream frames)
 * and prepares the old buffer to become the next staging buffer.
 *
 * Called by vlc_stitch_try_commit() in vlc_core.c once audio has also drained.
 */
void vlc_video_stitch_commit(void)
{
    pthread_mutex_lock(&ring_mtx);

    int old_read   = video_read_buf;
    video_read_buf = video_write_buf;   /* start reading from staging */

    /* Reset the now-retired buffer so it can serve as staging next time. */
    display_slot[old_read] = -1;
    write_slot[old_read]   = 0;
    for (int i = 0; i < RING_SIZE; i++)
        ring[old_read][i].ready = false;

    pthread_mutex_unlock(&ring_mtx);

    fprintf(stderr, "[VLC] Video stitch commit: reads -> buf %d\n", video_read_buf);
}

/* ── Format negotiation ──────────────────────────────────────────────────── */

static unsigned setup_format_cb(void **opaque, char *chroma,
                                unsigned *width, unsigned *height,
                                unsigned *pitches, unsigned *lines)
{
    (void)opaque;
    memcpy(chroma, "RV32", 4);

    /* Honour fixed IPTV resolution if set. */
    if (core.iptv_menu_enabled && core.iptv_fixed_width > 0) {
        *width  = core.iptv_fixed_width;
        *height = core.iptv_fixed_height;
    }

    unsigned max_w, max_h;
    if (core.iptv_menu_enabled && core.iptv_fixed_width > 0) {
        max_w = core.iptv_fixed_width;
        max_h = core.iptv_fixed_height;
    } else {
        max_w = core.max_width;
        max_h = core.max_height;
    }

    /* Clamp to max while preserving aspect ratio. */
    if (*width > max_w) {
        float aspect = (float)*height / (float)*width;
        *width  = max_w;
        *height = (unsigned)(max_w * aspect + 0.5f);
        if (*height > max_h) *height = max_h;
    }
    if (*height > max_h) {
        float aspect = (float)*width / (float)*height;
        *height = max_h;
        *width  = (unsigned)(max_h * aspect + 0.5f);
    }

    unsigned new_pitch = *width * 4;

    /* Consume the hard-discontinuity flag set by vlc_event_cb. */
    pthread_mutex_lock(&core.mutex);
core.hard_discontinuity = core.true_discontinuity_pending;
    core.true_discontinuity_pending = false;
    pthread_mutex_unlock(&core.mutex);

    bool needs_realloc = (*width > ring_alloc_width || *height > ring_alloc_height);

    /* ── PATH A: buffer large enough, no realloc ── */
    if (!needs_realloc) {
        pthread_mutex_lock(&ring_mtx);

        if (current_gen == 0) {
            /* ── Initial load ── */
            current_gen            = 1;
            expected_gen           = 1;
            waiting_for_real_frame = true;
            video_write_buf        = 0;
            video_read_buf         = 0;
            display_slot[0]        = -1;
            display_slot[1]        = -1;

            pthread_mutex_unlock(&ring_mtx);

            /* Hard reset audio so it fills from scratch and waits for video. */
             vlc_audio_ring_reset();

            pthread_mutex_lock(&core.mutex);
            core.stitch_resync_pending = true;
            core.stitch_switch_pending = false;
            pthread_mutex_unlock(&core.mutex);

            fprintf(stderr, "[VLC] Path A initial load (gen 1).\n");

        }  else {
            /*
             * ── Seamless format change or stitch already armed by audio_play ──
             * Just bump the generation so stale frames are rejected; the staging
             * buffers were already set up by vlc_video_stitch_and_flush().
             */
            if (!core.stitch_switch_pending) {
                /* Truly seamless (e.g. resolution change on same stream). */
                current_gen++;
                expected_gen           = current_gen;
                waiting_for_real_frame = true;
                /* writes stay on the same buffer (no staging swap needed) */

                pthread_mutex_unlock(&ring_mtx);
                // vlc_audio_ring_reset();   /* restart audio sync for new format */

                pthread_mutex_lock(&core.mutex);
                core.stitch_resync_pending = true;
                pthread_mutex_unlock(&core.mutex);

                fprintf(stderr, "[VLC] Path A seamless format change (gen %u).\n",
                        current_gen);
            } else {
                /* Stitch already armed (PTS jump path) — nothing to do here. */
                pthread_mutex_unlock(&ring_mtx);
                fprintf(stderr, "[VLC] Path A: stitch already in flight (gen %u).\n",
                        current_gen);
            }
        }

        pthread_mutex_lock(&ring_mtx);
        ring_width  = *width;
        ring_height = *height;
        ring_pitch  = new_pitch;
        pthread_mutex_unlock(&ring_mtx);

        *pitches = new_pitch;
        *lines   = *height;
        return 1;
    }

    /* ── PATH B: buffer too small — full realloc ── */
    fprintf(stderr, "[VLC] Path B: realloc (%ux%u -> %ux%u).\n",
            ring_alloc_width, ring_alloc_height, max_w, max_h);

    pthread_mutex_lock(&core.mutex);
    pthread_mutex_lock(&ring_mtx);

    if (!ring_alloc(max_w, max_h)) {
        pthread_mutex_unlock(&ring_mtx);
        pthread_mutex_unlock(&core.mutex);
        return 0;
    }

    ring_width  = *width;
    ring_height = *height;
    ring_pitch  = new_pitch;

    current_gen++;
    expected_gen           = current_gen;
    waiting_for_real_frame = true;
    video_write_buf        = 0;
    video_read_buf         = 0;
    display_slot[0]        = -1;
    display_slot[1]        = -1;

    core.stitch_resync_pending = true;
    core.stitch_switch_pending = false;

    core.video_width  = *width;
    core.video_height = *height;
    core.video_pitch  = new_pitch;

    struct retro_game_geometry geo = {
        .base_width   = *width,
        .base_height  = *height,
        .max_width    = max_w,
        .max_height   = max_h,
        .aspect_ratio = (float)*width / (float)*height
    };
    environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geo);

    pthread_mutex_unlock(&ring_mtx);
    pthread_mutex_unlock(&core.mutex);

    /* Hard audio reset — wait for next first-frame enable. */
     vlc_audio_ring_reset();

    *pitches = new_pitch;
    *lines   = *height;
    return 1;
}

/* ── Frame access ────────────────────────────────────────────────────────── */

/*
 * vlc_video_get_frame
 *
 * Returns the most recent ready frame from rb[video_read_buf].
 * Only returns frames belonging to the current generation (stale pre-stitch
 * frames are silently skipped).
 *
 * The stitch commit and audio-enable decisions are made in retro_run (vlc_core.c)
 * after this call returns, keeping this function simple and lock-minimal.
 */
bool vlc_video_get_frame(const uint32_t **buf_out,
                         unsigned *w, unsigned *h, unsigned *pitch)
{
    pthread_mutex_lock(&ring_mtx);

    int ds = display_slot[video_read_buf];
    bool ok = (ds >= 0
               && ring[video_read_buf][ds].ready
               && ring[video_read_buf][ds].generation == current_gen);

    if (ok) {
        *buf_out = ring[video_read_buf][ds].buf - 2;
        *w       = ring[video_read_buf][ds].width;
        *h       = ring[video_read_buf][ds].height;
        *pitch   = ring[video_read_buf][ds].pitch;
		
	
    }

    pthread_mutex_unlock(&ring_mtx);	
	
    return ok;
}

bool vlc_video_consume_pending_release(int64_t *frame_time_us)
{
    pthread_mutex_lock(&ring_mtx);
    bool r = pending_release;
    if (r && frame_time_us)
        *frame_time_us = first_frame_time_us;
    pending_release     = false;
    first_frame_time_us = 0;
    pthread_mutex_unlock(&ring_mtx);
    return r;
}

/* ── Setup ───────────────────────────────────────────────────────────────── */

void vlc_video_setup_callbacks(libvlc_media_player_t *mp)
{
    libvlc_video_set_callbacks(mp, lock_cb, unlock_cb, display_cb, NULL);
    libvlc_video_set_format_callbacks(mp, setup_format_cb, NULL);
}