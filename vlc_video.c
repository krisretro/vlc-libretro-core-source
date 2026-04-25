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

#define RING_SIZE 6
#define VIDEO_TRANSITION_GRACE_US 120000

typedef struct {
    uint32_t *buf;
    unsigned width;
    unsigned height;
    unsigned pitch;
    bool ready;
    unsigned generation;
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
        first_frame_time_us = get_time_us();
        fprintf(stderr, "[VLC-VIDEO] First real frame of gen %u\n", expected_gen);
    }

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

    pthread_mutex_unlock(&ring_mtx);

    fprintf(stderr, "[VLC-VIDEO] Flush display: all buffers cleared\n");
}

void vlc_video_stitch_and_flush(void)
{
    pthread_mutex_lock(&ring_mtx);

    /* Prevent duplicate re-arm while a transition is already active */
    if (video_state == VIDEO_STATE_DRAINING) {
        pthread_mutex_unlock(&ring_mtx);
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

    int b = video_read_buf;
    unsigned gen = buffer_gen[b];
    size_t count = 0;

    for (int i = 0; i < RING_SIZE; i++) {
        if (ring[b][i].ready && ring[b][i].generation == gen)
            count++;
    }

    bool drained = (count == 0);

    if (video_state != VIDEO_STATE_DRAINING) {
        pthread_mutex_unlock(&ring_mtx);
        /* No video stitch is armed.  Report ready unconditionally so the
         * core's vlc_stitch_try_commit() can fire on the audio drain
         * condition alone (DVD path).  The caller must not invoke
         * vlc_video_stitch_commit() when we return true here — it is a
         * no-op because video_state is still PLAYING. */
        return true;
    }

    int64_t now_us = get_time_us();
    bool deadline_hit = (now_us >= video_transition_deadline_us);

    bool ready = (video_transition_seen_new && drained) || deadline_hit;

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

        if (current_gen == 0) {
            current_gen = 1;
            expected_gen = 1;
            waiting_for_real_frame = true;

            video_write_buf = 0;
            video_read_buf  = 0;

            buffer_gen[0] = 1;
            buffer_gen[1] = 0;

            write_slot[0] = write_slot[1] = 0;
            read_slot[0]  = read_slot[1]  = 0;

            fprintf(stderr, "[VLC-VIDEO] Path A initial load (gen 1).\n");
        } else {
            current_gen++;
            expected_gen = current_gen;
            waiting_for_real_frame = true;

            buffer_gen[video_write_buf] = current_gen;

            fprintf(stderr, "[VLC-VIDEO] Path A seamless format change (gen %u).\n", current_gen);
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
bool vlc_video_get_frame(const uint32_t **buf_out, unsigned *w, unsigned *h, unsigned *pitch)
{
    pthread_mutex_lock(&ring_mtx);

    int b = video_read_buf;
    unsigned gen = buffer_gen[b];
    bool found = false;

    for (int step = 0; step < RING_SIZE; step++) {
        int slot = (read_slot[b] + step) % RING_SIZE;
        if (ring[b][slot].ready && ring[b][slot].generation == gen) {
            *buf_out = ring[b][slot].buf;
            *w       = ring[b][slot].width;
            *h       = ring[b][slot].height;
            *pitch   = ring[b][slot].pitch;

            ring[b][slot].ready = false;
            read_slot[b] = (slot + 1) % RING_SIZE;

            found = true;
            break;
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