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

#define RING_SIZE 3
static unsigned current_gen = 0;
typedef struct {
    uint32_t *buf;          /* allocated at max_pitch * max_height */
    unsigned  width;        /* current stream width  (<= max) */
    unsigned  height;       /* current stream height (<= max) */
    unsigned  pitch;        /* current stream pitch = width*4 */
    bool      ready;
	 unsigned  generation; 
} ring_frame_t;

static ring_frame_t    ring[RING_SIZE];
static int             write_slot      = 0;
static int             display_slot    = -1;
static pthread_mutex_t ring_mtx        = PTHREAD_MUTEX_INITIALIZER;

static unsigned ring_alloc_width  = 0;
static unsigned ring_alloc_height = 0;
static unsigned ring_alloc_pitch  = 0;

static unsigned ring_width  = 0;
static unsigned ring_height = 0;
static unsigned ring_pitch  = 0;

static bool     post_stitch     = false;
static bool     pending_release = false;
static int64_t  first_frame_time_us = 0;

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

static void ring_free_all(void)
{
    for (int i = 0; i < RING_SIZE; i++) {
        free(ring[i].buf);
        ring[i].buf    = NULL;
        ring[i].ready  = false;
    }
    write_slot   = 0;
    display_slot = -1;
}

static bool ring_alloc(unsigned max_w, unsigned max_h)
{
    ring_free_all();
    unsigned pitch = max_w * 4;
    for (int i = 0; i < RING_SIZE; i++) {
        ring[i].buf = (uint32_t *)calloc(1, (size_t)pitch * max_h);
        if (!ring[i].buf) {
            fprintf(stderr, "[VLC] ring_alloc OOM slot %d (%ux%u)\n", i, max_w, max_h);
            ring_free_all();
            return false;
        }
        ring[i].ready = false;
    }
    ring_alloc_width  = max_w;
    ring_alloc_height = max_h;
    ring_alloc_pitch  = pitch;
    fprintf(stderr, "[VLC] ring_alloc: %d slots @ %ux%u (max)\n", RING_SIZE, max_w, max_h);
    return true;
}

static void *lock_cb(void *data, void **planes)
{
    pthread_mutex_lock(&ring_mtx);
    if (!ring[write_slot].buf) {
        static uint32_t scratch[640 * 360];
        *planes = scratch;
        return NULL;
    }
	 ring[write_slot].generation = current_gen; 
    *planes = ring[write_slot].buf;
    return (void *)(intptr_t)write_slot;
}

static void unlock_cb(void *data, void *id, void *const *planes)
{
    pthread_mutex_unlock(&ring_mtx);
}

/* Call this at a stitch point to discard any pre-stitch frames that VLC's
 * decode pipeline may have already queued.  Sets display_slot = -1 so
 * retro_run won't show stale frames while waiting for the first new frame. */
void vlc_video_flush_display(void)
{
    pthread_mutex_lock(&ring_mtx);
    display_slot = -1;
    for (int i = 0; i < RING_SIZE; i++)
        ring[i].ready = false;
    pthread_mutex_unlock(&ring_mtx);
    fprintf(stderr, "[VLC] Video ring flushed — stale pre-stitch frames discarded\n");
}

static void display_cb(void *data, void *id)
 
{
    pthread_mutex_lock(&ring_mtx);

    int slot = write_slot;
    // Only accept this frame if it belongs to the current generation
    if (ring[slot].generation == current_gen) {
        ring[slot].width  = ring_width;
        ring[slot].height = ring_height;
        ring[slot].pitch  = ring_pitch;
        ring[slot].ready  = true;

        display_slot = slot;

        if (post_stitch) {
          
            post_stitch = false;
   
        }
    }     pthread_mutex_unlock(&ring_mtx);

}


static unsigned setup_format_cb(void **opaque, char *chroma,
                                unsigned *width, unsigned *height,
                                unsigned *pitches, unsigned *lines)
{
   /* Always use RV32 for libretro compatibility */
   memcpy(chroma, "RV32", 4);

   /* Handle IPTV fixed resolution constraints if enabled */
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

   /* Constrain resolution to max limits while maintaining aspect ratio */
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

   /* Check for identical format to determine if we need a full reallocation */
   bool needs_realloc = true;
   pthread_mutex_lock(&core.mutex);
   if (core.video_width == *width &&
       core.video_height == *height &&
       core.video_pitch == new_pitch) {
       needs_realloc = false;
   }
   pthread_mutex_unlock(&core.mutex);

   if (!needs_realloc) {
       /* PATH A: Seamless stitch (Same resolution) */
       pthread_mutex_lock(&ring_mtx);
       
     
       
       /* Arm the resync logic: audio will output silence until the first video frame arrives */
       core.stitch_resync_pending = true;
       current_gen++;
       post_stitch = true;
       
       /* Manually flush video ring to avoid deadlocking on vlc_video_flush_display */
       display_slot = -1;
       for (int i = 0; i < RING_SIZE; i++) {
           ring[i].ready = false;
       }
         /* Reset audio to create the required sync gap */
       vlc_audio_ring_reset();
       pthread_mutex_unlock(&ring_mtx);

       fprintf(stderr, "[VLC] setup_format: same size stitch - audio resync and video flush, gen=%u\n", current_gen);
       
       *pitches = new_pitch;
       *lines   = *height;
       return 1;
   }

   /* PATH B: REAL format change detected (New resolution) */
   fprintf(stderr, "[VLC] REAL format change detected - doing full init + audio re-lock\n");

   if (!ring_alloc(max_w, max_h))
       return 0;

   pthread_mutex_lock(&core.mutex);
   pthread_mutex_lock(&ring_mtx);

   ring_width  = *width;
   ring_height = *height;
   ring_pitch  = new_pitch;
   post_stitch = true;
   
   /* arm resync and increment generation to ignore stale frames in lock_cb/display_cb */
   core.stitch_resync_pending = true;
   current_gen++;

   /* Reset audio and flush video ring */
   vlc_audio_ring_reset();
   display_slot = -1;
   for (int i = 0; i < RING_SIZE; i++) {
       ring[i].ready = false;
   }
 
   core.video_width  = *width;
   core.video_height = *height;
   core.video_pitch  = new_pitch;

   if (!core.iptv_menu_enabled)
       core.pending_play = true;

   /* Update frontend geometry for the new resolution */
   struct retro_game_geometry geo = {
       .base_width   = *width,
       .base_height  = *height,
       .max_width    = max_w,
       .max_height   = max_h,
       .aspect_ratio = (float)*width / (float)*height
   };
   environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geo);

   fprintf(stderr, "[VLC] setup_format: alloc %ux%u for %ux%u, gen=%u\n",
           max_w, max_h, *width, *height, current_gen);

   *pitches = new_pitch;
   *lines   = *height;

   pthread_mutex_unlock(&ring_mtx);
   pthread_mutex_unlock(&core.mutex);
   
   return 1;
}


bool vlc_video_get_frame(const uint32_t **buf_out, unsigned *w, unsigned *h, unsigned *pitch)
{
    pthread_mutex_lock(&ring_mtx);
    bool ok = (display_slot >= 0 && ring[display_slot].ready);
    if (ok) {
        *buf_out = ring[display_slot].buf;
        *w       = ring[display_slot].width;
        *h       = ring[display_slot].height;
        *pitch   = ring[display_slot].pitch;

        // The audio "gate" only opens when the first real frame 
        // is actually pulled by the frontend.
        if (core.stitch_resync_pending) {
            core.stitch_resync_pending = false; 
        }
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
    pending_release = false;
    first_frame_time_us = 0;
    pthread_mutex_unlock(&ring_mtx);
    return r;
}

void vlc_video_setup_callbacks(libvlc_media_player_t *mp)
{
    libvlc_video_set_callbacks(mp, lock_cb, unlock_cb, display_cb, NULL);
    libvlc_video_set_format_callbacks(mp, setup_format_cb, NULL);
}