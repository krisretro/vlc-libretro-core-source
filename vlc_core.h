#ifndef VLC_CORE_H
#define VLC_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <vlc/vlc.h>
#include "libretro.h"

#define MAX_W 1920
#define MAX_H 1080
#define AUDIO_BUFFER_SIZE (4 * 1024 * 1024)
#define AUDIO_TARGET_RATE 48000

typedef struct {
    libvlc_instance_t *libvlc;
    libvlc_media_player_t *mp;
    
    uint32_t *video_buffer;
    unsigned video_width;
    unsigned video_height;
    unsigned video_pitch;

    int16_t audio_ring[AUDIO_BUFFER_SIZE];
    size_t audio_read_pos;
    size_t audio_write_pos;
    
    int64_t audio_sent_frames;
    int64_t sync_offset;
    bool sync_offset_initialized;

    double video_fps;
    int64_t last_video_pts;
    int64_t last_audio_pts;
    int64_t frame_time;
    
    bool is_playing;
    bool seeking;
    int64_t seek_target;
    int64_t last_vlc_time;
    bool paused;                      // user pause state
    
    char **playlist;
    int playlist_size;
    int playlist_index;
    bool playlist_mode;
    bool transitioning;
    uint32_t transition_timeout_frames;
    pthread_mutex_t mutex;
    int64_t last_video_presented_ms;
    double sample_accum_frac;          // fractional accumulator for audio output
    bool pending_start;                 // delay initial playback until first retro_run
  bool spu_initialized;   // whether subpicture track has been enabled
      int  audio_mute_frames;   // countdown of frames to force silence after load/seek
	  int   audio_desync_ms;            // NEW: persistent user value
	  unsigned max_width;      // NEW
   unsigned max_height;     // NEW
} vlc_core_ctx;

extern vlc_core_ctx core;

void vlc_video_setup_callbacks(libvlc_media_player_t *mp);
void vlc_audio_setup_callbacks(libvlc_media_player_t *mp);
void vlc_audio_flush(void);

#endif