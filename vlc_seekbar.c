/*
 * vlc_seekbar.c
 *
 * Renders a seek bar overlay into a SEPARATE composite buffer which is
 * then blended onto the video frame only at display time.  This prevents
 * flicker caused by new video frames overwriting seekbar pixels between
 * retro_run ticks.
 *
 * Usage in vlc_core.c:
 *
 *   #include "vlc_seekbar.c"
 *
 *   // Call once when video dimensions are known / change:
 *   vlc_seekbar_resize(video_width, video_height);
 *
 *   // Each retro_run tick while scrubbing or fading:
 *   vlc_seekbar_update(scrub_pos_ms, duration_ms, scrubbing);
 *
 *   // After vlc_video_get_frame, before video_cb:
 *   //   composite seekbar onto the frame then present it
 *   const uint32_t *out = vlc_seekbar_composite(vbuf, vw, vh, vpitch_bytes);
 *   video_cb(out, vw, vh, vpitch_bytes);
 *
 *   // vlc_seekbar_composite returns vbuf unchanged if seekbar is invisible,
 *   // so there is no cost when not seeking.
 *
 *   // On cleanup:
 *   vlc_seekbar_free();
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── Tunables ──────────────────────────────────────────────────────────── */
#define SEEKBAR_FADE_TICKS     90     /* frames to fade after scrub ends    */
#define BAR_HEIGHT_FRAC        0.012f /* bar height as fraction of frame h  */
#define BAR_BOTTOM_FRAC        0.10f  /* distance from bottom as fraction   */
#define BAR_MARGIN_FRAC        0.06f  /* left/right margin as fraction of w */
#define PANEL_PADDING_FRAC     0.03f  /* padding inside background panel    */

/* Colours — 0xRRGGBB (alpha handled separately) */
#define COL_TRACK       0x303030u
#define COL_ELAPSED     0xE0E0E0u
#define COL_THUMB       0xFFFFFFu
#define COL_THUMB_INNER 0xFF3333u
#define COL_PANEL       0x0A0A0Au
#define COL_TIME_CURR   0xFFFFFFu
#define COL_TIME_DUR    0x909090u
#define COL_SCRUB_GLOW  0xFF4444u

/* ── Internal state ────────────────────────────────────────────────────── */
static uint32_t *sb_buf       = NULL; /* composite working buffer           */
static unsigned  sb_w         = 0;
static unsigned  sb_h         = 0;
static int       sb_fade      = 0;   /* remaining fade ticks               */
static bool      sb_scrubbing = false;
static int64_t   sb_pos_ms    = 0;
static int64_t   sb_dur_ms    = 0;

/* ── Tiny 5×7 pixel font — digits 0-9 and colon ───────────────────────── */
static const uint8_t font5x7[11][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
    {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, /* 2 */
    {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}, /* 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
    {0x00,0x04,0x04,0x00,0x04,0x04,0x00}, /* : */
};

/* ── Pixel helpers ─────────────────────────────────────────────────────── */

/* Write a pre-multiplied colour into sb_buf with alpha blend */
static inline void sb_put(int x, int y, uint32_t rgb, uint8_t a)
{
    if (x < 0 || y < 0 || (unsigned)x >= sb_w || (unsigned)y >= sb_h) return;
    uint32_t *p  = sb_buf + (unsigned)y * sb_w + (unsigned)x;
    uint32_t dst = *p;

    uint32_t sr = (rgb >> 16) & 0xFF;
    uint32_t sg = (rgb >>  8) & 0xFF;
    uint32_t sb = (rgb      ) & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >>  8) & 0xFF;
    uint32_t db = (dst      ) & 0xFF;

    uint32_t ia = 255 - a;
    uint32_t r  = (sr * a + dr * ia) / 255;
    uint32_t g  = (sg * a + dg * ia) / 255;
    uint32_t b  = (sb * a + db * ia) / 255;

    *p = (r << 16) | (g << 8) | b;
}

static void sb_fill_rect(int x, int y, int w, int h, uint32_t rgb, uint8_t a)
{
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            sb_put(col, row, rgb, a);
}

static void sb_fill_circle(int cx, int cy, int r, uint32_t rgb, uint8_t a)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r)
                sb_put(cx+dx, cy+dy, rgb, a);
}

/* Anti-aliased circle edge — 1px soft ring outside the hard fill */
static void sb_circle_aa(int cx, int cy, int r, uint32_t rgb, uint8_t a)
{
    sb_fill_circle(cx, cy, r, rgb, a);
    float rf = (float)r + 0.5f;
    for (int dy = -(r+1); dy <= r+1; dy++) {
        for (int dx = -(r+1); dx <= r+1; dx++) {
            float d = sqrtf((float)(dx*dx + dy*dy));
            if (d > (float)r && d <= rf) {
                uint8_t aa = (uint8_t)((rf - d) * (float)a);
                sb_put(cx+dx, cy+dy, rgb, aa);
            }
        }
    }
}

/* Rounded rectangle */
static void sb_fill_rrect(int x, int y, int w, int h, int r,
                           uint32_t rgb, uint8_t a)
{
    if (r > h/2) r = h/2;
    if (r > w/2) r = w/2;
    sb_fill_rect(x+r, y, w-2*r, h, rgb, a);
    sb_fill_rect(x, y+r, r, h-2*r, rgb, a);
    sb_fill_rect(x+w-r, y+r, r, h-2*r, rgb, a);
    sb_fill_circle(x+r,   y+r,   r, rgb, a);
    sb_fill_circle(x+w-r, y+r,   r, rgb, a);
    sb_fill_circle(x+r,   y+h-r, r, rgb, a);
    sb_fill_circle(x+w-r, y+h-r, r, rgb, a);
}

/* ── Font rendering ────────────────────────────────────────────────────── */

static int sb_glyph_w(int scale) { return 5*scale + scale; }

static void sb_draw_char(int px, int py, int ch, int scale,
                          uint32_t rgb, uint8_t a)
{
    int idx = (ch >= '0' && ch <= '9') ? ch - '0'
            : (ch == ':')              ? 10
            : -1;
    if (idx < 0) return;
    const uint8_t *g = font5x7[idx];
    for (int row = 0; row < 7; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 5; col++) {
            if (bits & (1 << (4-col))) {
                sb_fill_rect(px + col*scale, py + row*scale,
                             scale, scale, rgb, a);
            }
        }
    }
}

static int sb_str_width(const char *s, int scale)
{
    int w = 0;
    for (; *s; s++) w += sb_glyph_w(scale);
    return w;
}

static void sb_draw_str(int px, int py, const char *s, int scale,
                         uint32_t rgb, uint8_t a)
{
    for (; *s; s++) {
        sb_draw_char(px, py, *s, scale, rgb, a);
        px += sb_glyph_w(scale);
    }
}

static void sb_fmt_time(char *out, size_t sz, int64_t ms)
{
    if (ms < 0) ms = 0;
    int64_t ts = ms / 1000;
    int64_t h  = ts / 3600; ts %= 3600;
    int64_t m  = ts / 60;   ts %= 60;
    int64_t s  = ts;
    if (h > 0)
        snprintf(out, sz, "%lld:%02lld:%02lld",
                 (long long)h, (long long)m, (long long)s);
    else
        snprintf(out, sz, "%02lld:%02lld", (long long)m, (long long)s);
}

/* ── Internal draw ─────────────────────────────────────────────────────── */

static void sb_render(uint8_t alpha)
{
    if (!sb_buf || sb_w == 0 || sb_h == 0 || alpha == 0) return;

    /* Clear composite buffer to transparent black */
    memset(sb_buf, 0, sb_w * sb_h * sizeof(uint32_t));

    float frac = 0.0f;
    if (sb_dur_ms > 0 && sb_pos_ms >= 0)
        frac = (float)sb_pos_ms / (float)sb_dur_ms;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    int margin   = (int)((float)sb_w * BAR_MARGIN_FRAC);
    int bar_h    = (int)((float)sb_h * BAR_HEIGHT_FRAC);
    if (bar_h < 4)  bar_h = 4;
    if (bar_h > 20) bar_h = 20;

    int bar_w    = (int)sb_w - 2 * margin;
    int bar_y    = (int)sb_h - (int)((float)sb_h * BAR_BOTTOM_FRAC);
    int bar_x    = margin;
    int fill_w   = (int)((float)bar_w * frac);
    int thumb_cx = bar_x + fill_w;
    int thumb_cy = bar_y + bar_h / 2;
    int thumb_r  = bar_h + bar_h / 2;   /* thumb larger than bar           */
    int bar_r    = bar_h / 2;           /* pill ends                       */

    /* font scale relative to bar height */
    int fscale   = bar_h / 5;
    if (fscale < 1) fscale = 1;
    if (fscale > 5) fscale = 5;
    int fh       = 7 * fscale;

    char pos_str[16], dur_str[16];
    sb_fmt_time(pos_str, sizeof(pos_str), sb_pos_ms);
    sb_fmt_time(dur_str, sizeof(dur_str), sb_dur_ms);

    int pos_w  = sb_str_width(pos_str, fscale);
    int dur_w  = sb_str_width(dur_str, fscale);
    int pad    = (int)((float)sb_w * PANEL_PADDING_FRAC);

    /* ── Background panel ── */
    int panel_y  = bar_y - thumb_r - fh - pad * 3;
    int panel_h  = (bar_y + thumb_r + pad) - panel_y;
    int panel_x  = bar_x - pad;
    int panel_w  = bar_w + pad * 2;
    if (panel_y < 0) panel_y = 0;

    sb_fill_rrect(panel_x, panel_y, panel_w, panel_h, pad,
                  COL_PANEL, (uint8_t)(210 * alpha / 255));

    /* ── Track: empty portion ── */
    sb_fill_rrect(bar_x, bar_y, bar_w, bar_h, bar_r,
                  COL_TRACK, (uint8_t)(255 * alpha / 255));

    /* ── Track: elapsed portion ── */
    if (fill_w > 0) {
        int ew = fill_w < bar_r*2 ? fill_w : fill_w;
        sb_fill_rrect(bar_x, bar_y, ew, bar_h, bar_r,
                      COL_ELAPSED, (uint8_t)(255 * alpha / 255));
    }

    /* ── Thumb glow (soft halo behind thumb) ── */
    sb_fill_circle(thumb_cx, thumb_cy, thumb_r + thumb_r/2,
                   COL_SCRUB_GLOW, (uint8_t)(40 * alpha / 255));
    sb_fill_circle(thumb_cx, thumb_cy, thumb_r + thumb_r/3,
                   COL_SCRUB_GLOW, (uint8_t)(60 * alpha / 255));

    /* ── Thumb: white circle with red inner dot ── */
    sb_circle_aa(thumb_cx, thumb_cy, thumb_r,
                 COL_THUMB, (uint8_t)(255 * alpha / 255));
    sb_fill_circle(thumb_cx, thumb_cy, thumb_r / 3,
                   COL_THUMB_INNER, (uint8_t)(255 * alpha / 255));

    /* ── Time labels ── */
    int label_y = bar_y - thumb_r - fh - pad;

    /* Current position — centred on thumb, clamped to panel */
    int pos_x = thumb_cx - pos_w / 2;
    if (pos_x < panel_x + pad)              pos_x = panel_x + pad;
    if (pos_x + pos_w > panel_x + panel_w - pad)
        pos_x = panel_x + panel_w - pad - pos_w;

    sb_draw_str(pos_x, label_y, pos_str, fscale,
                COL_TIME_CURR, (uint8_t)(255 * alpha / 255));

    /* Duration — right-aligned inside panel */
    if (sb_dur_ms > 0) {
        int dur_x = panel_x + panel_w - pad - dur_w;
        /* Only draw if it won't overlap the position label */
        if (dur_x > pos_x + pos_w + pad * 2) {
            sb_draw_str(dur_x, label_y, dur_str, fscale,
                        COL_TIME_DUR, (uint8_t)(180 * alpha / 255));

            /* Divider dot between pos and duration */
            int dot_x = pos_x + pos_w + (dur_x - pos_x - pos_w) / 2;
            int dot_y = label_y + fh / 2;
            sb_fill_circle(dot_x, dot_y, fscale,
                           COL_TIME_DUR, (uint8_t)(120 * alpha / 255));
        }
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

void vlc_seekbar_free(void)
{
    free(sb_buf);
    sb_buf = NULL;
    sb_w = sb_h = 0;
}

void vlc_seekbar_resize(unsigned w, unsigned h)
{
    if (w == sb_w && h == sb_h) return;
    free(sb_buf);
    sb_buf = (uint32_t *)calloc(w * h, sizeof(uint32_t));
    if (!sb_buf) { sb_w = sb_h = 0; return; }
    sb_w = w;
    sb_h = h;
}

/*
 * vlc_seekbar_update — call each retro_run tick.
 * scrubbing=true resets the fade timer and renders at full opacity.
 * scrubbing=false decrements the fade counter.
 */
void vlc_seekbar_update(int64_t pos_ms, int64_t dur_ms, bool scrubbing)
{
    sb_pos_ms    = pos_ms;
    sb_dur_ms    = dur_ms;
    sb_scrubbing = scrubbing;

    if (scrubbing) {
        sb_fade = SEEKBAR_FADE_TICKS;
    } else if (sb_fade > 0) {
        sb_fade--;
    }

    /* Render into composite buffer now so composite() is just a blit */
    if (sb_fade > 0 || scrubbing) {
        uint8_t alpha = scrubbing
            ? 255
            : (uint8_t)(sb_fade * 255 / SEEKBAR_FADE_TICKS);
        sb_render(alpha);
    }
}

/*
 * vlc_seekbar_composite — blend the seekbar onto a video frame.
 *
 * Returns a pointer to a composited buffer when the seekbar is visible,
 * or src unchanged when it is not (zero overhead during normal playback).
 *
 * The returned pointer is valid until the next call to vlc_seekbar_composite
 * or vlc_seekbar_free.  Do not free it.
 *
 * pitch_bytes is the row stride of src in bytes.
 */
const uint32_t *vlc_seekbar_composite(const uint32_t *src,
                                       unsigned w, unsigned h,
                                       unsigned pitch_bytes)
{
    if (!sb_buf || (sb_fade <= 0 && !sb_scrubbing)) return src;
    if (w != sb_w || h != sb_h) return src;

    unsigned pitch_px = pitch_bytes / 4;

    /* Blend sb_buf (pre-rendered overlay) onto src into a static output buf.
     * We use sb_buf itself as the output to avoid a third allocation —
     * re-render will overwrite it next tick anyway. */
    static uint32_t *out_buf  = NULL;
    static unsigned  out_size = 0;
    unsigned needed = w * h;

    if (needed > out_size) {
        free(out_buf);
        out_buf  = (uint32_t *)malloc(needed * sizeof(uint32_t));
        out_size = needed;
        if (!out_buf) { out_size = 0; return src; }
    }

    for (unsigned row = 0; row < h; row++) {
        const uint32_t *src_row = src    + row * pitch_px;
        const uint32_t *ov_row  = sb_buf + row * w;
              uint32_t *dst_row = out_buf + row * w;

        for (unsigned col = 0; col < w; col++) {
            uint32_t ov  = ov_row[col];
            uint32_t s   = src_row[col];

            /* overlay pixels are pre-blended against black in sb_render;
             * treat their luminance as alpha: if overlay pixel is black,
             * pass video through. Use simple max-channel as opacity hint. */
            uint32_t or_ = (ov >> 16) & 0xFF;
            uint32_t og  = (ov >>  8) & 0xFF;
            uint32_t ob  = (ov      ) & 0xFF;
            uint32_t oa  = or_ > og ? or_ : og;
            if (ob > oa) oa = ob;

            if (oa == 0) {
                dst_row[col] = s;
            } else if (oa == 255) {
                dst_row[col] = ov;
            } else {
                uint32_t ia  = 255 - oa;
                uint32_t sr  = (s >> 16) & 0xFF;
                uint32_t sg  = (s >>  8) & 0xFF;
                uint32_t sb_ = (s      ) & 0xFF;
                uint32_t r   = (or_*oa + sr*ia) / 255;
                uint32_t g   = (og*oa + sg*ia) / 255;
                uint32_t b   = (ob*oa + sb_*ia) / 255;
                dst_row[col] = (r << 16) | (g << 8) | b;
            }
        }
    }

    return out_buf;
}