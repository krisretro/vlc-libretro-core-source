#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>
#include "vlc_core.h"
#include "libretro.h"
#include <vlc/libvlc_version.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
// Forward declaration for libvlc_video_set_mouse_position (needed for older headers)
#if LIBVLC_VERSION_INT >= LIBVLC_VERSION(3,0,0,0)
LIBVLC_API void libvlc_video_set_mouse_position(libvlc_media_player_t *p_mi, int num, int denom, int x, int y);
#endif

#ifdef _WIN32
#include <windows.h>
typedef void (*pfn_libvlc_video_set_mouse_position)(libvlc_media_player_t*, int, int, int, int);
static pfn_libvlc_video_set_mouse_position dyn_libvlc_video_set_mouse_position = NULL;
#endif

#ifdef _WIN32
#define strcasestr _stristr
static const char* _stristr(const char* haystack, const char* needle) {
    if (!*needle) return haystack;
    for (; *haystack; ++haystack) {
        if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
            const char *h = haystack, *n = needle;
            while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
                ++h; ++n;
            }
            if (!*n) return haystack;
        }
    }
    return NULL;
}
#endif

vlc_core_ctx core = {0};
retro_video_refresh_t     video_cb = NULL;
retro_audio_sample_batch_t audio_batch_cb = NULL;
retro_environment_t       environ_cb = NULL;
retro_input_poll_t        input_poll_cb = NULL;
retro_input_state_t       input_state_cb = NULL;

static bool prev_l1 = false, prev_r1 = false;
static bool prev_l2 = false, prev_r2 = false;

static void log_cb(void *data, int level, const libvlc_log_t *ctx, const char *fmt, va_list args) {
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, args);
    fprintf(stderr, "[VLC-LOG] %s\n", msg);
}
static bool get_option_enabled(const char *key) {
    struct retro_variable var = { .key = key };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        return strcmp(var.value, "enabled") == 0;
    return false;
}
// Helper: update FPS from current media
static void update_fps_from_current_media(void) {
    libvlc_media_t *media = libvlc_media_player_get_media(core.mp);
    if (!media) return;

    libvlc_media_parse_with_options(media, libvlc_media_parse_local, 180000);

    libvlc_media_track_t **tracks;
    int num_tracks = libvlc_media_tracks_get(media, &tracks);

    for (int i = 0; i < num_tracks; i++) {
        if (tracks[i]->i_type == libvlc_track_video) {
            if (tracks[i]->video->i_frame_rate_num > 0 && tracks[i]->video->i_frame_rate_den > 0) {
                core.video_fps = (double)tracks[i]->video->i_frame_rate_num /
                                 (double)tracks[i]->video->i_frame_rate_den;
                fprintf(stderr, "[VLC] Detected video FPS: %.3f (%d/%d)\n",
                        core.video_fps,
                        tracks[i]->video->i_frame_rate_num,
                        tracks[i]->video->i_frame_rate_den);
            }
            break;
        }
    }

    libvlc_media_tracks_release(tracks, num_tracks);
    libvlc_media_release(media);
}

static bool load_media_file(const char *path) {
    fprintf(stderr, "[VLC] Loading media: %s\n", path);
vlc_audio_flush();                     // ← stronger flush
    core.audio_sent_frames = 0;
    core.sample_accum_frac = 0.0;
    core.sync_offset_initialized = false;
    core.last_audio_pts = 0;
    core.audio_mute_frames = 8;      
   core.audio_desync_ms = 0;             // will be read from option below
	core.paused = false;
    core.spu_initialized = false;
    if (core.mp) {
        libvlc_media_player_stop(core.mp);
        libvlc_media_player_release(core.mp);
        core.mp = NULL;
    }

    core.mp = libvlc_media_player_new(core.libvlc);
    if (!core.mp) {
        fprintf(stderr, "[VLC] Failed to create media player\n");
        return false;
    }
    vlc_video_setup_callbacks(core.mp);
    vlc_audio_setup_callbacks(core.mp);

    libvlc_media_t *m = NULL;
    bool is_online = strstr(path, "://") != NULL;
    bool is_dvd = false;
    bool is_bluray = false;

    // --- Auto-detection (DVD/ISO) ---
    const char *ext = strrchr(path, '.');
    if (ext && (strcasecmp(ext, ".iso") == 0 || strcasecmp(ext, ".ISO") == 0)) {
        is_dvd = true;
    }
#ifdef _WIN32
    if (strlen(path) >= 2 && path[1] == ':') {
        const char *rest = path + 2;
        if (*rest == '\0' || ((*rest == '\\' || *rest == '/') && *(rest+1) == '\0')) {
            is_dvd = true;
        }
    }
#endif

    // --- Read core options ---
    struct retro_variable var = {0};

    // DVD detection toggle
    var.key = "vlc_dvd_detection";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (strcmp(var.value, "Disabled") == 0) {
            is_dvd = false;
        }
    }

    // Media type override
    var.key = "vlc_media_type";
    const char *media_type = "auto";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        media_type = var.value;
    }
    if (strcmp(media_type, "dvd") == 0) {
        is_dvd = true;
        is_bluray = false;
    } else if (strcmp(media_type, "bluray") == 0) {
        is_dvd = false;
        is_bluray = true;
    } else if (strcmp(media_type, "file") == 0) {
        is_dvd = false;
        is_bluray = false;
    }

    // --- Create media based on type ---
    if (is_dvd) {
        char dvd_mrl[4096];
        snprintf(dvd_mrl, sizeof(dvd_mrl), "dvd:///%s", path);
        m = libvlc_media_new_location(core.libvlc, dvd_mrl);
        fprintf(stderr, "[VLC] Opening as DVD: %s\n", dvd_mrl);

        var.key = "vlc_dvd_menu";
        const char *dvd_menu = "normal";
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            dvd_menu = var.value;
        }

        if (strcmp(dvd_menu, "disable") == 0) {
            libvlc_media_add_option(m, ":dvdread");
            fprintf(stderr, "[VLC] DVD menu: disabled (simple playback)\n");
        } else {
            libvlc_media_add_option(m, ":dvdnav");
            if (strcmp(dvd_menu, "force") == 0) {
                fprintf(stderr, "[VLC] DVD menu: force (skip intros)\n");
            } else {
                libvlc_media_add_option(m, ":no-dvdnav-menu");
                fprintf(stderr, "[VLC] DVD menu: normal (play intros)\n");
            }
        }

        var.key = "vlc_dvd_title";
        int dvd_title = 0;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            dvd_title = atoi(var.value);
            if (dvd_title < 0) dvd_title = 0;
            if (dvd_title > 9) dvd_title = 9;
        }
        char title_opt[32];
        snprintf(title_opt, sizeof(title_opt), ":dvd-title=%d", dvd_title);
        libvlc_media_add_option(m, title_opt);
        fprintf(stderr, "[VLC] DVD title set to %d\n", dvd_title);
    }
    else if (is_bluray) {
        char bd_mrl[4096];
        snprintf(bd_mrl, sizeof(bd_mrl), "bluray:///%s", path);
        m = libvlc_media_new_location(core.libvlc, bd_mrl);
        fprintf(stderr, "[VLC] Opening as Blu-ray: %s\n", bd_mrl);

        var.key = "vlc_bluray_menu";
        const char *bd_menu = "normal";
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            bd_menu = var.value;
        }
        if (strcmp(bd_menu, "disable") == 0) {
            libvlc_media_add_option(m, ":no-bluray-menu");
            fprintf(stderr, "[VLC] Blu-ray menu: disabled\n");
        } else {
            libvlc_media_add_option(m, ":bluray-menu");
            fprintf(stderr, "[VLC] Blu-ray menu: enabled\n");
        }
    }
    else if (is_online) {
        m = libvlc_media_new_location(core.libvlc, path);
    }
    else {
        m = libvlc_media_new_path(core.libvlc, path);
    }

    if (!m) {
        fprintf(stderr, "[VLC] Failed to create media\n");
        return false;
    }

    // --- Common options (demux + ONE video-filter + ONE audio-filter) ---
    if (strcasestr(path, ".m3u8")) {
        libvlc_media_add_option(m, ":demux=adaptive");
    } else {
        libvlc_media_add_option(m, ":demux=avformat");
    }

    // === INDIVIDUAL VIDEO FILTERS (only ONE :video-filter= line) ===
    char vfilters[256] = {0};
    const char *vsep = "";

    #define ADD_VFILTER(key, name) \
        do { \
            if (get_option_enabled(key)) { \
                strncat(vfilters, vsep, sizeof(vfilters) - strlen(vfilters) - 1); \
                strncat(vfilters, name, sizeof(vfilters) - strlen(vfilters) - 1); \
                vsep = ","; \
            } \
        } while(0)

    ADD_VFILTER("vlc_vf_sharpen",     "sharpen");
    ADD_VFILTER("vlc_vf_denoise",     "hqdn3d");
    ADD_VFILTER("vlc_vf_postproc",    "postproc");
    ADD_VFILTER("vlc_vf_deinterlace", "deinterlace");
    ADD_VFILTER("vlc_vf_blend",       "blend");
var.key = "vlc_sharpen_sigma";

    if (vfilters[0]) {
        char opt[300];
        snprintf(opt, sizeof(opt), ":video-filter=%s", vfilters);
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
    char sigma_opt[64];
    snprintf(sigma_opt, sizeof(sigma_opt), ":sharpen-sigma=%s", var.value);
    libvlc_media_add_option(m, sigma_opt);
    fprintf(stderr, "[VLC] Sharpen sigma set to %s\n", var.value);
}
        libvlc_media_add_option(m, opt);
        fprintf(stderr, "[VLC] Video filters: %s\n", vfilters);
    }

    // Overlay debug mode
    var.key = "vlc_overlay_debug";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (strcmp(var.value, "enabled") == 0) {
            libvlc_media_add_option(m, ":sub-filter=debug");
            libvlc_media_add_option(m, ":osd=1");
            fprintf(stderr, "[VLC] Overlay debug mode enabled\n");
        }
    }

    // DVD angle
    var.key = "vlc_dvd_angle";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        int angle = atoi(var.value);
        if (angle >= 1 && angle <= 4) {
            char angle_opt[32];
            snprintf(angle_opt, sizeof(angle_opt), ":dvd-angle=%d", angle);
            libvlc_media_add_option(m, angle_opt);
            fprintf(stderr, "[VLC] DVD angle set to %d\n", angle);
        }
    }

    // Aspect ratio
    var.key = "vlc_aspect_ratio";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && strcmp(var.value, "default") != 0) {
        char opt[64];
        snprintf(opt, sizeof(opt), ":aspect-ratio=%s", var.value);
        libvlc_media_add_option(m, opt);
        fprintf(stderr, "[VLC] Aspect ratio set to %s\n", var.value);
    }

    // Crop
    var.key = "vlc_crop";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && strcmp(var.value, "default") != 0) {
        char opt[64];
        snprintf(opt, sizeof(opt), ":crop=%s", var.value);
        libvlc_media_add_option(m, opt);
        fprintf(stderr, "[VLC] Crop set to %s\n", var.value);
    }

    // Video track
    var.key = "vlc_video_track";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        int track = atoi(var.value);
        if (track > 0) {
            char opt[32];
            snprintf(opt, sizeof(opt), ":video-track=%d", track);
            libvlc_media_add_option(m, opt);
            fprintf(stderr, "[VLC] Video track set to %d\n", track);
        }
    }

    // Deinterlace mode
    var.key = "vlc_deinterlace_mode";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && strcmp(var.value, "auto") != 0) {
        char opt[64];
        snprintf(opt, sizeof(opt), ":deinterlace-mode=%s", var.value);
        libvlc_media_add_option(m, opt);
        fprintf(stderr, "[VLC] Deinterlace mode set to %s\n", var.value);
    }

    // Scaling quality
    var.key = "vlc_swscale_mode";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        char opt[32];
        snprintf(opt, sizeof(opt), ":swscale-mode=%s", var.value);
        libvlc_media_add_option(m, opt);
        fprintf(stderr, "[VLC] Scaling mode set to %s\n", var.value);
    }

    // Post-processing quality
    var.key = "vlc_postproc_quality";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        char opt[32];
        snprintf(opt, sizeof(opt), ":postproc-quality=%s", var.value);
        libvlc_media_add_option(m, opt);
        fprintf(stderr, "[VLC] Postproc quality set to %s\n", var.value);
    }
    // === HARDWARE DECODING ===
    var.key = "vlc_hw_decoding";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (strcmp(var.value, "disabled") != 0) {
            char hw[128];
            if (strcmp(var.value, "auto") == 0)
                snprintf(hw, sizeof(hw), ":avcodec-hw=any");
            else
                snprintf(hw, sizeof(hw), ":avcodec-hw=%s", var.value);
            libvlc_media_add_option(m, hw);
            fprintf(stderr, "[VLC] Hardware decoding: %s\n", var.value);
        } else {
            libvlc_media_add_option(m, ":avcodec-hw=none");
            fprintf(stderr, "[VLC] Hardware decoding: disabled\n");
        }
    }

    // === CACHING ===
    var.key = "vlc_network_caching";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        char opt[64];
        snprintf(opt, sizeof(opt), ":network-caching=%s", var.value);
        libvlc_media_add_option(m, opt);
    }
    var.key = "vlc_file_caching";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        char opt[64];
        snprintf(opt, sizeof(opt), ":file-caching=%s", var.value);
        libvlc_media_add_option(m, opt);
    }

     // === MAX RESOLUTION ===
    var.key = "vlc_max_resolution";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (strcmp(var.value, "720p") == 0)   { core.max_width = 1280; core.max_height = 720; }
        else if (strcmp(var.value, "1080p") == 0) { core.max_width = 1920; core.max_height = 1080; }
        else if (strcmp(var.value, "1440p") == 0) { core.max_width = 2560; core.max_height = 1440; }
        else if (strcmp(var.value, "4K") == 0)    { core.max_width = 3840; core.max_height = 2160; }
        // "native" keeps original size (no clamping)
    }
    fprintf(stderr, "[VLC] Max resolution set to %ux%u\n", core.max_width, core.max_height);
    // === INDIVIDUAL AUDIO FILTERS ===
    char afilters[256] = {0};
    const char *asep = "";

    #define ADD_AFILTER(key, name) \
        do { \
            if (get_option_enabled(key)) { \
                strncat(afilters, asep, sizeof(afilters) - strlen(afilters) - 1); \
                strncat(afilters, name, sizeof(afilters) - strlen(afilters) - 1); \
                asep = ","; \
            } \
        } while(0)

    ADD_AFILTER("vlc_af_equalizer",   "equalizer");
    ADD_AFILTER("vlc_af_compressor",  "compressor");
    ADD_AFILTER("vlc_af_karaoke",     "karaoke");
    ADD_AFILTER("vlc_af_headphone",   "headphone");

    if (afilters[0]) {
        char opt[300];
        snprintf(opt, sizeof(opt), ":audio-filter=%s", afilters);
        libvlc_media_add_option(m, opt);
        fprintf(stderr, "[VLC] Audio filters: %s\n", afilters);
    }

    // Audio track
    var.key = "vlc_audio_track";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        int track = atoi(var.value);
        if (track > 0) {
            char opt[32];
            snprintf(opt, sizeof(opt), ":audio-track=%d", track);
            libvlc_media_add_option(m, opt);
            fprintf(stderr, "[VLC] Audio track set to %d\n", track);
        }
    }

    // Audio channels
    var.key = "vlc_audio_channels";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        int channels = atoi(var.value);
        if (channels > 0) {
            char opt[32];
            snprintf(opt, sizeof(opt), ":audio-channels=%d", channels);
            libvlc_media_add_option(m, opt);
            fprintf(stderr, "[VLC] Audio channels set to %d\n", channels);
        }
    }

    // Stereo mode
    var.key = "vlc_stereo_mode";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && strcmp(var.value, "Stereo") != 0) {
        char opt[64];
        snprintf(opt, sizeof(opt), ":stereo-mode=%s", var.value);
        libvlc_media_add_option(m, opt);
        fprintf(stderr, "[VLC] Stereo mode set to %s\n", var.value);
    }

    // Audio visualisation (fixed for pure audio files)
    var.key = "vlc_audio_visual";
if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && strcmp(var.value, "none") != 0) {
    // Always use the 'visual' plugin
    libvlc_media_add_option(m, ":audio-visual=visual");
    // Set the effect (spectrometer, spectrum, scope, vu)
    char effect_opt[64];
    snprintf(effect_opt, sizeof(effect_opt), ":effect-list=%s", var.value);
    libvlc_media_add_option(m, effect_opt);
    // Force a video canvas for pure audio files
   libvlc_media_add_option(m, ":effect-width=640");
        libvlc_media_add_option(m, ":effect-height=360");
  }

    // Audio resampler and quality
    var.key = "vlc_audio_resampler";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (strcmp(var.value, "soxr") == 0) {
            libvlc_media_add_option(m, ":audio-resampler=soxr");
            var.key = "vlc_soxr_quality";
            if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
                char opt[32];
                snprintf(opt, sizeof(opt), ":soxr-quality=%s", var.value);
                libvlc_media_add_option(m, opt);
            }
            fprintf(stderr, "[VLC] Audio resampler: soxr\n");
        } else {
            libvlc_media_add_option(m, ":audio-resampler=ugly");
            fprintf(stderr, "[VLC] Audio resampler: ugly\n");
        }
    }

    // Audio desync

    var.key = "vlc_audio_desync";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        core.audio_desync_ms = atoi(var.value);
        char opt[64];
        snprintf(opt, sizeof(opt), ":audio-desync=%d", core.audio_desync_ms);
        libvlc_media_add_option(m, opt);
        fprintf(stderr, "[VLC] Audio desync set to %d ms\n", core.audio_desync_ms);
    }

    // Text subtitle track
    var.key = "vlc_sub_track";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        int track = atoi(var.value);
        if (track > 0) {
            char opt[32];
            snprintf(opt, sizeof(opt), ":sub-track=%d", track);
            libvlc_media_add_option(m, opt);
            fprintf(stderr, "[VLC] Subtitle track set to %d\n", track);
        }
    }

    // Subtitle margin
    var.key = "vlc_sub_margin";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        int margin = atoi(var.value);
        if (margin > 0) {
            char opt[32];
            snprintf(opt, sizeof(opt), ":sub-margin=%d", margin);
            libvlc_media_add_option(m, opt);
            fprintf(stderr, "[VLC] Subtitle margin set to %d\n", margin);
        }
    }

    // Subtitle text size
    var.key = "vlc_sub_size";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && strcmp(var.value, "default") != 0) {
        char opt[64];
        if (strcmp(var.value, "small") == 0)
            snprintf(opt, sizeof(opt), ":sub-size=12");
        else if (strcmp(var.value, "medium") == 0)
            snprintf(opt, sizeof(opt), ":sub-size=18");
        else if (strcmp(var.value, "large") == 0)
            snprintf(opt, sizeof(opt), ":sub-size=24");
        else
            snprintf(opt, sizeof(opt), ":sub-size=%s", var.value);
        libvlc_media_add_option(m, opt);
        fprintf(stderr, "[VLC] Subtitle size set to %s\n", var.value);
    }

    libvlc_media_player_set_media(core.mp, m);
    libvlc_media_release(m);

    core.pending_start = true;

    pthread_mutex_lock(&core.mutex);
    core.transitioning = true;
    core.transition_timeout_frames = is_online ? 7200 : 14400;
    if (core.video_buffer && core.video_pitch > 0 && core.video_height > 0) {
        memset(core.video_buffer, 0, core.video_pitch * core.video_height);
    }
    pthread_mutex_unlock(&core.mutex);

    return true;
}
static bool parse_and_append(const char *path_or_url, char ***playlist, int *size, int *capacity, int depth) {
    if (depth > 5) {  // Prevent infinite recursion (e.g., self-referencing playlists)
        fprintf(stderr, "[VLC] Playlist recursion depth exceeded: %s\n", path_or_url);
        return false;
    }

    bool is_online = strstr(path_or_url, "://") != NULL;
    bool is_m3u8 = strcasestr(path_or_url, ".m3u8") != NULL;

    if (is_m3u8) {
        // Treat m3u8 as single stream (LibVLC handles variants internally)
        if (*size >= *capacity) {
            *capacity *= 2;
            *playlist = realloc(*playlist, *capacity * sizeof(char*));
            if (!*playlist) return false;
        }
        (*playlist)[(*size)++] = strdup(path_or_url);
        fprintf(stderr, "[VLC] Added m3u8 stream: %s\n", path_or_url);
        return true;
    }

    libvlc_media_t *media;
    if (is_online) {
        media = libvlc_media_new_location(core.libvlc, path_or_url);
    } else {
        media = libvlc_media_new_path(core.libvlc, path_or_url);
    }
    if (!media) {
        fprintf(stderr, "[VLC] Failed to create media for playlist: %s\n", path_or_url);
        return false;
    }

    // Parse (network for online, with timeout)
    libvlc_media_parse_with_options(media, is_online ? libvlc_media_parse_network : libvlc_media_parse_local, 10000);

    libvlc_media_list_t *subitems = libvlc_media_subitems(media);
    bool has_subitems = (subitems && libvlc_media_list_count(subitems) > 0);

    if (has_subitems) {
        // Recurse into sub-items (flatten nested m3u)
        int count = libvlc_media_list_count(subitems);
        for (int i = 0; i < count; i++) {
            libvlc_media_t *sub_media = libvlc_media_list_item_at_index(subitems, i);
            if (sub_media) {
                const char *mrl = libvlc_media_get_mrl(sub_media);
                if (mrl) {
                    parse_and_append(mrl, playlist, size, capacity, depth + 1);
                }
                libvlc_media_release(sub_media);
            }
        }
        libvlc_media_list_release(subitems);
    } else {
        // Not a playlist: Add as single item
        if (*size >= *capacity) {
            *capacity *= 2;
            *playlist = realloc(*playlist, *capacity * sizeof(char*));
            if (!*playlist) return false;
        }
        (*playlist)[(*size)++] = strdup(path_or_url);
        fprintf(stderr, "[VLC] Added media: %s\n", path_or_url);
    }

    libvlc_media_release(media);
    return true;
}
static bool parse_playlist(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[4096];
    char basedir[4096];
    strncpy(basedir, path, sizeof(basedir)-1);
    basedir[sizeof(basedir)-1] = '\0';
    char *slash = strrchr(basedir, '/');
    if (slash) *(slash + 1) = '\0';
    else basedir[0] = '\0';

    // Temp dynamic list
    int capacity = 16;
    core.playlist = malloc(capacity * sizeof(char*));
    core.playlist_size = 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (isspace(*p)) p++;
        if (!*p || *p == '#') continue;

        char *end = p + strlen(p) - 1;
        while (end >= p && isspace(*end)) *end-- = '\0';

        char full[4096];
        if (strstr(p, "://")) {
            snprintf(full, sizeof(full), "%s", p);
        } else {
            snprintf(full, sizeof(full), "%s%s", basedir, p);
        }

        // Recursively parse and append (handles nested m3u/m3u8)
        parse_and_append(full, &core.playlist, &core.playlist_size, &capacity, 0);
    }

    fclose(f);
    if (core.playlist_size == 0) {
        free(core.playlist);
        core.playlist = NULL;
        return false;
    }

    // Shrink to fit
    core.playlist = realloc(core.playlist, core.playlist_size * sizeof(char*));
    return true;
}

RETRO_API void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
        static const struct retro_variable vars[] = {
        // Existing options
        { "vlc_media_type", "Media type; auto|dvd|bluray|file" },
        { "vlc_dvd_menu", "DVD menu behavior; normal|force|disable" },
        { "vlc_bluray_menu", "Blu-ray menu behavior; normal|disable" },
        { "vlc_spu_enable", "Enable subtitles/overlays; auto|yes|no" },
        { "vlc_spu_track", "Subtitle/SPU track; 0|1|2|3|4|5|6|7|8|9" },
        { "vlc_x11_threads", "X11 thread safety; disabled|enabled" },
        { "vlc_overlay_debug", "Overlay debug mode; disabled|enabled" },
        { "vlc_dvd_angle", "DVD angle; 1|2|3|4" },
        { "vlc_mouse_enable", "Enable mouse for DVD menus; disabled|enabled" },
        { "vlc_mouse_speed", "Mouse pointer speed; 0.5|1.0|1.5|2.0" },
        { "vlc_mouse_click_on_a", "Use A button as mouse click; disabled|enabled" },

        // Video options
        { "vlc_aspect_ratio", "Aspect ratio; default|16:9|4:3|2.35:1|1.85:1|5:4|16:10" },
        { "vlc_crop", "Crop; default|16:9|4:3|2.35:1|1.85:1|5:4|16:10" },
        { "vlc_video_track", "Video track; 0|1|2|3|4|5" },
        { "vlc_deinterlace_mode", "Deinterlace mode; auto|blend|bob|linear|mean|x|yadif|yadif2x" },
        { "vlc_swscale_mode", "Scaling quality; 0=fast|1=linear|2=bicubic" },
        { "vlc_postproc_quality", "Post-processing quality; 0|1|2|3|4|5|6" },
       
        // Individual Video Filters (clean UX)
        { "vlc_vf_sharpen",     "Video: Sharpen; disabled|enabled" },
        { "vlc_vf_denoise",     "Video: Denoise (hqdn3d); disabled|enabled" },
        { "vlc_vf_postproc",    "Video: Post-processing; disabled|enabled" },
        { "vlc_vf_deinterlace", "Video: Deinterlace (as filter); disabled|enabled" },
        { "vlc_vf_blend",       "Video: Blend (overlays); disabled|enabled" },

        // Audio options
        { "vlc_audio_track", "Audio track; 0|1|2|3|4|5" },
        { "vlc_audio_channels", "Audio channels; 2 (Stereo)|1 (Mono)|4|5|6" },
        { "vlc_stereo_mode", "Stereo mode; Stereo|Left|Right|Reverse" },
        { "vlc_audio_visual", "Audio visualisation; none|spectrometer|spectrum|scope|vu" },
        { "vlc_af_equalizer",   "Audio: Equalizer; disabled|enabled" },
        { "vlc_af_compressor",  "Audio: Compressor; disabled|enabled" },
        { "vlc_af_karaoke",     "Audio: Karaoke; disabled|enabled" },
        { "vlc_af_headphone",   "Audio: Headphone virtualization; disabled|enabled" },
        { "vlc_audio_resampler", "Audio resampler; ugly|soxr" },
        { "vlc_soxr_quality", "SoX resampler quality; 0|1|2|3" },
        { "vlc_audio_desync", "Audio desync (ms); 0|50|100|120|150|200|250|-50|-100|-150" },
        // Subtitle/OSD
        { "vlc_sub_track", "Text subtitle track; 0|1|2|3|4|5" },
        { "vlc_sub_margin", "Subtitle bottom margin; 0|10|20|30|50|100" },
        { "vlc_sub_size", "Subtitle text size; default|small|medium|large" },
{ "vlc_hw_decoding",      "Hardware decoding; disabled|auto|dxva2|vaapi|d3d11va|nvdec|videotoolbox" },
        { "vlc_network_caching",  "Network caching (ms); 1000|2000|3000|5000|10000|20000|50000" },
        { "vlc_file_caching",     "File caching (ms); 300|1000|3000|10000" },
        { "vlc_max_resolution",   "Max output resolution (performance); native|720p|1080p|1440p|4K" },
        { NULL, NULL },
    };
    environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_init(void) {
    fprintf(stderr, "[VLC] retro_init() called\n");
    pthread_mutex_init(&core.mutex, NULL);

    // Read X11 thread safety option
    struct retro_variable var = {0};
    var.key = "vlc_x11_threads";
    bool x11_threads = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (strcmp(var.value, "enabled") == 0) {
            x11_threads = true;
        }
    }

    // Build args dynamically
    const char* base_args[] = {
        "--no-video-title-show",
        "--clock-jitter=0",
        "--network-caching=20000",
        "--file-caching=10000",
        "--live-caching=20000",
        "--http-reconnect",
        "--vout=vmem",
        "--aout=amem",
        "--sub-autodetect-file",
        "--audio-resampler",
        "--ac3-float",
        "--spu",
        NULL
    };

    int arg_count = 0;
    while (base_args[arg_count] != NULL) arg_count++;

    const char** args = malloc((arg_count + 2) * sizeof(char*));
    for (int i = 0; i < arg_count; i++) {
        args[i] = base_args[i];
    }

    if (x11_threads) {
        args[arg_count++] = "--no-xlib";
        fprintf(stderr, "[VLC] X11 thread safety enabled\n");
    }
    args[arg_count] = NULL;

    core.libvlc = libvlc_new(arg_count, args);
    free(args);
#ifdef _WIN32
    // Try to get the function pointer from the libvlc DLL
    HMODULE hLib = GetModuleHandle("libvlc.dll");
    if (!hLib) {
        // Some systems might have a different name
        hLib = GetModuleHandle("libvlc");
    }
    if (hLib) {
        dyn_libvlc_video_set_mouse_position = (pfn_libvlc_video_set_mouse_position)GetProcAddress(hLib, "libvlc_video_set_mouse_position");
        if (dyn_libvlc_video_set_mouse_position) {
            fprintf(stderr, "[VLC] Found libvlc_video_set_mouse_position\n");
        } else {
            fprintf(stderr, "[VLC] libvlc_video_set_mouse_position not found in DLL\n");
        }
    } else {
        fprintf(stderr, "[VLC] Could not get handle to libvlc DLL\n");
    }
#endif

    if (core.libvlc) {
        libvlc_log_set(core.libvlc, log_cb, NULL);
        fprintf(stderr, "[VLC] VLC instance created\n");

        static const struct retro_frame_time_callback frame_time_cb = { NULL, 1000 };
        environ_cb(RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK, (void*)&frame_time_cb);
    } else {
        fprintf(stderr, "[VLC] Failed to create VLC instance\n");
    }

    core.video_fps = 60.0;
    core.seeking = false;
    core.is_playing = false;
    core.audio_sent_frames = 0;
    core.sync_offset = 0;
    core.sync_offset_initialized = false;
    core.paused = false;
    core.last_audio_pts = 0;
    core.playlist = NULL;
    core.playlist_size = 0;
    core.playlist_index = 0;
    core.playlist_mode = false;
    core.transitioning = false;
    core.transition_timeout_frames = 0;
    core.video_buffer = NULL;
    core.video_width = 0;
    core.video_height = 0;
    core.video_pitch = 0;
	core.last_video_presented_ms = 0;
	core.spu_initialized = false;

    core.sample_accum_frac = 0.0;
	    core.max_width         = MAX_W;
    core.max_height        = MAX_H;
    core.audio_mute_frames = 0;
    core.audio_desync_ms   = 0;
  
}

RETRO_API bool retro_load_game(const struct retro_game_info *info) {
    fprintf(stderr, "[VLC] Loading: %s\n", info->path);

    if (!core.libvlc) return false;

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
  
    const char *path = info->path;
    bool is_playlist = (strcasestr(path, ".m3u") || strcasestr(path, ".m3u8"));

    if (is_playlist) {
        if (!parse_playlist(path)) {
            fprintf(stderr, "[VLC] Failed to parse playlist\n");
            return false;
        }
        core.playlist_mode = true;
        core.playlist_index = 0;
if (!load_media_file(core.playlist[0])) {
	fprintf(stderr, "[VLC] Failed to load first playlist item\n");
            return false;
        }
    } else {
        core.playlist_mode = false;
       if (!load_media_file(path)) {
            fprintf(stderr, "[VLC] Failed to load media file\n");
            return false;
        }
    }

    core.seeking = false;
    core.last_vlc_time = 0;
    core.audio_sent_frames = 0;
    core.sync_offset = 0;
    core.sync_offset_initialized = false;
    core.last_audio_pts = 0;
	core.last_video_presented_ms = 0;

    return true;
}

RETRO_API void retro_run(void) {
    static int frame_count = 0;
    frame_count++;
// Get current state
libvlc_state_t state = libvlc_media_player_get_state(core.mp);
core.is_playing = (state == libvlc_Playing);
double fps = (core.video_fps > 0.0) ? core.video_fps : 60.0;
int samples_per_frame = (int)(AUDIO_TARGET_RATE / fps + 0.5);
static int16_t silence_buffer[48000 * 2]; // large enough for one frame
    if (!core.libvlc || !core.mp) return;
if (core.pending_start && core.mp)
{
    fprintf(stderr, "[VLC] Starting playback on first active frame\n");
    libvlc_media_player_play(core.mp);
    core.pending_start = false;
}
    input_poll_cb();
// Inside retro_run(), after pause toggle

static bool prev_up = false, prev_down = false, prev_left = false, prev_right = false;
static bool prev_a = false, prev_select = false;

bool up    = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
bool down  = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
bool left  = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
bool right = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);
bool a     = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
bool b     = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
bool select= input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT);

// Always send navigation commands if media is playing (ignore has_menu check)
if (core.mp && core.is_playing) {
    if (up    && !prev_up)    libvlc_media_player_navigate(core.mp, libvlc_navigate_up);
    if (down  && !prev_down)  libvlc_media_player_navigate(core.mp, libvlc_navigate_down);
    if (left  && !prev_left)  libvlc_media_player_navigate(core.mp, libvlc_navigate_left);
    if (right && !prev_right) libvlc_media_player_navigate(core.mp, libvlc_navigate_right);
    if (a     && !prev_a)     libvlc_media_player_navigate(core.mp, libvlc_navigate_activate);
    // SELECT triggers the root DVD menu
  if (select && !prev_select) {
    libvlc_media_player_set_title(core.mp, 0); // attempt root menu

}
}

prev_up    = up;
prev_down  = down;
prev_left  = left;
prev_right = right;
prev_a     = a;
//prev_b     = b;
prev_select = select;
  // Pause toggle on X button (PlayStation X)
    static bool prev_x = false;
    bool x = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);
    if (x && !prev_x) {
        core.paused = !core.paused;
        libvlc_media_player_set_pause(core.mp, core.paused ? 1 : 0);
        fprintf(stderr, "[VLC] %s\n", core.paused ? "Paused" : "Resumed");
    }
    prev_x = x;

    // If paused, output silence and last video frame, then return
    if (core.paused || !core.is_playing) {
    memset(silence_buffer, 0, samples_per_frame * 2 * sizeof(int16_t));
    audio_batch_cb(silence_buffer, samples_per_frame);

    pthread_mutex_lock(&core.mutex);
    uint32_t *safe_buf = core.video_buffer;
    unsigned safe_w = core.video_width;
    unsigned safe_h = core.video_height;
    unsigned safe_p = core.video_pitch;
    pthread_mutex_unlock(&core.mutex);
    if (video_cb && safe_buf && safe_w > 0 && safe_h > 0) {
        video_cb(safe_buf, safe_w, safe_h, safe_p);
    }
    return;
}


    bool l1 = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L);
    bool r1 = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);
    bool l2 = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2);
    bool r2 = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2);



// Mouse control for DVD menus
struct retro_variable var = {0};
var.key = "vlc_mouse_enable";
bool mouse_enabled = false;
if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
    if (strcmp(var.value, "enabled") == 0) {
        mouse_enabled = true;
    }
}

if (core.is_playing && mouse_enabled && core.mp) {
    // Get mouse speed
    var.key = "vlc_mouse_speed";
    float speed = 1.0f;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        speed = atof(var.value);
    }
    
    // Get mouse deltas (relative movement)
    int16_t mouse_x = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
    int16_t mouse_y = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
    
    // Accumulate to absolute position
    static int abs_x = 0, abs_y = 0;
    abs_x += (int)(mouse_x * speed);
    abs_y += (int)(mouse_y * speed);
    
    // Clamp to video bounds
    if (abs_x < 0) abs_x = 0;
    if (abs_x >= (int)core.video_width) abs_x = core.video_width - 1;
    if (abs_y < 0) abs_y = 0;
    if (abs_y >= (int)core.video_height) abs_y = core.video_height - 1;
    
    // Enable mouse input in libVLC and set position
    libvlc_video_set_mouse_input(core.mp, true);
  if (dyn_libvlc_video_set_mouse_position) {
    dyn_libvlc_video_set_mouse_position(core.mp, 0, 0, abs_x, abs_y);
} else {
    static int mouse_warn_once = 0;
    if (!mouse_warn_once) {
        fprintf(stderr, "[VLC] Mouse position not supported by this libVLC version\n");
        mouse_warn_once = 1;
    }
}
    
    // Handle mouse click (left button)
    static bool prev_left = false;
    bool left = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
    if (left && !prev_left) {
        libvlc_media_player_navigate(core.mp, libvlc_navigate_activate);
        fprintf(stderr, "[VLC] Mouse click at (%d, %d)\n", abs_x, abs_y);
    }
    prev_left = left;
    
    // Optionally map A button to mouse click
    var.key = "vlc_mouse_click_on_a";
    bool a_as_click = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (strcmp(var.value, "enabled") == 0) {
            a_as_click = true;
        }
    }
    
    if (a_as_click) {
        static bool prev_a_mouse = false;
        bool a = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
        if (a && !prev_a_mouse) {
            libvlc_media_player_navigate(core.mp, libvlc_navigate_activate);
            fprintf(stderr, "[VLC] A button as mouse click\n");
        }
        prev_a_mouse = a;
    }
}


// Aspect ratio cycling on B button
static const char* aspect_presets[] = {
    "default", "16:9", "4:3", "2.35:1", "1.85:1", "5:4", "16:10", NULL
};
static int aspect_index = 0;
static bool prev_b_cycle = false;

if (core.is_playing && core.mp && b && !prev_b_cycle) {
    aspect_index++;
    if (aspect_presets[aspect_index] == NULL)
        aspect_index = 0;
    const char* new_aspect = aspect_presets[aspect_index];
    if (strcmp(new_aspect, "default") == 0)
        libvlc_video_set_aspect_ratio(core.mp, NULL);
    else
        libvlc_video_set_aspect_ratio(core.mp, new_aspect);
    fprintf(stderr, "[VLC] B button: aspect ratio set to %s\n", new_aspect);
}
prev_b_cycle = b;



// ===== RUNTIME OPTION CHANGES =====
if (core.mp && core.is_playing) {
    static struct {
        int  spu_enable;      // -1=no, 0=auto, 1=yes (we'll store as int for comparison)
        int  spu_track;
        char aspect[32];
        char crop[32];
        int  audio_track;
        int  audio_desync;    // ms
        char deint[32];       // store deinterlace mode string
        int  sub_track;
    } prev = {0};

    // --- SPU enable/track ---
    struct retro_variable var = {0};
    var.key = "vlc_spu_enable";
    const char *spu_enable_str = "auto";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        spu_enable_str = var.value;
    int spu_enable_val = 0; // 0=auto, 1=yes, -1=no
    if (strcmp(spu_enable_str, "yes") == 0)
        spu_enable_val = 1;
    else if (strcmp(spu_enable_str, "no") == 0)
        spu_enable_val = -1;

    var.key = "vlc_spu_track";
    int spu_track_val = 0;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        spu_track_val = atoi(var.value);

    if (spu_enable_val != prev.spu_enable || spu_track_val != prev.spu_track) {
        int spu_count = libvlc_video_get_spu_count(core.mp);
        if (spu_enable_val == -1) {
            libvlc_video_set_spu(core.mp, -1);
            fprintf(stderr, "[VLC] Runtime SPU disabled\n");
        } else if (spu_enable_val == 1) {
            // yes: use selected track (clamp if needed)
            if (spu_track_val < 0) spu_track_val = 0;
            if (spu_count > 0 && spu_track_val >= spu_count)
                spu_track_val = spu_count - 1;
            libvlc_video_set_spu(core.mp, spu_track_val);
            fprintf(stderr, "[VLC] Runtime SPU track set to %d\n", spu_track_val);
        } else { // auto
            if (spu_count > 0)
                libvlc_video_set_spu(core.mp, 0);
            else
                libvlc_video_set_spu(core.mp, -1);
            fprintf(stderr, "[VLC] Runtime SPU auto: %s\n", spu_count > 0 ? "enabled track 0" : "disabled");
        }
        prev.spu_enable = spu_enable_val;
        prev.spu_track = spu_track_val;
    }

   // --- Aspect ratio ---
var.key = "vlc_aspect_ratio";
const char *aspect_str = "default";
if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    aspect_str = var.value;

if (strcmp(aspect_str, prev.aspect) != 0) {
    if (strcmp(aspect_str, "default") == 0)
        libvlc_video_set_aspect_ratio(core.mp, NULL);
    else
        libvlc_video_set_aspect_ratio(core.mp, aspect_str);

    // Tell RetroArch the aspect changed
    struct retro_game_geometry geo = {0};
    geo.base_width  = core.video_width;
    geo.base_height = core.video_height;
    geo.max_width   = MAX_W;
    geo.max_height  = MAX_H;

    if (strcmp(aspect_str, "default") == 0)
        geo.aspect_ratio = (float)core.video_width / core.video_height;
    else {
        float w = 16.0f, h = 9.0f;
        sscanf(aspect_str, "%f:%f", &w, &h);
        geo.aspect_ratio = w / h;
    }

    environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geo);

    fprintf(stderr, "[VLC] Aspect ratio changed to %s (notified frontend)\n", aspect_str);
    strncpy(prev.aspect, aspect_str, sizeof(prev.aspect)-1);
    prev.aspect[sizeof(prev.aspect)-1] = '\0';
}

    // --- Crop ---
    var.key = "vlc_crop";
    const char *crop_str = "default";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        crop_str = var.value;
    if (strcmp(crop_str, prev.crop) != 0) {
        if (strcmp(crop_str, "default") == 0)
            libvlc_video_set_crop_geometry(core.mp, NULL);
        else
            libvlc_video_set_crop_geometry(core.mp, crop_str);
        fprintf(stderr, "[VLC] Runtime crop set to %s\n", crop_str);
        strncpy(prev.crop, crop_str, sizeof(prev.crop));
    }

    // --- Audio track ---
    var.key = "vlc_audio_track";
    int audio_track_val = 0;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        audio_track_val = atoi(var.value);
    if (audio_track_val != prev.audio_track) {
        if (audio_track_val > 0) {
            libvlc_audio_set_track(core.mp, audio_track_val);
            fprintf(stderr, "[VLC] Runtime audio track set to %d\n", audio_track_val);
        }
        prev.audio_track = audio_track_val;
    }

    // --- Audio desync ---
    var.key = "vlc_audio_desync";
    int audio_desync_val = 0;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        audio_desync_val = atoi(var.value);
    if (audio_desync_val != prev.audio_desync) {
        libvlc_audio_set_delay(core.mp, audio_desync_val * 1000); // convert ms to µs
        fprintf(stderr, "[VLC] Runtime audio delay set to %d ms\n", audio_desync_val);
        prev.audio_desync = audio_desync_val;
    }

    // --- Deinterlace mode (if function available) ---
#if defined(LIBVLC_VERSION_INT) && LIBVLC_VERSION_INT >= LIBVLC_VERSION(3,0,0,0)
    var.key = "vlc_deinterlace_mode";
    const char *deint_str = "auto";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        deint_str = var.value;
    if (strcmp(deint_str, prev.deint) != 0) {
        if (strcmp(deint_str, "auto") == 0)
            libvlc_video_set_deinterlace(core.mp, NULL);
        else
            libvlc_video_set_deinterlace(core.mp, deint_str);
        fprintf(stderr, "[VLC] Runtime deinterlace mode set to %s\n", deint_str);
        strncpy(prev.deint, deint_str, sizeof(prev.deint));
        prev.deint[sizeof(prev.deint)-1] = '\0'; // ensure null termination
    }
#endif

     // --- Subtitle track (same SPU API) ---
    var.key = "vlc_sub_track";
    int sub_track_val = 0;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        sub_track_val = atoi(var.value);
    if (sub_track_val != prev.sub_track) {
        int spu_count = libvlc_video_get_spu_count(core.mp);
        if (sub_track_val == 0) {
            // auto: enable first track if any, else disable
            if (spu_count > 0) {
                libvlc_video_set_spu(core.mp, 0);
                fprintf(stderr, "[VLC] Runtime subtitle auto: enabled track 0\n");
            } else {
                libvlc_video_set_spu(core.mp, -1);
                fprintf(stderr, "[VLC] Runtime subtitle auto: no tracks, disabled\n");
            }
        } else if (sub_track_val > 0 && sub_track_val <= spu_count) {
            libvlc_video_set_spu(core.mp, sub_track_val);
            fprintf(stderr, "[VLC] Runtime subtitle track set to %d\n", sub_track_val);
        } else {
            // invalid track, disable
            libvlc_video_set_spu(core.mp, -1);
            fprintf(stderr, "[VLC] Runtime subtitle track %d invalid, disabled\n", sub_track_val);
        }
        prev.sub_track = sub_track_val;
    }
}


    // Playlist skip (L2/R2)
    if (core.playlist_mode && core.mp) {
        int new_index = core.playlist_index;
        if (l2 && !prev_l2) new_index = (core.playlist_index - 1 + core.playlist_size) % core.playlist_size;
        if (r2 && !prev_r2) new_index = (core.playlist_index + 1) % core.playlist_size;

        if (new_index != core.playlist_index) {
            fprintf(stderr, "[VLC] Switching track %d → %d\n", core.playlist_index, new_index);
                        if (load_media_file(core.playlist[new_index])) {
                core.playlist_index = new_index;
                core.audio_sent_frames = 0;
                core.sync_offset_initialized = false;
                core.last_audio_pts = 0;
                core.last_vlc_time = 0;
                vlc_audio_flush();
            } else {
load_media_file(core.playlist[core.playlist_index]);
            }
        }
    }
    prev_l2 = l2; prev_r2 = r2;

    // Seek handling
    if (core.is_playing && ((l1 && !prev_l1) || (r1 && !prev_r1))) {
        core.seeking = true;
        int64_t t = libvlc_media_player_get_time(core.mp);
        int64_t new_time = r1 ? t + 10000 : (t < 10000 ? 0 : t - 10000);
        core.seek_target = new_time;
       vlc_audio_flush();
	   pthread_mutex_lock(&core.mutex);
    core.audio_mute_frames = 15;           // ← silence during seek
	pthread_mutex_unlock(&core.mutex);
        libvlc_media_player_set_time(core.mp, new_time);

        core.last_vlc_time = 0;
        core.audio_sent_frames = 0;
        core.sync_offset = 0;
        core.sync_offset_initialized = false;
        core.last_audio_pts = 0;
        fprintf(stderr, "[VLC] Seek to %lld ms\n", (long long)new_time);
    }
    prev_l1 = l1; prev_r1 = r1;

    if (core.transitioning) {
        pthread_mutex_lock(&core.mutex);
        bool ready = (core.video_width > 0 && core.video_height > 0 && core.video_buffer != NULL);
        pthread_mutex_unlock(&core.mutex);

        if (ready || --core.transition_timeout_frames == 0) {
            if (ready) {
                update_fps_from_current_media();
                struct retro_system_av_info av_info = {0};
                av_info.geometry.base_width   = core.video_width;
                av_info.geometry.base_height  = core.video_height;
                av_info.geometry.max_width    = MAX_W;
                av_info.geometry.max_height   = MAX_H;
                av_info.geometry.aspect_ratio = (float)core.video_width / core.video_height;
                av_info.timing.fps            = core.video_fps > 0 ? core.video_fps : 60.0;
                av_info.timing.sample_rate    = AUDIO_TARGET_RATE;
                environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
                fprintf(stderr, "[VLC] Track switch complete: %ux%u @ %.3f fps\n", core.video_width, core.video_height, core.video_fps);
            } else {
                fprintf(stderr, "[VLC] Track switch timeout\n");
                pthread_mutex_lock(&core.mutex);
                if (core.video_buffer) memset(core.video_buffer, 0, core.video_pitch * core.video_height);
                pthread_mutex_unlock(&core.mutex);
            }
            core.transitioning = false;
			pthread_mutex_lock(&core.mutex);
core.audio_mute_frames = 0;                // ← ready to play
			pthread_mutex_unlock(&core.mutex);

			
			
			// Reset audio sync to start fresh with correct FPS
core.audio_sent_frames = 0;
core.sync_offset_initialized = false;
core.sample_accum_frac = 0.0;
        }
    }
   
   if (state == libvlc_Error && core.playlist_mode) {
        fprintf(stderr, "[VLC] Media error → auto-next track\n");
        int new_index = (core.playlist_index + 1) % core.playlist_size;
load_media_file(core.playlist[new_index]);
        core.playlist_index = new_index;
        return;
    }

// === AUDIO SYNC (using PTS, with jump detection) ===
if (core.is_playing && audio_batch_cb) {
    if (core.audio_mute_frames > 0) {
    core.audio_mute_frames--;
    memset(silence_buffer, 0, samples_per_frame * 2 * sizeof(int16_t));
    audio_batch_cb(silence_buffer, samples_per_frame);
    return;   // or continue to video output
}
	pthread_mutex_lock(&core.mutex);

    int64_t pts_us = core.last_audio_pts;
    if (pts_us <= 0) {
        pthread_mutex_unlock(&core.mutex);
        // Send silence to keep pipeline alive
        static int16_t silence[64] = {0};
        audio_batch_cb(silence, 64);
        return;
    }

    // Convert PTS to sample count at target rate
    int64_t pts_frames = (pts_us * (int64_t)AUDIO_TARGET_RATE) / 1000000LL;

    // Target delay: 200 ms in samples
    int64_t target_delay_frames = (AUDIO_TARGET_RATE * 120) / 1000;

    // Calibration: after first valid PTS, set sync_offset to achieve target delay
    if (!core.sync_offset_initialized && pts_frames > 0) {
        core.sync_offset = pts_frames - core.audio_sent_frames - target_delay_frames;
        core.sync_offset_initialized = true;
        fprintf(stderr, "[VLC] PTS sync calibrated: target delay %lld frames (200 ms)\n",
                (long long)target_delay_frames);
    }

    if (!core.sync_offset_initialized) {
        pthread_mutex_unlock(&core.mutex);
        static int16_t silence[64] = {0};
        audio_batch_cb(silence, 64);
        return;
    }

    // Current error: positive means audio is behind (need to send more)
    int64_t error = pts_frames - core.sync_offset - core.audio_sent_frames;

    // Detect large jump (> 1 second) and recalibrate
    const int64_t JUMP_THRESHOLD = AUDIO_TARGET_RATE; // 1 second at 48 kHz
    if (llabs(error) > JUMP_THRESHOLD) {
        fprintf(stderr, "[VLC] Large sync jump detected (%lld frames), recalibrating\n",
                (long long)error);
        core.sync_offset = pts_frames - core.audio_sent_frames - target_delay_frames;
        error = pts_frames - core.sync_offset - core.audio_sent_frames; // should now be near target_delay
    }

    // Compute nominal samples to output this frame (based on video FPS)
    double fps = (core.video_fps > 0.0) ? core.video_fps : 60.0;
    double samples_per_frame = (double)AUDIO_TARGET_RATE / fps;
    int to_output = (int)(samples_per_frame + core.sample_accum_frac);
    core.sample_accum_frac += samples_per_frame - to_output;
    if (to_output > 5000) to_output = 5000; // safety cap

    // Available audio frames in the ring buffer
    size_t r = core.audio_read_pos;
    size_t w = core.audio_write_pos;
    size_t avail = (w >= r) ? (w - r) : (AUDIO_BUFFER_SIZE - r + w);
    size_t frames_avail = avail / 2;

    // How many of this frame's samples will be real audio
    int64_t real_to_send = error;
    if (real_to_send < 0) real_to_send = 0;
    if (real_to_send > to_output) real_to_send = to_output;
    if (real_to_send > (int64_t)frames_avail) real_to_send = frames_avail;

    // Allocate output buffer on the stack
    int16_t out_buffer[to_output * 2];

    if (real_to_send > 0) {
        // Copy real audio from ring
        size_t contig = (AUDIO_BUFFER_SIZE - r) / 2;
        if ((size_t)real_to_send <= contig) {
            memcpy(out_buffer, core.audio_ring + r, real_to_send * 2 * sizeof(int16_t));
        } else {
            size_t first = contig;
            size_t second = real_to_send - first;
            memcpy(out_buffer, core.audio_ring + r, first * 2 * sizeof(int16_t));
            memcpy(out_buffer + first * 2, core.audio_ring, second * 2 * sizeof(int16_t));
        }
        // Pad the rest with silence
        if (real_to_send < to_output) {
            memset(out_buffer + real_to_send * 2, 0,
                   (to_output - real_to_send) * 2 * sizeof(int16_t));
        }
        // Update ring read position and content counter
        core.audio_read_pos = (r + real_to_send * 2) % AUDIO_BUFFER_SIZE;
        core.audio_sent_frames += real_to_send;
    } else {
        // Entire frame is silence
        memset(out_buffer, 0, to_output * 2 * sizeof(int16_t));
    }

    pthread_mutex_unlock(&core.mutex);

    // Send the constant‑size block to the frontend
    audio_batch_cb(out_buffer, to_output);
}
    // === VIDEO OUTPUT (always after transitioning) ===
    pthread_mutex_lock(&core.mutex);
    uint32_t *safe_buf = core.video_buffer;
    unsigned safe_w = core.video_width;
    unsigned safe_h = core.video_height;
    unsigned safe_p = core.video_pitch;
    pthread_mutex_unlock(&core.mutex);

    if (core.is_playing && video_cb && safe_buf && safe_w > 0 && safe_h > 0 && !core.transitioning) {
        video_cb(safe_buf, safe_w, safe_h, safe_p);
    }
}
RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info) {
    double fps = (core.video_fps > 0) ? core.video_fps : 60.0;

    info->geometry.base_width   = core.video_width ? core.video_width : 1280;
    info->geometry.base_height  = core.video_height ? core.video_height : 720;
    info->geometry.max_width    = MAX_W;
    info->geometry.max_height   = MAX_H;
    info->geometry.aspect_ratio = (float)info->geometry.base_width / info->geometry.base_height;
    info->timing.fps            = fps;
    info->timing.sample_rate    = AUDIO_TARGET_RATE;

    fprintf(stderr, "[VLC] Reporting AV Info: %ux%u, FPS=%.3f\n",
            info->geometry.base_width, info->geometry.base_height, fps);
}

RETRO_API void retro_get_system_info(struct retro_system_info *i) {
    static char library_name[]    = "VLC Media Player";
    static char library_version[] = "1.0";
    static char valid_extensions[] = "mp4|mkv|avi|mov|flv|webm|m3u|m3u8";

    i->library_name    = library_name;
    i->library_version = library_version;
    i->valid_extensions = valid_extensions;
    i->need_fullpath   = true;
}

RETRO_API void retro_deinit(void) {
    fprintf(stderr, "[VLC] retro_deinit()\n");

    if (core.mp) {
        libvlc_media_player_stop(core.mp);
        libvlc_media_player_release(core.mp);
    }
    if (core.libvlc)
        libvlc_release(core.libvlc);

    if (core.playlist) {
        for (int i = 0; i < core.playlist_size; i++)
            free(core.playlist[i]);
        free(core.playlist);
    }

    pthread_mutex_destroy(&core.mutex);
}

// Required stubs
RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }
RETRO_API void retro_unload_game(void) { }
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API void retro_reset(void) {}
RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void *data, size_t size) { return false; }
RETRO_API bool retro_unserialize(const void *data, size_t size) { return false; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code) {}
RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num_info) { return false; }
RETRO_API void *retro_get_memory_data(unsigned id) { return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { return 0; }
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {}