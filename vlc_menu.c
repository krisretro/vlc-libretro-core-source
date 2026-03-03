#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "vlc_core.h"
#include "libretro.h"
#define FONT_SCALE 2   // Makes text 16x16 pixels (adjust as needed)

#define VISIBLE_ITEMS 22   // Number of items visible at once (adjust for your line spacing)
extern retro_input_state_t input_state_cb;

// Forward declaration
static void build_current_group_items(void);


// 8x8 bitmap font (standard in many cores)
static const uint8_t font8x8[96][8] = {
    {0,0,0,0,0,0,0,0}, {0x18,0x3C,0x3C,0x18,0x18,0x0,0x18,0x0}, {0x6C,0x6C,0x6C,0x0,0x0,0x0,0x0,0x0}, {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x0},
    {0x18,0x7E,0xC0,0x7C,0x6,0xFC,0x18,0x0}, {0x0,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x0}, {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x0}, {0x18,0x18,0x30,0x0,0x0,0x0,0x0,0x0},
    {0xC,0x18,0x30,0x30,0x30,0x18,0xC,0x0}, {0x30,0x18,0xC,0xC,0xC,0x18,0x30,0x0}, {0x0,0x66,0x3C,0xFF,0x3C,0x66,0x0,0x0}, {0x0,0x18,0x18,0x7E,0x18,0x18,0x0,0x0},
    {0x0,0x0,0x0,0x0,0x0,0x18,0x18,0x30}, {0x0,0x0,0x0,0x7E,0x0,0x0,0x0,0x0}, {0x0,0x0,0x0,0x0,0x0,0x18,0x18,0x0}, {0x6,0xC,0x18,0x30,0x60,0xC0,0x80,0x0},
    {0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x0}, {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x0}, {0x7C,0xC6,0x6,0x3C,0x60,0xC0,0xFE,0x0}, {0x7C,0xC6,0x6,0x3C,0x6,0xC6,0x7C,0x0},
    {0x1C,0x3C,0x6C,0xCC,0xFE,0xC,0x1E,0x0}, {0xFE,0xC0,0xF8,0x6,0x6,0xC6,0x7C,0x0}, {0x38,0x60,0xC0,0xF8,0xC6,0xC6,0x7C,0x0}, {0xFE,0xC6,0xC,0x18,0x30,0x30,0x30,0x0},
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x0}, {0x7C,0xC6,0xC6,0x7E,0x6,0xC,0x78,0x0}, {0x0,0x18,0x18,0x0,0x18,0x18,0x0,0x0}, {0x0,0x18,0x18,0x0,0x18,0x18,0x30,0x0},
    {0x18,0x30,0x60,0xC0,0x60,0x30,0x18,0x0}, {0x0,0x0,0x7E,0x0,0x7E,0x0,0x0,0x0}, {0x60,0x30,0x18,0xC,0x18,0x30,0x60,0x0}, {0x7C,0xC6,0xC,0x18,0x18,0x0,0x18,0x0},
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x0}, {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x0}, {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x0}, {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x0},
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x0}, {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x0}, {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x0}, {0x3C,0x66,0xC0,0xCE,0xC6,0x66,0x3E,0x0},
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x0}, {0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x0}, {0x1E,0xC,0xC,0xC,0xC,0xCC,0x78,0x0}, {0xC6,0xCC,0xD8,0xF0,0xD8,0xCC,0xC6,0x0},
    {0xF0,0x60,0x60,0x60,0x60,0x62,0xFE,0x0}, {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x0}, {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x0}, {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x0},
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x0}, {0x7C,0xC6,0xC6,0xC6,0xDE,0xCC,0x76,0x0}, {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xC6,0x0}, {0x7C,0xC6,0x60,0x38,0x6,0xC6,0x7C,0x0},
    {0x7E,0x5A,0x18,0x18,0x18,0x18,0x3C,0x0}, {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x0}, {0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x0}, {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x0},
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x0}, {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x0}, {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x0}, {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x0},
    {0xC0,0x60,0x30,0x18,0xC,0x6,0x2,0x0}, {0x3C,0xC,0xC,0xC,0xC,0xC,0x3C,0x0}, {0x10,0x38,0x6C,0xC6,0x0,0x0,0x0,0x0}, {0x0,0x0,0x0,0x0,0x0,0x0,0x0,0xFF},
    {0x30,0x30,0x18,0x0,0x0,0x0,0x0,0x0}, {0x0,0x0,0x78,0xC,0x7C,0xCC,0x76,0x0}, {0xE0,0x60,0x7C,0x66,0x66,0x66,0x7C,0x0}, {0x0,0x0,0x7C,0xC6,0xC0,0xC6,0x7C,0x0},
    {0x1C,0xC,0x7C,0xCC,0xCC,0xCC,0x76,0x0}, {0x0,0x0,0x7C,0xC6,0xFE,0xC0,0x7C,0x0}, {0x3C,0x66,0x60,0xF8,0x60,0x60,0xF0,0x0}, {0x0,0x0,0x76,0xCC,0xCC,0x7C,0xC,0xF8},
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0x66,0x0}, {0x18,0x0,0x38,0x18,0x18,0x18,0x3C,0x0}, {0x6,0x0,0x6,0x6,0x6,0x66,0x3C,0x0}, {0xE0,0x60,0x66,0x6C,0x78,0x6C,0x66,0x0},
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x0}, {0x0,0x0,0xEC,0xFE,0xD6,0xD6,0xD6,0x0}, {0x0,0x0,0xDC,0x66,0x66,0x66,0x66,0x0}, {0x0,0x0,0x7C,0xC6,0xC6,0xC6,0x7C,0x0},
    {0x0,0x0,0x7C,0x66,0x66,0x7C,0x60,0xF0}, {0x0,0x0,0x76,0xCC,0xCC,0x7C,0xC,0x1E}, {0x0,0x0,0xDC,0x76,0x60,0x60,0xF0,0x0}, {0x0,0x0,0x7E,0xC0,0x7C,0x6,0xFC,0x0},
    {0x30,0x30,0xFC,0x30,0x30,0x34,0x18,0x0}, {0x0,0x0,0xCC,0xCC,0xCC,0xCC,0x76,0x0}, {0x0,0x0,0xC6,0xC6,0xC6,0x6C,0x38,0x0}, {0x0,0x0,0xC6,0xD6,0xFE,0xEE,0xC6,0x0},
    {0x0,0x0,0xC6,0x6C,0x38,0x6C,0xC6,0x0}, {0x0,0x0,0xC6,0xC6,0xC6,0x7E,0x6,0xFC}, {0x0,0x0,0xFE,0x6C,0x38,0x64,0xFE,0x0}, {0xE,0x18,0x18,0x70,0x18,0x18,0xE,0x0},
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x0}, {0x70,0x18,0x18,0xE,0x18,0x18,0x70,0x0}, {0x76,0xDC,0x0,0x0,0x0,0x0,0x0,0x0}, {0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0}
};

typedef struct {
    char *title;
    char *path;
    char *group;
    char *logo;          // ← NEW: tvg-logo support
} playlist_item_t;

static playlist_item_t *all_items = NULL;
static int total_items = 0;

static char **groups = NULL;
static int group_count = 0;

static playlist_item_t *current_group_items = NULL;
static int current_group_item_count = 0;

static int current_group_idx = 0;
static int current_selection = 0;
static bool in_submenu = false;

static void free_menu_data(void) {
    for (int i = 0; i < total_items; i++) {
        free(all_items[i].title);
        free(all_items[i].path);
        free(all_items[i].group);
        free(all_items[i].logo);
    }
    free(all_items);
    for (int i = 0; i < group_count; i++) free(groups[i]);
    free(groups);
    free(current_group_items);
    all_items = NULL; groups = NULL; current_group_items = NULL;
    total_items = group_count = current_group_item_count = 0;
}

static char* extract_field(const char *extinf, const char *key) {
    char *start = strstr(extinf, key);
    if (!start) return NULL;
    start += strlen(key);
    char *end = strchr(start, '"');
    if (!end) return NULL;
    size_t len = end - start;
    char *val = malloc(len + 1);
    strncpy(val, start, len);
    val[len] = '\0';
    return val;
}

static char* extract_title(const char *extinf) {
    char *tvg = extract_field(extinf, "tvg-name=\"");
    if (tvg) return tvg;

    char *comma = strrchr(extinf, ',');
    if (comma && *(comma+1)) return strdup(comma + 1);

    return strdup("Unknown Channel");
}

bool vlc_menu_init(const char *m3u_path) {
    free_menu_data();

    FILE *f = fopen(m3u_path, "r");
    if (!f) return false;

    char line[4096];
    char current_group[256] = "Ungrouped";
    char current_title[256] = "Unknown";

    int capacity = 64;
    all_items = malloc(capacity * sizeof(playlist_item_t));
    total_items = 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = line; while (isspace((unsigned char)*p)) p++;
        if (!*p) continue;

        if (strncmp(p, "#EXTINF:", 8) == 0) {
            char *group_start = strstr(p, "group-title=\"");
            if (group_start) {
                group_start += 13;
                char *end = strchr(group_start, '"');
               if (end) {
    size_t len = end - group_start;
    if (len >= sizeof(current_group)) len = sizeof(current_group) - 1;
    memcpy(current_group, group_start, len);
    current_group[len] = '\0';
}
            }
            char *title = extract_title(p);
            snprintf(current_title, sizeof(current_title), "%s", title);
            free(title);
        } 
        else if (*p != '#') {
            // path + logo parsing
            char full[4096];
            if (strstr(p, "://")) snprintf(full, sizeof(full), "%s", p);
            else {
                char basedir[4096] = {0};
                snprintf(basedir, sizeof(basedir), "%s", m3u_path);
char *slash = strrchr(basedir, '/');
if (slash) *(slash+1) = '\0';
else basedir[0] = '\0';
            }
            char *nl = strchr(full, '\n'); if (nl) *nl = '\0';

            if (total_items >= capacity) {
                capacity *= 2;
                all_items = realloc(all_items, capacity * sizeof(playlist_item_t));
            }
            playlist_item_t *it = &all_items[total_items++];
            it->title = strdup(current_title);
            it->path  = strdup(full);
            it->group = strdup(current_group);
            it->logo  = extract_field(line, "tvg-logo=\"");   // ← parsed!
            if (!it->logo) it->logo = strdup("");
        }
    }
    fclose(f);

    if (total_items == 0) {
        free_menu_data();
        return false;
    }

    // Redirect detection (unchanged)
    if (total_items == 1 && strstr(all_items[0].path, "://")) {
        const char *p = all_items[0].path;
        if (strcasecmp(p + strlen(p) - 4, ".m3u") == 0 || strcasecmp(p + strlen(p) - 5, ".m3u8") == 0) {
            fprintf(stderr, "[VLC] Redirect M3U detected – falling back\n");
            free_menu_data();
            return false;
        }
    }

    // Build groups (unchanged)
    groups = malloc(total_items * sizeof(char*));
    group_count = 0;
    for (int i = 0; i < total_items; i++) {
        bool exists = false;
        for (int j = 0; j < group_count; j++) {
            if (strcmp(groups[j], all_items[i].group) == 0) { exists = true; break; }
        }
        if (!exists) groups[group_count++] = strdup(all_items[i].group);
    }

    core.menu_active = true;
    in_submenu = false;
    current_selection = 0;
    fprintf(stderr, "[VLC] IPTV Menu activated – %d groups, %d channels\n", group_count, total_items);
    return true;
}

static void build_current_group_items(void) {
    // Free previous items (pointers only, not the data itself)
    if (current_group_items) {
        free(current_group_items);
        current_group_items = NULL;
    }
    current_group_item_count = 0;

    const char *gname = groups[current_group_idx];
    // Count items in this group
    for (int i = 0; i < total_items; i++) {
        if (strcmp(all_items[i].group, gname) == 0)
            current_group_item_count++;
    }
    if (current_group_item_count == 0) return;

    // Allocate array of pointers (no deep copy)
    current_group_items = malloc(current_group_item_count * sizeof(playlist_item_t));
    int idx = 0;
    for (int i = 0; i < total_items; i++) {
        if (strcmp(all_items[i].group, gname) == 0) {
            current_group_items[idx++] = all_items[i];
        }
    }
}

void vlc_menu_handle_input(void) {
    // (input logic exactly as before – no change)
    bool up    = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
    bool down  = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
    bool a     = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
    bool b     = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
    static bool prev_up = false, prev_down = false, prev_a = false, prev_b = false;

    if (!core.menu_active) return;

    int max = in_submenu ? current_group_item_count : group_count;

    if (up && !prev_up)    current_selection = (current_selection - 1 + max) % max;
    if (down && !prev_down) current_selection = (current_selection + 1) % max;

    if (a && !prev_a) {
        if (!in_submenu) {
            current_group_idx = current_selection;
            build_current_group_items();
            in_submenu = true;
            current_selection = 0;
        } else {
            core.menu_active = false;
            switch_to_media(current_group_items[current_selection].path);
        }
    }

    if (b && !prev_b && in_submenu) {
        in_submenu = false;
        free(current_group_items);
        current_group_items = NULL;
        current_selection = current_group_idx;
    }

    prev_up = up; prev_down = down; prev_a = a; prev_b = b;
}

// ==================== GRAPHICAL DRAWING ====================

static void draw_pixel(uint32_t *buf, unsigned pitch, int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x >= 1280 || y >= 720) return;
    buf[y * (pitch/4) + x] = color;
}
static void draw_filled_rect(uint32_t *buf, unsigned pitch, int x, int y, int w, int h, uint32_t color) {
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            draw_pixel(buf, pitch, x + col, y + row, color);
        }
    }
}
static void draw_char(uint32_t *buf, unsigned pitch, int x, int y, char c, uint32_t color) {
    if (c < 32 || c > 127) c = ' ';
    const uint8_t *glyph = font8x8[c - 32];
    for (int py = 0; py < 8; py++) {
        for (int px = 0; px < 8; px++) {
            if (glyph[py] & (1 << (7-px))) {
                // Draw a block of FONT_SCALE x FONT_SCALE pixels
                for (int dy = 0; dy < FONT_SCALE; dy++) {
                    for (int dx = 0; dx < FONT_SCALE; dx++) {
                        draw_pixel(buf, pitch, x + px*FONT_SCALE + dx, y + py*FONT_SCALE + dy, color);
                    }
                }
            }
        }
    }
}

static void draw_string(uint32_t *buf, unsigned pitch, int x, int y, const char *str, uint32_t color) {
    while (*str) {
        draw_char(buf, pitch, x, y, *str++, color);
        x += 8 * FONT_SCALE;
    }
}



void vlc_menu_draw(void) {
    pthread_mutex_lock(&core.mutex);
    if (!core.video_buffer || core.video_width < 640 || core.video_height < 480) {
        pthread_mutex_unlock(&core.mutex);
        return;
    }

    uint32_t *buf = core.video_buffer;
    unsigned pitch = core.video_pitch;

    // Dark background
    for (size_t i = 0; i < (pitch/4) * core.video_height; i++) buf[i] = 0xFF1A1A1A;

    // Header
    draw_string(buf, pitch, 40, 40, "IPTV MENU", 0xFFFFFFFF);

   if (!in_submenu) {
    draw_string(buf, pitch, 40, 80, "Select Group", 0xFFAAAAAA);
    // Calculate visible range
    int start = current_selection - VISIBLE_ITEMS / 2;
    if (start < 0) start = 0;
    if (start + VISIBLE_ITEMS > group_count)
        start = group_count - VISIBLE_ITEMS;
    if (start < 0) start = 0;   // when group_count < VISIBLE_ITEMS

for (int i = 0; i < VISIBLE_ITEMS && (start + i) < group_count; i++) {
    int idx = start + i;
    int y = 120 + i * 24;
    // Clear the whole line area (from left margin to right edge)
    draw_filled_rect(buf, pitch, 40, y, 1200, 16, 0xFF1A1A1A);
    draw_string(buf, pitch, 60, y, groups[idx], (idx == current_selection) ? 0xFFFF0000 : 0xFFFFFFFF);
    if (idx == current_selection)
        draw_string(buf, pitch, 40, y, "→", 0xFFFF0000);
}
} else {
    draw_string(buf, pitch, 40, 80, "Channels in group:", 0xFFAAAAAA);
    char header[128];
    snprintf(header, sizeof(header), "Group: %s", groups[current_group_idx]);
    draw_string(buf, pitch, 40, 100, header, 0xFF00FF00);

    // Visible range
    int start = current_selection - VISIBLE_ITEMS / 2;
    if (start < 0) start = 0;
    if (start + VISIBLE_ITEMS > current_group_item_count)
        start = current_group_item_count - VISIBLE_ITEMS;
    if (start < 0) start = 0;

    for (int i = 0; i < VISIBLE_ITEMS && (start + i) < current_group_item_count; i++) {
    int idx = start + i;
    int y = 140 + i * 24;
    // Clear the line
    draw_filled_rect(buf, pitch, 40, y, 1200, 16, 0xFF1A1A1A);
    char line[256];
    snprintf(line, sizeof(line), "%s%s",
             (idx == current_selection) ? "→ " : "  ",
             current_group_items[idx].title);
    draw_string(buf, pitch, 60, y, line, (idx == current_selection) ? 0xFFFF0000 : 0xFFFFFFFF);
    if (current_group_items[idx].logo && current_group_items[idx].logo[0]) {
        draw_string(buf, pitch, 400, y, "[LOGO]", 0xFF00AAFF);
    }
}
}

    // Bottom bar
    draw_string(buf, pitch, 40, 680, "Select=Close Menu   A=Play   B=Back   Up/Down=Navigate", 0xFF888888);

    pthread_mutex_unlock(&core.mutex);
}
void vlc_menu_deinit(void) {
    free_menu_data();
}