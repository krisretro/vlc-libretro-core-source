#ifndef VLC_YOUTUBE_H
#define VLC_YOUTUBE_H

#include <stdbool.h>

/*
 * vlc_youtube.h
 *
 * Asynchronous YouTube URL resolver using yt-dlp.
 *
 * Because yt-dlp can take 2-10 seconds to resolve a URL, resolution
 * runs on a background thread.  The caller checks resolution status
 * each retro_run tick via vlc_youtube_poll().
 *
 * Typical flow:
 *
 *   // When a YouTube URL is encountered in the playlist:
 *   vlc_youtube_resolve_async(youtube_url, playlist_index);
 *
 *   // Each retro_run tick:
 *   char *resolved = NULL;
 *   int   index    = -1;
 *   vlc_youtube_status_t status = vlc_youtube_poll(&resolved, &index);
 *   if (status == YT_STATUS_DONE) {
 *       // resolved is heap-allocated — caller must free() it
 *       // index is the playlist slot to update
 *       free(core.playlist[index]);
 *       core.playlist[index] = resolved;
 *   } else if (status == YT_STATUS_FAILED) {
 *       // resolution failed — keep original URL or remove from playlist
 *   }
 *   // YT_STATUS_IDLE  — nothing in progress
 *   // YT_STATUS_BUSY  — still resolving, show loading indicator
 */

typedef enum {
    YT_STATUS_IDLE   = 0,  /* no resolution in progress          */
    YT_STATUS_BUSY   = 1,  /* resolution running in background    */
    YT_STATUS_DONE   = 2,  /* resolved URL ready — poll returns it */
    YT_STATUS_FAILED = 3,  /* yt-dlp failed or not installed      */
} vlc_youtube_status_t;

/* Returns true if the URL looks like a YouTube link yt-dlp can handle */
bool vlc_youtube_is_url(const char *url);

/*
 * Start async resolution of a YouTube URL.
 * tag is an arbitrary integer (e.g. playlist index) returned unchanged
 * by vlc_youtube_poll() so the caller knows which slot to update.
 * Returns false if a resolution is already in progress.
 */
bool vlc_youtube_resolve_async(const char *url, int tag);

/*
 * Poll resolution state.  Call once per retro_run tick.
 * When status == YT_STATUS_DONE:
 *   *out_url is a heap-allocated resolved URL — caller must free() it.
 *   *out_tag is the tag passed to vlc_youtube_resolve_async().
 * For all other statuses *out_url and *out_tag are unchanged.
 */
vlc_youtube_status_t vlc_youtube_poll(char **out_url, int *out_tag);

/* Cancel any in-progress resolution and reset to IDLE */
void vlc_youtube_cancel(void);

/* Free resources — call from retro_deinit */
void vlc_youtube_deinit(void);

#endif /* VLC_YOUTUBE_H */
