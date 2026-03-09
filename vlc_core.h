#ifndef VLC_CORE_H
#define VLC_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <vlc/vlc.h>          // ← MUST be here before any libvlc types
#include "libretro.h"

#define MAX_W 1920
#define MAX_H 1080
#define AUDIO_TARGET_RATE 48000

typedef struct {
    libvlc_instance_t *libvlc;
    libvlc_media_player_t *mp;
    
    uint32_t *video_buffer;
    unsigned video_width;
    unsigned video_height;
    unsigned video_pitch;

    double video_fps;
    
    bool is_playing;
    bool paused;
    bool seeking;               // kept — used by L1/R1 seek

    char **playlist;
    int playlist_size;
    int playlist_index;
    bool playlist_mode;
bool pending_play;
bool play_start_attempt;     
int  play_attempt_frames;    
    pthread_mutex_t mutex;
    bool transitioning;
	bool video_frame_ready;
    unsigned max_width;
    unsigned max_height;
	    bool menu_active;        
		bool iptv_menu_enabled;
    int  menu_selection;        
} vlc_core_ctx;

extern vlc_core_ctx core;
extern retro_environment_t environ_cb;
extern retro_video_refresh_t video_cb;
extern retro_audio_sample_batch_t audio_batch_cb;
bool vlc_menu_init(const char *m3u_path);
void vlc_menu_handle_input(void);
void vlc_menu_deinit(void);
void vlc_video_setup_callbacks(libvlc_media_player_t *mp);
void vlc_audio_setup_callbacks(libvlc_media_player_t *mp);
bool switch_to_media(const char *path);
void vlc_menu_draw(void);


#endif