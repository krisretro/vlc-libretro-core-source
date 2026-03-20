#ifndef VLC_CORE_H
#define VLC_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <vlc/vlc.h>
#include "libretro.h"

#define MAX_W 3840
#define MAX_H 2160
#define AUDIO_TARGET_RATE 48000
#define SAMPLES_PER_FRAME (AUDIO_TARGET_RATE / 60)  /* 800 at 60 fps */

typedef struct {
    libvlc_instance_t    *libvlc;
    libvlc_media_player_t *mp;

    uint32_t *video_buffer;      /* only used for menu overlay */
    unsigned  video_width;
    unsigned  video_height;
    unsigned  video_pitch;

    double video_fps;

    bool is_playing;
    bool paused;
    bool seeking;

    char **playlist;
    int    playlist_size;
    int    playlist_index;
    bool   playlist_mode;
bool stitch_resync_pending;
    bool pending_play;
    bool play_start_attempt;
    int  play_attempt_frames;

    pthread_mutex_t mutex;

    bool video_frame_ready;

    bool menu_active;
    bool iptv_menu_enabled;
    int  menu_selection;
    int  iptv_fixed_width;
    int  iptv_fixed_height;

    unsigned max_width;
    unsigned max_height;
} vlc_core_ctx;

extern vlc_core_ctx               core;
extern retro_environment_t        environ_cb;
extern retro_video_refresh_t      video_cb;
extern retro_audio_sample_batch_t audio_batch_cb;

/* vlc_video.c */
void vlc_video_setup_callbacks(libvlc_media_player_t *mp);
bool vlc_video_get_frame(const uint32_t **buf_out, unsigned *w, unsigned *h, unsigned *pitch);
bool vlc_video_consume_pending_release(int64_t *out_time);
void vlc_video_flush_display(void);

/* vlc_audio.c */
void vlc_audio_setup_callbacks(libvlc_media_player_t *mp);
void vlc_audio_ring_read(int16_t *dst, size_t pairs);
void vlc_audio_ring_reset(void);
void vlc_audio_sync_and_enable(int64_t elapsed_us);
bool vlc_audio_is_output_enabled(void);
int64_t vlc_audio_get_reset_time_us(void);

/* vlc_menu.c */
bool vlc_menu_init(const char *m3u_path);
void vlc_menu_handle_input(void);
void vlc_menu_deinit(void);
void vlc_menu_draw(void);

/* vlc_core.c */
bool switch_to_media(const char *path);

#endif