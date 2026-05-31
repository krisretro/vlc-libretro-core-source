/*
 * vlc_youtube.c
 *
 * Asynchronous YouTube URL resolver using yt-dlp.
 * See vlc_youtube.h for usage.
 *
 * Compile alongside vlc_core.c — no special libraries needed beyond
 * pthreads (already linked for the rest of the core).
 *
 * yt-dlp must be installed and on PATH (or yt-dlp.exe on Windows PATH).
 * Get it from: https://github.com/yt-dlp/yt-dlp/releases
 */

#include "vlc_youtube.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* ── Internal state ────────────────────────────────────────────────────── */

typedef enum {
    STATE_IDLE   = 0,
    STATE_BUSY   = 1,
    STATE_DONE   = 2,
    STATE_FAILED = 3,
} resolver_state_t;

static pthread_mutex_t yt_mtx    = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       yt_thread = 0;
static bool            yt_thread_active = false;

static resolver_state_t yt_state    = STATE_IDLE;
static char            *yt_input    = NULL;   /* URL being resolved (heap) */
static char            *yt_result   = NULL;   /* resolved URL (heap)       */
static int              yt_tag      = -1;     /* caller's tag              */
static bool             yt_cancel   = false;

/* ── URL detection ─────────────────────────────────────────────────────── */

bool vlc_youtube_is_url(const char *url)
{
    if (!url) return false;
    return (strstr(url, "youtube.com/watch")   != NULL ||
            strstr(url, "youtu.be/")           != NULL ||
            strstr(url, "youtube.com/live")    != NULL ||
            strstr(url, "youtube.com/shorts")  != NULL ||
            strstr(url, "youtube.com/embed")   != NULL ||
            strstr(url, "music.youtube.com")   != NULL);
}

/* ── yt-dlp invocation ─────────────────────────────────────────────────── */

/*
 * Strip trailing CR/LF from a string in-place. Returns new length.
 */
static size_t strip_newline(char *s, size_t len)
{
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r'))
        s[--len] = '\0';
    return len;
}

/*
 * Read a full line from a Windows HANDLE into buf (max buf_size bytes).
 * Returns number of bytes read, 0 on EOF/error.
 */
#ifdef _WIN32
static size_t read_line_handle(HANDLE h, char *buf, size_t buf_size)
{
    size_t pos = 0;
    while (pos < buf_size - 1) {
        DWORD got = 0;
        char  ch  = 0;
        if (!ReadFile(h, &ch, 1, &got, NULL) || got == 0)
            break;
        if (ch == '\n') { buf[pos++] = ch; break; }
        buf[pos++] = ch;
    }
    buf[pos] = '\0';
    return pos;
}
#endif

/*
 * Run yt-dlp synchronously and return the resolved URL.
 * Returns heap-allocated string or NULL on failure.
 * Called from the background thread only.
 *
 * On Windows we use CreateProcess with CREATE_NO_WINDOW so no console
 * window flashes up and RetroArch is not blocked by console creation.
 */
static char *run_ytdlp(const char *url)
{
  static const char *fmt = "best[ext=mp4]/best";

    char line1[4096] = {0};
    char line2[4096] = {0};

#ifdef _WIN32
    /*
     * Windows path: use CreateProcess + anonymous pipe so we can read
     * stdout without ever creating a visible console window.
     * popen() on MinGW/MSYS2 implicitly calls AllocConsole which blocks
     * the calling thread until the console is ready — this freezes the
     * RetroArch render loop even though we're on a background thread,
     * because Windows serialises console creation per-process.
     */
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE pipe_rd = NULL, pipe_wr = NULL;

    if (!CreatePipe(&pipe_rd, &pipe_wr, &sa, 0)) {
        fprintf(stderr, "[YT-DLP] CreatePipe failed (%lu)\n", GetLastError());
        return NULL;
    }
    /* Don't inherit the read end in the child */
    SetHandleInformation(pipe_rd, HANDLE_FLAG_INHERIT, 0);

    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
        "yt-dlp.exe -f \"%s\" --get-url --no-playlist --no-warnings \"%s\"",
        fmt, url);

    fprintf(stderr, "[YT-DLP] Running (no-window): %s\n", cmd);

    STARTUPINFOA si = {0};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput  = pipe_wr;
    si.hStdError   = GetStdHandle(STD_ERROR_HANDLE); /* let stderr through */
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessA(
        NULL, cmd, NULL, NULL,
        TRUE,                   /* inherit handles (pipe_wr) */
        CREATE_NO_WINDOW,       /* ← key flag: no console window */
        NULL, NULL, &si, &pi);

    CloseHandle(pipe_wr);       /* child has its own copy now */

    if (!ok) {
        fprintf(stderr, "[YT-DLP] CreateProcess failed (%lu) — is yt-dlp.exe on PATH?\n",
                GetLastError());
        CloseHandle(pipe_rd);
        return NULL;
    }

    /* Read two lines from stdout */
    read_line_handle(pipe_rd, line1, sizeof(line1));
    read_line_handle(pipe_rd, line2, sizeof(line2));

    /* Wait for process to finish then clean up */
    WaitForSingleObject(pi.hProcess, 30000); /* 30s timeout */
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(pipe_rd);

#else
    /* Non-Windows: popen is fine, no console issue */
    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
        "yt-dlp -f '%s' --get-url --no-playlist --no-warnings \"%s\" 2>/dev/null",
        fmt, url);

    fprintf(stderr, "[YT-DLP] Running: %s\n", cmd);

    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        fprintf(stderr, "[YT-DLP] popen failed — is yt-dlp installed?\n");
        return NULL;
    }
    fgets(line1, sizeof(line1), pipe);
    fgets(line2, sizeof(line2), pipe);
    pclose(pipe);
#endif

    size_t l1 = strip_newline(line1, strlen(line1));
    size_t l2 = strip_newline(line2, strlen(line2));

    if (l1 == 0) {
        fprintf(stderr, "[YT-DLP] No URL returned — unavailable or geo-blocked?\n");
        return NULL;
    }

    fprintf(stderr, "[YT-DLP] Video URL: %.80s...\n", line1);

    if (l2 > 0) {
        /* DASH: two streams — return tab-separated so caller can split */
        fprintf(stderr, "[YT-DLP] Audio URL (DASH): %.80s...\n", line2);
        size_t total = l1 + 1 + l2 + 1;
        char *combined = malloc(total);
        if (!combined) return strdup(line1);
        snprintf(combined, total, "%s\t%s", line1, line2);
        return combined;
    }

    return strdup(line1);
}

/* ── Background thread ─────────────────────────────────────────────────── */

static void *resolver_thread(void *arg)
{
    (void)arg;

    pthread_mutex_lock(&yt_mtx);
    char *url = yt_input ? strdup(yt_input) : NULL;
    pthread_mutex_unlock(&yt_mtx);

    if (!url) {
        pthread_mutex_lock(&yt_mtx);
        yt_state = STATE_FAILED;
        yt_thread_active = false;
        pthread_mutex_unlock(&yt_mtx);
        return NULL;
    }

    char *result = NULL;

    pthread_mutex_lock(&yt_mtx);
    bool cancelled = yt_cancel;
    pthread_mutex_unlock(&yt_mtx);

    if (!cancelled) {
        result = run_ytdlp(url);
    }

    free(url);

    pthread_mutex_lock(&yt_mtx);
    yt_thread_active = false;

    if (yt_cancel) {
        free(result);
        yt_state = STATE_IDLE;
    } else if (result) {
        free(yt_result);
        yt_result = result;
        yt_state  = STATE_DONE;
        fprintf(stderr, "[YT-DLP] Resolution complete\n");
    } else {
        yt_state = STATE_FAILED;
        fprintf(stderr, "[YT-DLP] Resolution failed\n");
    }
    pthread_mutex_unlock(&yt_mtx);

    return NULL;
}

/* ── Public API ────────────────────────────────────────────────────────── */

bool vlc_youtube_resolve_async(const char *url, int tag)
{
    pthread_mutex_lock(&yt_mtx);

    if (yt_state == STATE_BUSY) {
        fprintf(stderr, "[YT-DLP] Already resolving — call vlc_youtube_cancel() first\n");
        pthread_mutex_unlock(&yt_mtx);
        return false;
    }

    free(yt_input);
    free(yt_result);
    yt_input  = strdup(url);
    yt_result = NULL;
    yt_tag    = tag;
    yt_cancel = false;
    yt_state  = STATE_BUSY;

    pthread_mutex_unlock(&yt_mtx);

    /* Detach so we don't need to join — poll via yt_state */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    int rc = pthread_create(&yt_thread, &attr, resolver_thread, NULL);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        pthread_mutex_lock(&yt_mtx);
        yt_state = STATE_FAILED;
        pthread_mutex_unlock(&yt_mtx);
        fprintf(stderr, "[YT-DLP] Failed to create resolver thread\n");
        return false;
    }

    yt_thread_active = true;
    fprintf(stderr, "[YT-DLP] Resolving: %s\n", url);
    return true;
}

vlc_youtube_status_t vlc_youtube_poll(char **out_url, int *out_tag)
{
    pthread_mutex_lock(&yt_mtx);
    vlc_youtube_status_t status = (vlc_youtube_status_t)yt_state;
    char *url = NULL;
    int   tag = yt_tag;

    if (status == YT_STATUS_DONE) {
        /* Hand ownership of the result to the caller */
        url       = yt_result;
        yt_result = NULL;
        yt_state  = STATE_IDLE;
    } else if (status == YT_STATUS_FAILED) {
        yt_state = STATE_IDLE;
    }
    pthread_mutex_unlock(&yt_mtx);

    if (out_url) *out_url = url;
    if (out_tag) *out_tag = tag;
    return status;
}

void vlc_youtube_cancel(void)
{
    pthread_mutex_lock(&yt_mtx);
    yt_cancel = true;
    yt_state  = STATE_IDLE;
    pthread_mutex_unlock(&yt_mtx);
    fprintf(stderr, "[YT-DLP] Resolution cancelled\n");
}

void vlc_youtube_deinit(void)
{
    vlc_youtube_cancel();

    pthread_mutex_lock(&yt_mtx);
    free(yt_input);  yt_input  = NULL;
    free(yt_result); yt_result = NULL;
    pthread_mutex_unlock(&yt_mtx);
}