/* video.c
 *
 * Conservative video output for stitch-safe transitions.
 * Only flips buffers when core explicitly calls vlc_video_stitch_commit().
 */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "vlc_core.h"


#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define RING_SIZE 60
#define VIDEO_TRANSITION_GRACE_US 120000

typedef struct {
    uint32_t *buf;
    unsigned width;
    unsigned height;
    unsigned pitch;
    bool ready;
    unsigned generation;
	int64_t pts;
} ring_frame_t;

/* Dual-buffer layout */
static ring_frame_t ring[2][RING_SIZE];
static int write_slot[2] = {0, 0};
static int read_slot[2]  = {0, 0};
static int video_write_buf = 0;
static int video_read_buf  = 0;

/* Generation per buffer */
static unsigned buffer_gen[2] = {0, 0};
static unsigned current_gen = 0;
static unsigned expected_gen = 0;

static bool waiting_for_real_frame = false;
static unsigned ring_alloc_width = 0;
static unsigned ring_alloc_height = 0;
static unsigned ring_alloc_pitch = 0;
static unsigned ring_width = 0;
static unsigned ring_height = 0;
static unsigned ring_pitch = 0;
static bool pending_release = false;
static int64_t first_frame_time_us = 0;
static pthread_mutex_t ring_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Transition state */
typedef enum {
    VIDEO_STATE_PLAYING = 0,
    VIDEO_STATE_DRAINING
} video_state_t;

static video_state_t video_state = VIDEO_STATE_PLAYING;
static bool video_transition_seen_new = false;
static int64_t video_transition_deadline_us = 0;

/* Time helper */
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

/* Allocation helpers */
static void ring_free_all(void)
{
    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < RING_SIZE; i++) {
            free(ring[b][i].buf);
            ring[b][i].buf = NULL;
            ring[b][i].ready = false;
            ring[b][i].generation = 0;
        }
        write_slot[b] = 0;
        read_slot[b] = 0;
        buffer_gen[b] = 0;
    }

    ring_alloc_width = 0;
    ring_alloc_height = 0;
    ring_alloc_pitch = 0;
}

static bool ring_alloc(unsigned max_w, unsigned max_h)
{
    ring_free_all();
    unsigned pitch = max_w * 4;

    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < RING_SIZE; i++) {
            ring[b][i].buf = (uint32_t *)calloc(1, (size_t)pitch * max_h);
            if (!ring[b][i].buf) {
                fprintf(stderr, "[VLC-VIDEO] ring_alloc OOM slot %d buf %d (%ux%u)\n",
                        i, b, max_w, max_h);
                ring_free_all();
                return false;
            }
            ring[b][i].ready = false;
            ring[b][i].generation = 0;
        }
    }

    ring_alloc_width = max_w;
    ring_alloc_height = max_h;
    ring_alloc_pitch = pitch;

    fprintf(stderr, "[VLC-VIDEO] ring_alloc %ux%u pitch=%u\n",
            max_w, max_h, pitch);
    return true;
}

/* Count ready frames in current read buffer */
size_t vlc_video_read_buf_fill(void)
{
    pthread_mutex_lock(&ring_mtx);
    size_t count = 0;
    int b = video_read_buf;
    unsigned gen = buffer_gen[b];

    for (int i = 0; i < RING_SIZE; i++) {
        if (ring[b][i].ready && ring[b][i].generation == gen)
            count++;
    }

    pthread_mutex_unlock(&ring_mtx);
    return count;
}

/* VLC callbacks */
static void *lock_cb(void *data, void **planes)
{
    (void)data;
    pthread_mutex_lock(&ring_mtx);

    int b = video_write_buf;
    int slot = write_slot[b];

    if (!ring[b][slot].buf) {
        static uint32_t scratch[640 * 360];
        *planes = scratch;
        return NULL;
    }

    ring[b][slot].generation = buffer_gen[b];
    *planes = ring[b][slot].buf;
    return (void *)(intptr_t)slot;
}

static void unlock_cb(void *data, void *id, void *const *planes)
{
    (void)data;
    (void)id;
    (void)planes;
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
  if (!core.frontend_active) {
        pthread_mutex_unlock(&ring_mtx);
        return;
    }

    int b = video_write_buf;
    if (ring[b][slot].generation != buffer_gen[b]) {
        pthread_mutex_unlock(&ring_mtx);
        return;
    }

    if (video_state == VIDEO_STATE_DRAINING && !video_transition_seen_new) {
        
		video_transition_seen_new = true;
        fprintf(stderr, "[VLC-VIDEO] First new video frame (gen %u)\n", buffer_gen[b]);
    }

    if (waiting_for_real_frame) {
        waiting_for_real_frame = false;
        pending_release = true;
		core.stitch_switch_pending = false;
        first_frame_time_us = get_time_us();
        fprintf(stderr, "[VLC-VIDEO] First real frame of gen %u\n", expected_gen);
    }
ring[b][slot].pts = (int64_t)libvlc_media_player_get_time(core.mp) * 1000; 
    ring[b][slot].width  = ring_width;
    ring[b][slot].height = ring_height;
    ring[b][slot].pitch  = ring_pitch;
    ring[b][slot].ready  = true;

    write_slot[b] = (slot + 1) % RING_SIZE;
    pthread_mutex_unlock(&ring_mtx);
}

/* Public stitch helpers */
void vlc_video_flush_display(void)
{
    pthread_mutex_lock(&ring_mtx);

    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < RING_SIZE; i++) {
            ring[b][i].ready = false;
            ring[b][i].generation = 0;
        }
        read_slot[b] = 0;
        write_slot[b] = 0;
        buffer_gen[b] = 0;
    }

    video_read_buf = 0;
    video_write_buf = 0;
    current_gen = 0;
    expected_gen = 0;
    waiting_for_real_frame = false;
    pending_release = false;
    first_frame_time_us = 0;
    video_state = VIDEO_STATE_PLAYING;
    video_transition_seen_new = false;
    video_transition_deadline_us = 0;

    core.last_vbuf = NULL;
    core.last_vw = core.last_vh = core.last_vpitch = 0;

    pthread_mutex_unlock(&ring_mtx);

    fprintf(stderr, "[VLC-VIDEO] Flush display: all buffers cleared\n");
}

void vlc_video_stitch_and_flush(void)
{
    pthread_mutex_lock(&ring_mtx);

    /* Prevent duplicate re-arm while a transition is already active */
    if (video_state == VIDEO_STATE_DRAINING) {
        pthread_mutex_unlock(&ring_mtx);

        /* BUG 3 FIX: core.video_frame_seen must only be written while
         * holding core.mutex — this function is called from the VLC audio
         * thread (via audio_flush) and the main thread reads the flag
         * without any lock, so the unsynchronised write was a data race
         * that could cause a crash during seeking.  Take the proper mutex
         * now that ring_mtx is released. */
        pthread_mutex_lock(&core.mutex);
        core.video_frame_seen = false;
        pthread_mutex_unlock(&core.mutex);

        fprintf(stderr, "[VLC-VIDEO] Stitch already active — ignoring duplicate arm\n");
        return;
    }

    video_state = VIDEO_STATE_DRAINING;
    video_transition_seen_new = false;
    video_transition_deadline_us = get_time_us() + VIDEO_TRANSITION_GRACE_US;

    current_gen++;
    expected_gen = current_gen;
    waiting_for_real_frame = true;

    int staging = 1 - video_read_buf;
    video_write_buf = staging;

    buffer_gen[staging] = current_gen;
    write_slot[staging] = 0;
    read_slot[staging]  = 0;

    /* Only clear staging buffer, NOT the old read buffer */
    for (int i = 0; i < RING_SIZE; i++) {
        ring[staging][i].ready = false;
        ring[staging][i].generation = 0;
    }

    pthread_mutex_unlock(&ring_mtx);

    fprintf(stderr,
            "[VLC-VIDEO] Stitch armed: writes -> buf %d | still reading buf %d (gen %u, deadline %lldus)\n",
            staging, video_read_buf, current_gen, (long long)video_transition_deadline_us);
}

void vlc_video_stitch_commit(void)
{
    pthread_mutex_lock(&ring_mtx);

    if (video_state != VIDEO_STATE_DRAINING) {
        pthread_mutex_unlock(&ring_mtx);
        return;
    }

    int old_read = video_read_buf;
    video_read_buf = video_write_buf;

    /* Now safe to clear old buffer */
    read_slot[old_read]  = 0;
    write_slot[old_read] = 0;
    buffer_gen[old_read] = 0;

    for (int i = 0; i < RING_SIZE; i++) {
        ring[old_read][i].ready = false;
        ring[old_read][i].generation = 0;
    }

    video_state = VIDEO_STATE_PLAYING;
    video_transition_seen_new = false;
    video_transition_deadline_us = 0;
    waiting_for_real_frame = false;
    pending_release = false;
    first_frame_time_us = 0;

    pthread_mutex_unlock(&ring_mtx);

    fprintf(stderr, "[VLC-VIDEO] Stitch commit: reads -> buf %d\n", video_read_buf);
}

/* Old buffer drained check (used by core)
 * During a stitch this now means:
 *   - commit only after the new generation has started and the old buffer is empty, or
 *   - after the grace timeout.
 */
bool vlc_video_old_buffer_drained(void)
{
    pthread_mutex_lock(&ring_mtx);

    // If we aren't even stitching, we are "drained" by default
    if (video_state != VIDEO_STATE_DRAINING) {
        pthread_mutex_unlock(&ring_mtx);
        return true;
    }

    // IPTV Path: We commit as soon as we see the first frame of the new stream.
    // This prevents hanging if the previous stream stopped sending data.
 if (core.iptv_menu_enabled) {
        bool seen_new = video_transition_seen_new;
        pthread_mutex_unlock(&ring_mtx);
        /* IPTV: video is “done” once we’ve seen at least one new frame.
           Audio drain is handled in vlc_stitch_try_commit. */
        return seen_new;
    }


    /* DVD / Normal Path */
    int b = video_read_buf;
    unsigned gen = buffer_gen[b];
    size_t count = 0;

    for (int i = 0; i < RING_SIZE; i++) {
        if (ring[b][i].ready && ring[b][i].generation == gen)
            count++;
    }

    int64_t now_us = get_time_us();
    bool deadline_hit = (now_us >= video_transition_deadline_us);
    
    // Ready if (we've seen new data AND old buffer is empty) OR we hit the timeout
    bool ready = (video_transition_seen_new && count == 0) || deadline_hit;
    
    pthread_mutex_unlock(&ring_mtx);
    return ready;
}
/* Format negotiation */
static unsigned setup_format_cb(void **opaque, char *chroma, unsigned *width, unsigned *height,
                                unsigned *pitches, unsigned *lines)
{
    (void)opaque;
    memcpy(chroma, "RV32", 4);

    unsigned new_pitch = *width * 4;

    /* DVD mid-stream format change guard */
    if (core.isDVD && current_gen != 0) {
        pthread_mutex_lock(&ring_mtx);

        if (*width > ring_alloc_width || *height > ring_alloc_height) {
            if (!ring_alloc(*width, *height)) {
                pthread_mutex_unlock(&ring_mtx);
                return 0;
            }
            if (video_write_buf >= 0 && video_write_buf < 2) {
                buffer_gen[video_write_buf] = current_gen;
            }
            fprintf(stderr, "[VLC-VIDEO] DVD realloc %ux%u -> %ux%u, gen %u preserved\n",
                    ring_alloc_width, ring_alloc_height, *width, *height, current_gen);
        }

        ring_width  = *width;
        ring_height = *height;
        ring_pitch  = new_pitch;

        pthread_mutex_unlock(&ring_mtx);

        *pitches = new_pitch;
        *lines   = *height;
        fprintf(stderr, "[VLC-VIDEO] DVD format cb %ux%u gen %u kept\n",
                *width, *height, current_gen);
        return 1;
    }

    bool needs_realloc = (*width > ring_alloc_width || *height > ring_alloc_height);

if (!needs_realloc) {
        pthread_mutex_lock(&ring_mtx);

        /* If we are already mid-stitch, we only want to clear the writing side.
         * If we are NOT stitching, we clear everything to ensure no stale frames. */
        int start_b = (video_state == VIDEO_STATE_DRAINING) ? video_write_buf : 0;
        int end_b   = (video_state == VIDEO_STATE_DRAINING) ? video_write_buf : 1;

        for (int b = start_b; b <= end_b; b++) {
            for (int i = 0; i < RING_SIZE; i++) {
                ring[b][i].ready = false;
            }
            write_slot[b] = 0;
            read_slot[b]  = 0;
        }

        if (current_gen == 0) {
            current_gen = 1;
            buffer_gen[0] = 1;
            video_write_buf = video_read_buf = 0;
        } else {
            // Only increment generation if we aren't already waiting for a new one
            if (video_state != VIDEO_STATE_DRAINING) {
                current_gen++;
                buffer_gen[video_write_buf] = current_gen;
            }
        }

        ring_width  = *width;
        ring_height = *height;
        ring_pitch  = new_pitch;

        pthread_mutex_unlock(&ring_mtx);
        *pitches = new_pitch;
        *lines   = *height;
        return 1;
    }

    fprintf(stderr, "[VLC-VIDEO] Path B: realloc (%ux%u -> %ux%u).\n",
            ring_alloc_width, ring_alloc_height, *width, *height);

    pthread_mutex_lock(&ring_mtx);
    if (!ring_alloc(*width, *height)) {
        pthread_mutex_unlock(&ring_mtx);
        return 0;
    }

    ring_width  = *width;
    ring_height = *height;
    ring_pitch  = new_pitch;

    current_gen++;
    expected_gen = current_gen;
    waiting_for_real_frame = true;
core.stitch_switch_pending = true;
    video_write_buf = 0;
    video_read_buf  = 0;

    buffer_gen[0] = current_gen;
    buffer_gen[1] = 0;

    write_slot[0] = write_slot[1] = 0;
    read_slot[0]  = read_slot[1]  = 0;

    pthread_mutex_unlock(&ring_mtx);

    *pitches = new_pitch;
    *lines   = *height;
    return 1; 
}

/* Frame access */
bool vlc_video_get_frame(const uint32_t **buf_out, unsigned *w, unsigned *h, unsigned *pitch, int64_t *out_pts, int64_t target_pts)
{
    pthread_mutex_lock(&ring_mtx);

    int b = video_read_buf;
    unsigned gen = buffer_gen[b];
    bool found = false;

    /* Simple "due" search for IPTV/Timed mode */
    if (target_pts > 0) {
        bool retry = true;
        
        /* Check buffer fill level first */
        size_t fill = 0;
        for (int i = 0; i < RING_SIZE; i++) {
            if (ring[b][i].ready && ring[b][i].generation == gen) fill++;
        }

        while (retry) {
            retry = false;
            int oldest_slot = -1;
            for (int i = 0; i < RING_SIZE; i++) {
                int slot = (read_slot[b] + i) % RING_SIZE;
                if (ring[b][slot].ready && ring[b][slot].generation == gen) {
                    oldest_slot = slot;
                    break;
                }
            }

            if (oldest_slot != -1) {
                int64_t pts = ring[b][oldest_slot].pts;
                int64_t diff = pts - target_pts;

                /* TIMEBASE SANITY: If difference is > 5s, references are likely mismatched.
                 * Fallback to sequential to avoid permanent stall. */
                if (llabs(diff) > 5000000) {
                    target_pts = -1; 
                    break;
                }

                /* If the buffer is very full (>45 frames), drop frames rapidly to catch up */
                if (fill > 45 && diff < 0) {
                    ring[b][oldest_slot].ready = false;
                    read_slot[b] = (oldest_slot + 1) % RING_SIZE;
                    fill--;
                    retry = true;
                    continue;
                }

                /* If frame is way too late (>200ms), drop it and try next */
                if (diff < -200000) {
                    ring[b][oldest_slot].ready = false;
                    read_slot[b] = (oldest_slot + 1) % RING_SIZE;
                    fill--;
                    retry = true;
                    continue;
                }

                /* DYNAMIC DUE WINDOW:
                 * If buffer is empty, wait up to 40ms.
                 * If buffer has > 15 frames, reduce wait to 5ms to flush backlog.
                 * If buffer has > 35 frames, play regardless of timing (emergency flush).
                 */
                int64_t due_threshold = 40000;
                if (fill > 15) due_threshold = 5000;
                if (fill > 35) due_threshold = 1000000000; /* 1000 seconds */

                if (diff < due_threshold) {
                    *buf_out = ring[b][oldest_slot].buf;
                    *w       = ring[b][oldest_slot].width;
                    *h       = ring[b][oldest_slot].height;
                    *pitch   = ring[b][oldest_slot].pitch;
                    if (out_pts) *out_pts = ring[b][oldest_slot].pts;
                    ring[b][oldest_slot].ready = false;
                    read_slot[b] = (oldest_slot + 1) % RING_SIZE;
                    found = true;
                }
            }
        }
    }

    if (target_pts <= 0) {
        /* No timing - fallback to sequential */
        for (int step = 0; step < RING_SIZE; step++) {
            int slot = (read_slot[b] + step) % RING_SIZE;
            if (ring[b][slot].ready && ring[b][slot].generation == gen) {
                *buf_out = ring[b][slot].buf;
                *w       = ring[b][slot].width;
                *h       = ring[b][slot].height;
                *pitch   = ring[b][slot].pitch;
                if (out_pts) *out_pts = ring[b][slot].pts;
                ring[b][slot].ready = false;
                read_slot[b] = (slot + 1) % RING_SIZE;
                found = true;
                break;
            }
        }
    }

    /* Do NOT flip here — wait for core to call commit */
    pthread_mutex_unlock(&ring_mtx);
    return found;
}

bool vlc_video_consume_pending_release(int64_t *frame_time_us)
{
    pthread_mutex_lock(&ring_mtx);

    bool r = pending_release;
    if (r && frame_time_us)
        *frame_time_us = first_frame_time_us;

    pending_release = false;
    first_frame_time_us = 0;

    pthread_mutex_unlock(&ring_mtx);
    return r;
}

/* Setup */
void vlc_video_setup_callbacks(libvlc_media_player_t *mp)
{
    libvlc_video_set_callbacks(mp, lock_cb, unlock_cb, display_cb, NULL);
    libvlc_video_set_format_callbacks(mp, setup_format_cb, NULL);
}