#ifndef VLC_CORE_H
#define VLC_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <vlc/vlc.h>
#include "libretro.h"
#define MAX_PATH 1024
#define MAX_W 3840
#define MAX_H 2160
#define AUDIO_TARGET_RATE 48000
#define SAMPLES_PER_FRAME (AUDIO_TARGET_RATE / 60)  /* 800 at 60 fps */

typedef struct {
    libvlc_instance_t     *libvlc;
    libvlc_media_player_t *mp;

    uint32_t *video_buffer;      /* only used for menu overlay */
    unsigned  video_width;
    unsigned  video_height;
    unsigned  video_pitch;

    bool true_discontinuity_pending;
    double video_fps;
bool initial_load;
    bool is_playing;
    bool paused;
    bool seeking;
    int64_t last_video_frame_time;
    int64_t last_time;
    bool video_frame_seen;
    bool audio_blocked;
bool expect_ad_break_switch;
    char **playlist;
    int    playlist_size;
    int    playlist_index;
    bool   playlist_mode;

    /* ── Dual-buffer stitch flags ──────────────────────────────────────────
     * stitch_switch_pending = true while a stitch is in flight:
     *   old buffer (read_buf) is draining, new buffer (write_buf) is filling.
     *   vlc_stitch_try_commit() checks every frame and commits when drained.
     *
     * stitch_resync_pending = true while waiting for the first frame of the
     *   new generation so that vlc_video_get_frame() can reject stale frames. */
    bool stitch_switch_pending;
    bool stitch_resync_pending;
bool stitch_seek_pending;
    bool pending_play;
    bool play_start_attempt;
    int  play_attempt_frames;
bool suppress_next_stitch_event;
    pthread_mutex_t mutex;
int64_t suppress_stitch_until_us;
    bool video_frame_ready;
    bool exit_menu;
    bool menu_active;
    bool iptv_menu_enabled;
    int  menu_selection;
    int  iptv_fixed_width;
    int  iptv_fixed_height;
bool hard_discontinuity;
    unsigned max_width;
    unsigned max_height;
} vlc_core_ctx;

extern vlc_core_ctx               core;
extern retro_environment_t        environ_cb;
extern retro_video_refresh_t      video_cb;
extern retro_audio_sample_batch_t audio_batch_cb;
extern char current_channel_path[MAX_PATH];  // global storage
/* ── vlc_video.c ─────────────────────────────────────────────────────────── */
void vlc_video_setup_callbacks(libvlc_media_player_t *mp);
bool vlc_video_get_frame(const uint32_t **buf_out, unsigned *w, unsigned *h, unsigned *pitch);
bool vlc_video_consume_pending_release(int64_t *out_time);
void vlc_video_flush_display(void);
void vlc_video_stitch_and_flush(void);  /* redirect video writes to staging buffer  */
void vlc_video_stitch_commit(void);     /* flip video read pointer to staging buffer */

/* ── vlc_audio.c ─────────────────────────────────────────────────────────── */
void vlc_audio_setup_callbacks(libvlc_media_player_t *mp);
void vlc_audio_ring_read(int16_t *dst, size_t pairs);
void vlc_audio_ring_reset(void);        /* hard reset; re-gates output until next enable */
void vlc_audio_enable(void);            /* trim pre-roll + open gate (call on 1st video frame) */
bool vlc_audio_is_output_enabled(void);
size_t vlc_audio_read_buf_fill(void);   /* sample pairs remaining in the active read buffer */
void vlc_stitch_begin(void);            /* redirect audio writes to staging; old buf drains */
void vlc_stitch_commit_audio(void);     /* flip audio read pointer to staging buffer */

/* ── vlc_menu.c ──────────────────────────────────────────────────────────── */
bool vlc_menu_init(const char *m3u_path);
void vlc_menu_handle_input(void);
void vlc_menu_deinit(void);
void vlc_menu_draw(void);

/* ── vlc_core.c ──────────────────────────────────────────────────────────── */
bool switch_to_media(const char *path);
void vlc_stitch_try_commit(void);       /* called every frame; commits both bufs when drained */
void vlc_stitch_cancel(void);

#endif