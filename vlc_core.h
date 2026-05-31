#ifndef VLC_CORE_H
#define VLC_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <vlc/vlc.h>
#include "libretro.h"

#define MAX_PATH         1024
#define MAX_W            3840
#define MAX_H            2160
#define AUDIO_TARGET_RATE 48000
#define SAMPLES_PER_FRAME (AUDIO_TARGET_RATE / 60)  /* 800 samples at 60 fps */

typedef struct {
    libvlc_instance_t     *libvlc;
    libvlc_media_player_t *mp;
bool yt_resolving;       /* true while a YouTube URL is being resolved */
int  yt_resolving_index; /* playlist index being resolved              */
    /* Menu overlay buffer — only allocated while menu is active. */
    uint32_t *video_buffer;
    unsigned  video_width;
    unsigned  video_height;
    unsigned  video_pitch;
bool frontend_active; 
bool resuming_from_pause;
int  resume_hold_frames;
    double   video_fps;
    bool     initial_load;
    bool     is_playing;
    bool     paused;
    bool     seeking;
    bool     video_frame_seen;
    bool     audio_blocked;
    bool     is_buffering;          /* true while VLC reports a buffering event  */
    bool     true_discontinuity_pending;
    bool     expect_ad_break_switch;
    bool     hard_discontinuity;
    int64_t  last_video_frame_time;
    int64_t  last_time;
    int64_t  suppress_stitch_until_us;
    int64_t  current_frame_pts;
    int64_t  audio_start_time_us;

    /* Last displayed frame (for IPTV timing) */
    const uint32_t *last_vbuf;
    unsigned last_vw, last_vh, last_vpitch;

    char **playlist;
    int    playlist_size;
    int    playlist_index;
    bool   playlist_mode;
    bool   isDVD;

    /* Dual-buffer stitch state */
    bool stitch_switch_pending;
    bool stitch_seek_pending;
    bool suppress_next_stitch_event;
    bool stitch_resync_pending;

    /* Playback startup */
    bool pending_play;
    bool play_start_attempt;
    int  play_attempt_frames;

    /* Audio/video sync gate */
    bool audio_wait_for_sync;

    /* IPTV / menu state */
    bool exit_menu;
    bool menu_active;
    bool iptv_menu_enabled;
    int  menu_selection;
    int  iptv_fixed_width;
    int  iptv_fixed_height;
    unsigned max_width;
    unsigned max_height;

    pthread_mutex_t mutex;
} vlc_core_ctx;

extern vlc_core_ctx               core;
extern retro_environment_t        environ_cb;
extern retro_video_refresh_t      video_cb;
extern retro_audio_sample_batch_t audio_batch_cb;
extern char current_channel_path[MAX_PATH];

/* ── vlc_video.c ─────────────────────────────────────────────────────────── */
void vlc_video_setup_callbacks(libvlc_media_player_t *mp);
bool vlc_video_get_frame(const uint32_t **buf_out, unsigned *w, unsigned *h,
                         unsigned *pitch, int64_t *out_pts, int64_t target_pts);
bool vlc_video_consume_pending_release(int64_t *out_time);
void vlc_video_flush_display(void);
void vlc_video_stitch_and_flush(void);
void vlc_video_stitch_commit(void);
size_t vlc_video_read_buf_fill(void);
bool vlc_video_old_buffer_drained(void);

/* ── vlc_audio.c ─────────────────────────────────────────────────────────── */
void vlc_audio_setup_callbacks(libvlc_media_player_t *mp);
void vlc_audio_ring_read(int16_t *dst, size_t pairs);
void vlc_audio_ring_reset(void);
void vlc_audio_enable(void);
void vlc_audio_disable(void);
bool vlc_audio_is_output_enabled(void);
size_t vlc_audio_read_buf_fill(void);
void vlc_stitch_begin(void);
void vlc_stitch_commit_audio(void);
int64_t vlc_audio_get_playback_pts(void);   /* Added for IPTV sync */

/* ── vlc_menu.c ──────────────────────────────────────────────────────────── */
bool vlc_menu_init(const char *m3u_path);
void vlc_menu_handle_input(void);
void vlc_menu_deinit(void);
void vlc_menu_draw(void);

/* ── vlc_core.c ──────────────────────────────────────────────────────────── */
bool switch_to_media(const char *path);
void vlc_stitch_try_commit(void);
void vlc_stitch_cancel(void);

#endif /* VLC_CORE_H */