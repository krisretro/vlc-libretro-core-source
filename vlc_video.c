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
    unsigned width;
    unsigned height;
    unsigned pitch;
    bool ready;
    unsigned generation;
} ring_frame_t;

/* Dual-buffer layout */
static ring_frame_t ring[2][RING_SIZE];
static int write_slot[2] = {0, 0};
static int display_slot[2] = {-1, -1};
static int video_write_buf = 0;
static int video_read_buf = 0;
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

/* ── Time helper ─────────────────────────────────────────────────────────── */
static int64_t get_time_us(void) {
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
static void ring_free_all(void) {
    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < RING_SIZE; i++) {
            free(ring[b][i].buf);
            ring[b][i].buf = NULL;
            ring[b][i].ready = false;
        }
        write_slot[b] = 0;
        display_slot[b] = -1;
    }
}

static bool ring_alloc(unsigned max_w, unsigned max_h) {
    ring_free_all();
    unsigned pitch = max_w * 4;

    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < RING_SIZE; i++) {
            ring[b][i].buf = (uint32_t *)calloc(1, (size_t)pitch * max_h);
            if (!ring[b][i].buf) {
                fprintf(stderr, "[VLC] ring_alloc OOM slot %d buf %d (%ux%u)\n", i, b, max_w, max_h);
                ring_free_all();
                return false;
            }
            ring[b][i].ready = false;
        }
    }

    ring_alloc_width = max_w;
    ring_alloc_height = max_h;
    ring_alloc_pitch = pitch;
    return true;
}

/* ── VLC video callbacks ─────────────────────────────────────────────────── */
static void *lock_cb(void *data, void **planes) {
    (void)data;
    pthread_mutex_lock(&ring_mtx);

    if (!ring[video_write_buf][write_slot[video_write_buf]].buf) {
        static uint32_t scratch[640 * 360];
        *planes = scratch;
        return NULL;
    }

    ring[video_write_buf][write_slot[video_write_buf]].generation = current_gen;
    *planes = ring[video_write_buf][write_slot[video_write_buf]].buf;
    return (void *)(intptr_t)write_slot[video_write_buf];
}

static void unlock_cb(void *data, void *id, void *const *planes) {
    (void)data;
    (void)id;
    (void)planes;
    pthread_mutex_unlock(&ring_mtx);
}

static void display_cb(void *data, void *id) {
    (void)data;
    pthread_mutex_lock(&ring_mtx);

    int slot = (int)(intptr_t)id;
    if (slot < 0 || slot >= RING_SIZE) {
        pthread_mutex_unlock(&ring_mtx);
        return;
    }

    if (ring[video_write_buf][slot].generation != current_gen) {
        pthread_mutex_unlock(&ring_mtx);
        return;
    }

    if (waiting_for_real_frame) {
        waiting_for_real_frame = false;
                  pending_release        = true;
// vlc_audio_ring_reset(); 
//		vlc_audio_ring_reset();
core.audio_start_time_us = get_time_us();
core.audio_wait_for_sync = true;
        first_frame_time_us = get_time_us();
        fprintf(stderr, "[VLC] First real frame of gen %u\n", expected_gen);
    }

    ring[video_write_buf][slot].width = ring_width;
    ring[video_write_buf][slot].height = ring_height;
    ring[video_write_buf][slot].pitch = ring_pitch;
    ring[video_write_buf][slot].ready = true;
    display_slot[video_write_buf] = slot;

    pthread_mutex_unlock(&ring_mtx);
}

/* ── Public stitch helpers ───────────────────────────────────────────────── */
void vlc_video_flush_display(void) {
    pthread_mutex_lock(&ring_mtx);
    display_slot[0] = -1;
    display_slot[1] = -1;
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < RING_SIZE; i++)
            ring[b][i].ready = false;
    pthread_mutex_unlock(&ring_mtx);
    fprintf(stderr, "[VLC] Video ring flushed — stale frames discarded.\n");
}

void vlc_video_stitch_and_flush(void) {
    pthread_mutex_lock(&ring_mtx);
    current_gen++;
    expected_gen = current_gen;
    waiting_for_real_frame = true;

    int staging = 1 - video_read_buf;
    video_write_buf = staging;
    write_slot[staging] = 0;
    display_slot[staging] = -1;

    for (int i = 0; i < RING_SIZE; i++)
        ring[staging][i].ready = false;

    pthread_mutex_unlock(&ring_mtx);
    fprintf(stderr, "[VLC] Video stitch armed: writes -> buf %d | reading buf %d (gen %u)\n",
            staging, video_read_buf, current_gen);
}

void vlc_video_stitch_commit(void) {
    pthread_mutex_lock(&ring_mtx);

    int old_read = video_read_buf;
    video_read_buf = video_write_buf;

    display_slot[old_read] = -1;
    write_slot[old_read] = 0;
    for (int i = 0; i < RING_SIZE; i++)
        ring[old_read][i].ready = false;

    pthread_mutex_unlock(&ring_mtx);
    fprintf(stderr, "[VLC] Video stitch commit: reads -> buf %d\n", video_read_buf);
}

/* ── Format negotiation ──────────────────────────────────────────────────── */
static unsigned setup_format_cb(void **opaque, char *chroma, unsigned *width, unsigned *height,
                                unsigned *pitches, unsigned *lines) {
    (void)opaque;
    memcpy(chroma, "RV32", 4);
    unsigned new_pitch = *width * 4;

    bool needs_realloc = (*width > ring_alloc_width || *height > ring_alloc_height);
    if (!needs_realloc) {
        pthread_mutex_lock(&ring_mtx);
        if (current_gen == 0) {
            current_gen = 1;
            expected_gen = 1;
            waiting_for_real_frame = true;
            video_write_buf = 0;
            video_read_buf = 0;
            display_slot[0] = display_slot[1] = -1;
            pthread_mutex_unlock(&ring_mtx);
            fprintf(stderr, "[VLC] Path A initial load (gen 1).\n");
        } else {
            current_gen++;
            expected_gen = current_gen;
            waiting_for_real_frame = true;
            pthread_mutex_unlock(&ring_mtx);
            fprintf(stderr, "[VLC] Path A seamless format change (gen %u).\n", current_gen);
        }

        pthread_mutex_lock(&ring_mtx);
        ring_width = *width;
        ring_height = *height;
        ring_pitch = new_pitch;
        pthread_mutex_unlock(&ring_mtx);

        *pitches = new_pitch;
        *lines = *height;
        return 1;
    }

    /* Realloc path */
    fprintf(stderr, "[VLC] Path B: realloc (%ux%u -> %ux%u).\n",
            ring_alloc_width, ring_alloc_height, *width, *height);

    pthread_mutex_lock(&ring_mtx);
    if (!ring_alloc(*width, *height)) {
        pthread_mutex_unlock(&ring_mtx);
        return 0;
    }
    ring_width = *width;
    ring_height = *height;
    ring_pitch = new_pitch;
    current_gen++;
    expected_gen = current_gen;
    waiting_for_real_frame = true;
    video_write_buf = 0;
    video_read_buf = 0;
    display_slot[0] = display_slot[1] = -1;
    pthread_mutex_unlock(&ring_mtx);

    *pitches = new_pitch;
    *lines = *height;
    return 1;
}

/* ── Frame access ────────────────────────────────────────────────────────── */
bool vlc_video_get_frame(const uint32_t **buf_out, unsigned *w, unsigned *h, unsigned *pitch) {
    pthread_mutex_lock(&ring_mtx);
    int ds = display_slot[video_read_buf];
    bool ok = (ds >= 0 && ring[video_read_buf][ds].ready && ring[video_read_buf][ds].generation == current_gen);

    if (ok) {
        *buf_out = ring[video_read_buf][ds].buf;
        *w = ring[video_read_buf][ds].width;
        *h = ring[video_read_buf][ds].height;
        *pitch = ring[video_read_buf][ds].pitch;
    }

    pthread_mutex_unlock(&ring_mtx);
    return ok;
}

bool vlc_video_consume_pending_release(int64_t *frame_time_us) {
    pthread_mutex_lock(&ring_mtx);
    bool r = pending_release;
    if (r && frame_time_us) *frame_time_us = first_frame_time_us;
    pending_release = false;
    first_frame_time_us = 0;
    pthread_mutex_unlock(&ring_mtx);
    return r;
}

/* ── Setup ───────────────────────────────────────────────────────────────── */
void vlc_video_setup_callbacks(libvlc_media_player_t *mp) {
    libvlc_video_set_callbacks(mp, lock_cb, unlock_cb, display_cb, NULL);
    libvlc_video_set_format_callbacks(mp, setup_format_cb, NULL);
}