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
char current_channel_path[MAX_PATH] = {0};
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
static int64_t get_time_us(void)
{
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (counter.QuadPart * 1000000) / freq.QuadPart;
}
#else
#include <time.h>
static int64_t get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
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

// The event callback function
// The event callback function
static void vlc_event_cb(const struct libvlc_event_t *ev, void *data) {
  
}

static libvlc_media_t* create_media(const char *path,
                                    bool *out_is_dvd,
                                    bool *out_is_online,
                                    bool *out_is_bluray) {
    bool is_online = (strstr(path, "://") != NULL);
    bool is_dvd = false;
    bool is_bluray = false;

    // --- Auto-detection (same as load_media_file) ---
    const char *ext = strrchr(path, '.');
    if (ext && (strcasecmp(ext, ".iso") == 0)) is_dvd = true;
#ifdef _WIN32
    if (strlen(path) >= 2 && path[1] == ':') {
        const char *rest = path + 2;
        if (*rest == '\0' || ((*rest == '\\' || *rest == '/') && *(rest+1) == '\0'))
            is_dvd = true;
    }
#endif
if (is_dvd && (strcasestr(path, "laserdisc") || strcasestr(path, "(ld)"))) {
        fprintf(stderr, "[VLC] LaserDisc/File override detected. Disabling DVD mode.\n");
        is_dvd = false;
    }
    // --- Read core options ---
    struct retro_variable var = {0};

   
   
    if (is_online) {
        is_dvd = false; is_bluray = false;
    }

    // --- Create base media ---
    libvlc_media_t *m = NULL;
    if (is_online) {
        m = libvlc_media_new_location(core.libvlc, path);
        fprintf(stderr, "[VLC] Creating online media: %s\n", path);
    } else if (is_dvd) {
        char dvd_mrl[4096];
        snprintf(dvd_mrl, sizeof(dvd_mrl), "dvd:///%s", path);
        m = libvlc_media_new_location(core.libvlc, dvd_mrl);
        fprintf(stderr, "[VLC] Creating DVD media: %s\n", dvd_mrl);
    } else if (is_bluray) {
        char bd_mrl[4096];
        snprintf(bd_mrl, sizeof(bd_mrl), "bluray:///%s", path);
        m = libvlc_media_new_location(core.libvlc, bd_mrl);
        fprintf(stderr, "[VLC] Creating Blu-ray media: %s\n", bd_mrl);
    } else {
        m = libvlc_media_new_path(core.libvlc, path);
        fprintf(stderr, "[VLC] Creating local file media: %s\n", path);
    }
    if (!m) return NULL;

    // --- Demux selection (must come before other options that depend on type) ---
    if (core.iptv_menu_enabled == true) {
		 	libvlc_media_add_option(m, ":avformat");
		 fprintf(stderr, "[VLC] IPTV MODE ********************************: %s\n", path);

	libvlc_media_add_option(m, ":adaptive-logic=fixedrate");
//libvlc_media_add_option(m, ":demux=avformat");
	libvlc_media_add_option(m, ":network-caching=5000");
libvlc_media_add_option(m, ":live-caching=5000");
//libvlc_media_add_option(m, ":adaptive-use-access");
			libvlc_media_add_option(m, ":http-reconnect");
		//	libvlc_media_add_option(m, ":live-playback-delay=15000");
			libvlc_media_add_option(m, ":avcodec-hw=none");
			libvlc_media_add_option(m, ":adaptive-use-access=true");
libvlc_media_add_option(m, ":adaptive-force=true");
			libvlc_media_add_option(m, ":adaptive-maxwidth=1920");
			libvlc_media_add_option(m, ":adaptive-maxheight=1080");
libvlc_media_add_option(m, ":clock-jitter=0");

//libvlc_media_add_option(m, ":clock-jitter=0");
//libvlc_media_add_option(m, ":clock-synchro=0");
//libvlc_media_add_option(m, ":drop-late-frames");
		//	libvlc_media_add_option(m, ":no-ts-trust-pcr");
    } else if (!is_online) {
        libvlc_media_add_option(m, ":demux=avformat");
    }

    // --- Online options ---
    if (is_online) {
        libvlc_media_add_option(m, ":network-caching=5000");
        libvlc_media_add_option(m, ":live-caching=2500");
        libvlc_media_add_option(m, ":http-reconnect");
        libvlc_media_add_option(m, ":avcodec-hw=none");
     }

    // --- DVD options ---
    if (is_dvd) 
	{
fprintf(stderr, "DVD MODE\n");    
        fprintf(stderr, "[VLC] DVD MIODE *****************************: %s\n", path);
	libvlc_media_add_option(m, ":demux=dvdnav");
              
	libvlc_media_add_option(m, ":no-dvdsub-transparency");	
	libvlc_media_add_option(m, ":disc-caching=300");
		libvlc_media_add_option(m, ":dvdnav");
        libvlc_media_add_option(m, ":no-dvdnav-menu");
        libvlc_media_add_option(m, ":no-dvdnav-mouse-events");
        libvlc_media_add_option(m, ":avcodec-hw=none");
        libvlc_media_add_option(m, ":dvd-title=0");
		libvlc_media_add_option(m, ":dvd-angle=1");
		libvlc_media_add_option(m, ":spu");
		
    }

    // --- Blu-ray options ---
    if (is_bluray) {
        var.key = "vlc_bluray_menu";
        const char *bd_menu = "normal";
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
            bd_menu = var.value;
        if (strcmp(bd_menu, "disable") == 0)
            libvlc_media_add_option(m, ":no-bluray-menu");
        else
            libvlc_media_add_option(m, ":bluray-menu");
    }


    if (out_is_dvd)    *out_is_dvd = is_dvd;
    if (out_is_online) *out_is_online = is_online;
    if (out_is_bluray) *out_is_bluray = is_bluray;

    return m;
}

bool switch_to_media(const char *path) {
    fprintf(stderr, "[VLC] Switching to playlist item: %s\n", path);

    if (!core.mp) {
        core.mp = libvlc_media_player_new(core.libvlc);
        if (!core.mp) return false;
        vlc_video_setup_callbacks(core.mp);
        vlc_audio_setup_callbacks(core.mp);
		libvlc_event_manager_t *em = libvlc_media_player_event_manager(core.mp);
libvlc_event_attach(em, libvlc_MediaPlayerESDeleted, vlc_event_cb, NULL);
    }

    libvlc_media_player_stop(core.mp);
    int waited = 0;
    while (libvlc_media_player_get_state(core.mp) != libvlc_Stopped && waited < 150) {
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10000);
#endif
        waited++;
    }

    // === ONLY free the MENU buffer (we no longer zero video width) ===
    pthread_mutex_lock(&core.mutex);
    if (!core.menu_active) {
        free(core.video_buffer);
        core.video_buffer = NULL;
    }
    // REMOVED: core.video_width = core.video_height = 0;  ← this was the bug
    pthread_mutex_unlock(&core.mutex);

    bool is_dvd, is_online, is_bluray;
    libvlc_media_t *m = create_media(path, &is_dvd, &is_online, &is_bluray);
    if (!m) {
        fprintf(stderr, "[VLC] Failed to create media for %s\n", path);
        return false;
    }
    // === FORCE resync gate immediately (works for both initial and stitches) ===
   
  
    libvlc_media_player_set_media(core.mp, m);
    libvlc_media_release(m);


    core.pending_play = true;

    return true;
}

static bool load_media_file(const char *path) {
    fprintf(stderr, "[VLC] Loading media: %s\n", path);
    if (core.mp) {
        libvlc_media_player_stop(core.mp);
        libvlc_media_player_release(core.mp);
        core.mp = NULL;
    }

    core.mp = libvlc_media_player_new(core.libvlc);
    if (!core.mp) return false;

    vlc_video_setup_callbacks(core.mp);
    vlc_audio_setup_callbacks(core.mp);
libvlc_event_manager_t *em = libvlc_media_player_event_manager(core.mp);
libvlc_event_attach(em, libvlc_MediaPlayerESDeleted, vlc_event_cb, NULL);
    libvlc_media_t *m = create_media(path, NULL, NULL, NULL);
    if (!m) {
        fprintf(stderr, "[VLC] Failed to create media\n");
        return false;
    }

    libvlc_media_player_set_media(core.mp, m);
    libvlc_media_release(m);
 core.pending_play = true;
    return true;
}

static bool parse_and_append(const char *path_or_url, char ***playlist, int *size, int *capacity, int depth) {
    if (depth > 5) {  // Prevent infinite recursion
        fprintf(stderr, "[VLC] Playlist recursion depth exceeded: %s\n", path_or_url);
        return false;
    }

    bool is_online = strstr(path_or_url, "://") != NULL;
    bool is_m3u8 = strcasestr(path_or_url, ".m3u8") != NULL;

    if (is_m3u8) {
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

    libvlc_media_parse_with_options(media, is_online ? libvlc_media_parse_network : libvlc_media_parse_local, 10000);

    libvlc_media_list_t *subitems = libvlc_media_subitems(media);
    bool has_subitems = (subitems && libvlc_media_list_count(subitems) > 0);

    if (has_subitems) {
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
        { "vlc_iptv_menu", "IPTV Menu (M3U groups); disabled|enabled" },
        { "vlc_iptv_resolution", "IPTV output resolution; 1080p|720p|1440p|4K" },
		{"vlc_ctts_fix", "Aggressive CTTS fix for broken MP4; disabled|enabled" },
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
        { "vlc_audio_visual", "Audio visualisation; none|libgoom|goom|projectm|glspectrum|spectrum|spectrometer|scope|vu" },
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

  
  
enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
    // === BASE VLC ARGUMENTS ===
    const char* base_args[] = {
		    "--no-video-title-show",
    "--quiet",
//    "--no-xlib",
  //  "--no-osd",
   // "--clock-jitter=0",
   //     "--network-caching=2000",
        "--file-caching=1000",
     //   "--live-caching=2000",
  //  "--no-audio-time-stretch",
 //  "--codec=avcodec",
 //  "--codec=avcodec",
//"--avcodec-hw=d3d11va",
  //  "--avcodec-fast",
    "--no-drop-late-frames",
    "--http-reconnect",
    "--vout=vmem",
    "--aout=amem"

    };

    int arg_count = 0;
    while (base_args[arg_count] != NULL) arg_count++;

    const char** args = malloc((arg_count + 3) * sizeof(char*));
    for (int i = 0; i < arg_count; i++) args[i] = base_args[i];

   

    // === SET VLC_PLUGIN_PATH (this is the only way that works now) ===
    const char *core_path = NULL;
    const char *sys_dir = NULL;
    environ_cb(RETRO_ENVIRONMENT_GET_LIBRETRO_PATH, &core_path);
    environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys_dir);

    char root[4096] = {0};
    if (core_path) {
        strncpy(root, core_path, sizeof(root)-1);
        char *sep = strrchr(root, '/'); if (!sep) sep = strrchr(root, '\\');
        if (sep) *sep = '\0';                    // cores
        sep = strrchr(root, '/'); if (!sep) sep = strrchr(root, '\\');
        if (sep) *sep = '\0';                    // RetroArch root
    }

    char env[8192];
    if (strlen(root) > 0) {
        snprintf(env, sizeof(env), "VLC_PLUGIN_PATH=%s/retroarch-plugins-visualization;%s/plugins;%s/plugins/visualization",
                 root, root, root);
    } else if (sys_dir) {
        snprintf(env, sizeof(env), "VLC_PLUGIN_PATH=%s/retroarch-plugins-visualization;%s/plugins;%s/plugins/visualization",
                 sys_dir, sys_dir, sys_dir);
    } else {
        strcpy(env, "VLC_PLUGIN_PATH=plugins");
    }

    _putenv(env);
    fprintf(stderr, "[VLC] Set VLC_PLUGIN_PATH = %s\n", env + 15);

    // === CREATE VLC ===
    core.libvlc = libvlc_new(arg_count, args);
    free(args);

    if (core.libvlc) {
        libvlc_log_set(core.libvlc, log_cb, NULL);
        fprintf(stderr, "[VLC] VLC instance created successfully\n");

        static const struct retro_frame_time_callback frame_time_cb = { NULL, 1000 };
        environ_cb(RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK, (void*)&frame_time_cb);
    } else {
        fprintf(stderr, "[VLC] Failed to create VLC instance\n");
    }

#ifdef _WIN32
    HMODULE hLib = GetModuleHandle("libvlc.dll");
    if (!hLib) hLib = GetModuleHandle("libvlc");
    if (hLib) {
        dyn_libvlc_video_set_mouse_position = (pfn_libvlc_video_set_mouse_position)
            GetProcAddress(hLib, "libvlc_video_set_mouse_position");
        if (dyn_libvlc_video_set_mouse_position)
            fprintf(stderr, "[VLC] Found libvlc_video_set_mouse_position\n");
    }
#endif

    // === CORE STATE ===
    core.video_fps = 60.0;
    core.is_playing = false;
    core.paused = false;
    core.playlist = NULL;
    core.playlist_size = 0;
    core.playlist_index = 0;
    core.playlist_mode = false;
    core.video_buffer = NULL;
    core.video_width = 0;
    core.video_height = 0;
    core.video_pitch = 0;
    core.max_width = MAX_W;
    core.max_height = MAX_H;
	core.pending_play = false;
	core.stitch_resync_pending = false;
core.stitch_seek_pending = false;
core.exit_menu = false;
core.last_video_frame_time = 0;
core.last_time = -1;
core.video_frame_seen = false;
core.initial_load = false;
core.true_discontinuity_pending = false;
}

static void init_menu_video_buffer(void) {
    pthread_mutex_lock(&core.mutex);
    // Free any existing buffer
    if (core.video_buffer) {
        free(core.video_buffer);
        core.video_buffer = NULL;
    }
    core.video_width  = 1280;
    core.video_height = 720;
    core.video_pitch  = 1280 * 4;
    core.video_buffer = (uint32_t*)calloc(1, (size_t)core.video_pitch * core.video_height);
    pthread_mutex_unlock(&core.mutex);

    if (!core.video_buffer) {
        fprintf(stderr, "[VLC] Failed to allocate menu buffer!\n");
        return;
    }

    struct retro_game_geometry geo = {0};
    geo.base_width   = 1280;
    geo.base_height  = 720;
    geo.max_width    = MAX_W;
    geo.max_height   = MAX_H;
    geo.aspect_ratio = 16.0f / 9.0f;
    environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geo);

    fprintf(stderr, "[VLC] Menu video buffer initialized 1280x720\n");
}

RETRO_API bool retro_load_game(const struct retro_game_info *info) {
    fprintf(stderr, "[VLC] Loading: %s\n", info->path);

    if (!core.libvlc) return false;

    const char *path = info->path;
  

bool is_playlist = (strcasestr(path, ".m3u") || strcasestr(path, ".m3u8"));

if (is_playlist) {
    struct retro_variable var = { .key = "vlc_iptv_menu" };
    core.iptv_menu_enabled = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        core.iptv_menu_enabled = (strcmp(var.value, "enabled") == 0);

    core.playlist_mode = true;
    core.playlist_index = 0;

   if (core.iptv_menu_enabled) {
        core.menu_active = true;
        if (!vlc_menu_init(path)) {
            fprintf(stderr, "[VLC] IPTV menu init failed – falling back to normal playlist\n");
            core.iptv_menu_enabled = false;
        } else {
                 init_menu_video_buffer();   // keeps dark background
                 struct retro_variable res_var = { .key = "vlc_iptv_resolution" };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &res_var) && res_var.value) {
        if (strcmp(res_var.value, "720p") == 0) {
            core.iptv_fixed_width = 1280;
            core.iptv_fixed_height = 720;
        } else if (strcmp(res_var.value, "1440p") == 0) {
            core.iptv_fixed_width = 2560;
            core.iptv_fixed_height = 1440;
        } else if (strcmp(res_var.value, "4K") == 0) {
            core.iptv_fixed_width = 3840;
            core.iptv_fixed_height = 2160;
        } else { // "1080p" or default
            core.iptv_fixed_width = 1920;
            core.iptv_fixed_height = 1080;
        }
    } else {
        // Fallback if option not found
        core.iptv_fixed_width = 1920;
        core.iptv_fixed_height = 1080;
    }
		}
    }

    if (!core.iptv_menu_enabled) {
        core.menu_active = false;
        if (!parse_playlist(path)) return false;
        if (core.playlist_size > 0)
            load_media_file(core.playlist[0]);
    }
} else {
    core.playlist_mode = false;
    core.menu_active = false;
    if (!load_media_file(path)) return false;
}

    core.seeking = false;


    return true;
}

/*
 * vlc_stitch_try_commit
 *
 * Called every frame while a dual-buffer stitch is in flight.
 *
 * Strategy:
 *   Fill buf B (staging) with new audio+video while buf A (old) drains.
 *   Once buf A's audio ring is empty — meaning every sample from the old
 *   stream has been sent to the frontend — flip both audio and video read
 *   pointers to buf B.  The old buf A is then reset and becomes the next
 *   staging buffer on the following stitch.
 *
 * Audio is the authoritative drain signal because it is continuous: even if
 * there are no more ready video frames in the old buffer, audio may still have
 * a few hundred milliseconds of samples to flush.  Waiting for audio ensures
 * we never cut the old stream short.
 */
void vlc_stitch_try_commit(void)
{
    if (!core.stitch_switch_pending)
        return;

    if (vlc_audio_read_buf_fill() > 0)
        return;   /* old audio still draining — nothing to do yet */

    /* Old buffer is dry — commit both audio and video atomically. */
    vlc_stitch_commit_audio();   /* flip audio read pointer to staging */
    vlc_video_stitch_commit();   /* flip video read pointer to staging */

    pthread_mutex_lock(&core.mutex);
    core.stitch_switch_pending = false;
    core.stitch_resync_pending = false;
    pthread_mutex_unlock(&core.mutex);

    fprintf(stderr, "[VLC-CORE] Stitch committed — both buffers switched.\n");
}

void vlc_stitch_cancel(void)
{
    pthread_mutex_lock(&core.mutex);
    core.stitch_switch_pending = false;
    core.stitch_resync_pending = false;
    pthread_mutex_unlock(&core.mutex);
    fprintf(stderr, "[VLC-CORE] Stitch cancelled.\n");
}

RETRO_API void retro_run(void)
{
    input_poll_cb();

    // 1. Read Inputs (Added L1 and R1)
    bool up      = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
    bool down    = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
    bool left    = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
    bool right   = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);
   bool l1 = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L); // Removed '1'
bool r1 = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R); // Removed '1'
    bool a       = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
    bool start   = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
    bool select  = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT);
    bool l2      = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2);
    bool r2      = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2);
    bool x       = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);

    // 2. Debounce (Added L1 and R1)
    static bool prev_up = false, prev_down = false, prev_left = false, prev_right = false;
    static bool prev_l1 = false, prev_r1 = false;
    static bool prev_a = false, prev_start = false, prev_select = false;
    static bool prev_l2 = false, prev_r2 = false, prev_x = false;

    // === IPTV MENU MODE ===
    if (core.playlist_mode && core.iptv_menu_enabled && core.menu_active) {
        vlc_menu_handle_input();
        vlc_menu_draw();
        video_cb(core.video_buffer, core.video_width, core.video_height, core.video_pitch);
        return;
    }

    if (!core.mp) goto end_inputs;


    // Start playback on first run 
	if (!core.menu_active && core.pending_play) { 
	core.pending_play = false;
	libvlc_media_player_play(core.mp); }

    // === ERROR DETECTION ===
    if (core.play_start_attempt) {
        libvlc_state_t state = libvlc_media_player_get_state(core.mp);
        if (state == libvlc_Error || (state == libvlc_Stopped && core.play_attempt_frames > 60)) {
            struct retro_message msg = { "Failed to play – bad link?", 180 };
            environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
           
        } else if (state == libvlc_Playing) {
            core.play_start_attempt = false;
        }
        core.play_attempt_frames++;
    }

    // === SELECT = Toggle Menu ===
    if (select && !prev_select && core.playlist_mode && core.iptv_menu_enabled) {
        core.menu_active = !core.menu_active;
        if (core.menu_active) {
            if (core.mp) libvlc_media_player_stop(core.mp);
            init_menu_video_buffer();
        } else {
            core.pending_play = false;
        }
    }

    // === START = Pause/Play ===
    if (start && !prev_start) {
        core.paused = !core.paused;
        libvlc_media_player_set_pause(core.mp, core.paused ? 1 : 0);
    }

    // === UP/DOWN = Channel Zapping ===
    if (core.playlist_mode && !core.menu_active) {
        if (up && !prev_up) {
            int new_idx = (core.playlist_index - 1 + core.playlist_size) % core.playlist_size;
            if (switch_to_media(core.playlist[new_idx])) core.playlist_index = new_idx;
        }
        if (down && !prev_down) {
            int new_idx = (core.playlist_index + 1) % core.playlist_size;
            if (switch_to_media(core.playlist[new_idx])) core.playlist_index = new_idx;
        }
    }

    // === UPDATE PLAYING STATE ===
    libvlc_state_t state = libvlc_media_player_get_state(core.mp);
    core.is_playing = (state == libvlc_Playing);

    // === X BUTTON PAUSE ===
    if (x && !prev_x) {
        core.paused = !core.paused;
        libvlc_media_player_set_pause(core.mp, core.paused ? 1 : 0);
    }

    if (core.paused) {
        pthread_mutex_lock(&core.mutex);
        if (video_cb && core.video_buffer && core.video_width && core.video_height)
            video_cb(core.video_buffer, core.video_width, core.video_height, core.video_pitch);
        pthread_mutex_unlock(&core.mutex);
        goto end_inputs;
    }

   // === DVD NAVIGATION + SEEK ===
    if (core.mp && core.is_playing) {
        // Up/Down/Left/Right used for DVD Menu Navigation
        if (up && !prev_up)       libvlc_media_player_navigate(core.mp, libvlc_navigate_up);
        if (down && !prev_down)   libvlc_media_player_navigate(core.mp, libvlc_navigate_down);
        if (left && !prev_left)   libvlc_media_player_navigate(core.mp, libvlc_navigate_left);
        if (right && !prev_right) libvlc_media_player_navigate(core.mp, libvlc_navigate_right);
        if (a && !prev_a)         libvlc_media_player_navigate(core.mp, libvlc_navigate_activate);

        // L1 / R1 used for Seeking back and forth
        if (l1 && !prev_l1) {
            int64_t t = libvlc_media_player_get_time(core.mp);
            libvlc_media_player_set_time(core.mp, t - 10000); // Back 10s
            struct retro_message msg = { "Seek ← 10s", 150 };
            environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
        }
        if (r1 && !prev_r1) {
            int64_t t = libvlc_media_player_get_time(core.mp);
            libvlc_media_player_set_time(core.mp, t + 10000); // Forward 10s
            struct retro_message msg = { "Seek → 10s", 150 };
            environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
        }
    }

    // === L2/R2 PLAYLIST SKIP ===
    if (core.playlist_mode && ((l2 && !prev_l2) || (r2 && !prev_r2))) {
        int new_index = core.playlist_index;
        if (l2 && !prev_l2) new_index = (core.playlist_index - 1 + core.playlist_size) % core.playlist_size;
        if (r2 && !prev_r2) new_index = (core.playlist_index + 1) % core.playlist_size;

        if (new_index != core.playlist_index) {
            if (switch_to_media(core.playlist[new_index]))
                core.playlist_index = new_index;
        }
    }

end_inputs:
   // Update debounce (ensure l1/r1 are included here)
    prev_up     = up;
    prev_down   = down;
    prev_left   = left;
    prev_right  = right;
    prev_l1     = l1;
    prev_r1     = r1;
    prev_a      = a;
    prev_start  = start;
    prev_select = select;
    prev_l2     = l2;
    prev_r2     = r2;
    prev_x      = x;
int64_t now_us = get_time_us();
int64_t t = libvlc_media_player_get_time(core.mp);

bool timestamp_jump = false;
bool video_stalled = false;

// --- Detect timestamp jump ---
/*if (core.last_time > 0) {
    int64_t diff = t - core.last_time;

    if (diff < -1000 || diff > 2000) {
        timestamp_jump = true;
        fprintf(stderr, "[SYNC] Timestamp jump: %lld ms\n", diff);
    }
}
core.last_time = t;
*/
// --- Detect video stall ---
if (core.last_video_frame_time > 0) {
    int64_t gap_ms = (now_us - core.last_video_frame_time) / 1000;

    if (gap_ms > 300) {
        video_stalled = true;
        fprintf(stderr, "[SYNC] Video stall: %lld ms\n", gap_ms);
    }
}

        // === VIDEO OUTPUT ===
    if (!core.menu_active) {
        const uint32_t *vbuf;
        unsigned vw, vh, vpitch;
        if (vlc_video_get_frame(&vbuf, &vw, &vh, &vpitch)) {
        
            core.last_video_frame_time = get_time_us();
            core.video_frame_seen = true;
            video_cb(vbuf, vw, vh, vpitch);
        }
    } else {
        pthread_mutex_lock(&core.mutex);
        if (core.video_buffer && core.video_width && core.video_height)
            video_cb(core.video_buffer, core.video_width, core.video_height, core.video_pitch);
        pthread_mutex_unlock(&core.mutex);
    }

    // Dual-buffer stitch commit
    if (core.stitch_switch_pending)
        vlc_stitch_try_commit();

  // === AUDIO OUTPUT every frame ===
if (audio_batch_cb) {

    // 🔒 SYNC GATE (THIS is the fix)
    if (core.audio_wait_for_sync)
    {
        int64_t now = get_time_us();

        // Still waiting → flush audio and output silence
        if (now < core.audio_start_time_us)
        {
            vlc_audio_ring_reset();

            static int16_t silence[SAMPLES_PER_FRAME * 2] = {0};
            audio_batch_cb(silence, SAMPLES_PER_FRAME);
            return;
        }

        // Sync point reached → allow audio
        core.audio_wait_for_sync = false;
        vlc_audio_enable();

        fprintf(stderr, "[AUDIO] Sync reached — starting audio\n");
    }

    static int16_t audio_frame[SAMPLES_PER_FRAME * 2];
    vlc_audio_ring_read(audio_frame, SAMPLES_PER_FRAME);
    audio_batch_cb(audio_frame, SAMPLES_PER_FRAME);
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
vlc_menu_deinit();
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