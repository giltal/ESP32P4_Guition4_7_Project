/*
 * Guition 4.3 Demo — the teaching demo from the Guition P4 firmware, as a launcher app.
 *
 * The firmware this comes from (https://github.com/giltal/ESP32P4_Guition4_7_Project) keeps one
 * control state — mode, level, indicator, message — and lets you change it from the screen, a web
 * page and Bluetooth, so students can watch one value move between three surfaces. A PAPP has no
 * networking, so this app keeps the state and the screen, adds the SD card as the surface that
 * outlives a reboot, and explains the rest on the About tab.
 *
 * Touch: the tabs along the top; X (top right) closes the app.
 * Pad:   L1 / R1 or LEFT / RIGHT change tab - except on Input, which shows those
 *        buttons rather than acting on them; START opens the menu (tabs, Exit).
 *   Control   UP / DOWN level, A indicator, X mode, Y message
 * A USB keyboard types into the message on the Control tab.
 * State lives in /sd/data/gil.guition43/state.txt, written 2 s after the last change.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "papp/papp.h"
#include "ui_font.h"

PAPP_MANIFEST(.id = "gil.guition43", .name = "Guition 4.3 Demo", .version = PAPP_APP_VERSION,
              .main_stack = 16384);

#define W     800
#define H     480
#define BAR   72                  /* tab bar height */

#define DATA_DIR "/sd/data/gil.guition43"
#define STATE    DATA_DIR "/state.txt"
#define SAVE_QUIET_US 2000000     /* write 2 s after the last change, as the firmware does */

#define RGB(h) PAPP_RGB565(((h) >> 16) & 255, ((h) >> 8) & 255, (h) & 255)
#define C_BG      RGB(0x081226)
#define C_PANEL   RGB(0x0E1A30)
#define C_RAISED  RGB(0x16274A)
#define C_BORDER  RGB(0x24406E)
#define C_BLUE    RGB(0x1E6FFF)
#define C_TEXT    RGB(0xFFFFFF)
#define C_DIM     RGB(0x8FA3BF)
#define C_FAINT   RGB(0x4A5C7A)
#define C_GREEN   RGB(0x3DDC84)
#define C_ORANGE  RGB(0xF5A623)
#define C_RED     RGB(0xD9453B)

enum { T_HOME, T_CONTROL, T_INPUT, T_ABOUT, T_N };
static const char *const k_tab[T_N] = { "Home", "Control", "Input", "About" };

enum { M_IDLE, M_RUN, M_TEST, M_N };
static const char *const k_mode[M_N] = { "Idle", "Run", "Test" };
static const uint16_t k_mode_col[M_N] = { C_FAINT, C_GREEN, C_ORANGE };

/* Preset messages, cycled with the Next button (Y on the pad) */
static const char *const k_msg[] = { "Hello from the P4", "Lab bench ready", "Experiment running",
                                     "Logging to SD", "Students welcome" };
#define MSGS ((int)(sizeof(k_msg) / sizeof(k_msg[0])))

/* ── The shared control state ───────────────────────────────────────────── */

static int  s_tab;
static int  s_mode;
static int  s_level = 50;
static bool s_led;
static char s_msg[33] = "Hello from the P4";
static int  s_msg_i;

static const char *s_src = "defaults";    /* which surface changed it last */
static int64_t s_dirty_at;                /* papp_time_us() of the last change, 0 = saved */
static bool s_sd;                         /* the card is there and writable */
static bool s_quit;

static void touched(const char *src)
{
    s_src = src;
    s_dirty_at = papp_time_us();
}

/* ── Persistence: the surface that outlives a reboot ────────────────────── */

static void save(void)
{
    mkdir("/sd/data", 0777);
    mkdir(DATA_DIR, 0777);
    FILE *f = fopen(STATE, "w");
    if (!f) {
        papp_log("guition43: cannot write %s\n", STATE);
        s_sd = false;
        s_dirty_at = 0;                   /* no card: stop retrying every frame */
        return;
    }
    fprintf(f, "# Guition 4.3 Demo control state\nmode=%d\nlevel=%d\nled=%d\nmsg=%s\n",
            s_mode, s_level, s_led ? 1 : 0, s_msg);
    fclose(f);
    s_sd = true;
    s_dirty_at = 0;
}

static void load(void)
{
    FILE *f = fopen(STATE, "r");
    if (!f) {                             /* first run: plant the defaults on the card */
        save();
        if (s_sd) s_src = "SD card (new)";
        return;
    }
    char ln[96];
    while (fgets(ln, sizeof(ln), f)) {
        ln[strcspn(ln, "\r\n")] = 0;
        int v;
        if (sscanf(ln, "mode=%d", &v) == 1 && v >= 0 && v < M_N) s_mode = v;
        else if (sscanf(ln, "level=%d", &v) == 1 && v >= 0 && v <= 100) s_level = v;
        else if (sscanf(ln, "led=%d", &v) == 1) s_led = v != 0;
        else if (strncmp(ln, "msg=", 4) == 0) {
            snprintf(s_msg, sizeof(s_msg), "%.32s", ln + 4);   /* 32 chars + NUL */
            for (int i = 0; i < MSGS; i++)
                if (strcmp(k_msg[i], s_msg) == 0) s_msg_i = i;
        }
    }
    fclose(f);
    s_sd = true;
    s_src = "SD card";
}

/* ── Drawing ───────────────────────────────────────────────────────────── */

static uint16_t *s_px;
static int s_stride;

/* Clip rectangle, the screen unless something narrows it (the Input tab's touch pad). */
static int s_cx0, s_cy0, s_cx1 = W, s_cy1 = H;

static inline void clip_reset(void) { s_cx0 = 0; s_cy0 = 0; s_cx1 = W; s_cy1 = H; }
static inline void clip_to(int x, int y, int w, int h)
{
    s_cx0 = x < 0 ? 0 : x;
    s_cy0 = y < 0 ? 0 : y;
    s_cx1 = x + w > W ? W : x + w;
    s_cy1 = y + h > H ? H : y + h;
}

static inline uint16_t mix(uint16_t a, uint16_t b, int t)        /* t 0..256 = a..b */
{
    int r = ((a >> 11) * (256 - t) + (b >> 11) * t) >> 8;
    int g = (((a >> 5) & 63) * (256 - t) + ((b >> 5) & 63) * t) >> 8;
    int bl = ((a & 31) * (256 - t) + (b & 31) * t) >> 8;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

static inline void plot(int x, int y, uint16_t c, int a)          /* a 0..255 */
{
    if (x < s_cx0 || x >= s_cx1 || y < s_cy0 || y >= s_cy1 || a <= 0) return;
    uint16_t *p = &s_px[y * s_stride + x];
    *p = a >= 255 ? c : mix(*p, c, a + (a >> 7));
}

static inline int cover(float v) { return v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255.0f); }

/* Two pixels per store: the surface lives in PSRAM and this is pure write bandwidth —
 * a 16-bit loop runs at about half the speed. fill is also every rrect's inner loop. */
static void fill(int x, int y, int w, int h, uint16_t c)
{
    if (x < s_cx0) { w += x - s_cx0; x = s_cx0; }
    if (y < s_cy0) { h += y - s_cy0; y = s_cy0; }
    if (x + w > s_cx1) w = s_cx1 - x;
    if (y + h > s_cy1) h = s_cy1 - y;
    if (w <= 0 || h <= 0) return;

    uint32_t c2 = (uint32_t)c << 16 | c;
    for (int j = y; j < y + h; j++) {
        uint16_t *p = &s_px[j * s_stride + x];
        int n = w;
        if (((uintptr_t)p & 3) && n) { *p++ = c; n--; }     /* reach a 4-byte boundary */
        uint32_t *q = (uint32_t *)p;
        for (int k = n >> 1; k; k--) *q++ = c2;
        if (n & 1) *(uint16_t *)q = c;
    }
}

/* Filled rounded rectangle, anti-aliased corners */
static void rrect(int x, int y, int w, int h, int r, uint16_t c)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++) {
        float py = j + 0.5f;
        float dy = py < r ? r - py : py > h - r ? py - (h - r) : 0;
        if (dy == 0) { fill(x, y + j, w, 1, c); continue; }
        fill(x + r, y + j, w - 2 * r, 1, c);                /* only the corners need care */
        for (int i = 0; i < w; i++) {
            if (i == r && w - r > r) i = w - r;
            float px = i + 0.5f;
            float dx = px < r ? r - px : px > w - r ? px - (w - r) : 0;
            float d = sqrtf(dx * dx + dy * dy);
            plot(x + i, y + j, c, cover(r - d + 0.5f));
        }
    }
}

static void disc(float cx, float cy, float r, uint16_t c)
{
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            plot(x, y, c, cover(r - sqrtf(dx * dx + dy * dy) + 0.5f));
        }
}

static void ring(float cx, float cy, float r, float th, uint16_t c)
{
    for (int y = (int)(cy - r - th); y <= (int)(cy + r + th); y++)
        for (int x = (int)(cx - r - th); x <= (int)(cx + r + th); x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            plot(x, y, c, cover(th / 2 - fabsf(sqrtf(dx * dx + dy * dy) - r) + 0.5f));
        }
}

static void line(float x0, float y0, float x1, float y1, float th, uint16_t c)
{
    float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy + 1e-6f, hh = th / 2 + 1;
    int xa = (int)(fminf(x0, x1) - hh), xb = (int)(fmaxf(x0, x1) + hh);
    int ya = (int)(fminf(y0, y1) - hh), yb = (int)(fmaxf(y0, y1) + hh);
    for (int y = ya; y <= yb; y++)
        for (int x = xa; x <= xb; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = (px * vx + py * vy) / len2;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float dx = px - t * vx, dy = py - t * vy;
            plot(x, y, c, cover(th / 2 - sqrtf(dx * dx + dy * dy) + 0.5f));
        }
}

/* ── Text (Montserrat, see make_font.py; digits share one width) ───────── */

static const uf_glyph_t *glyph(const uf_font_t *f, int ch)
{
    return &f->g[ch >= 32 && ch < 127 ? ch - 32 : 0];
}

static inline bool is_digit(int c) { return c >= '0' && c <= '9'; }

static int text_w(const uf_font_t *f, const char *s)
{
    int da = glyph(f, '0')->adv, w = 0;
    for (; *s; s++) w += is_digit(*s) ? da : glyph(f, (unsigned char)*s)->adv;
    return w;
}

/* y = top of the line; returns the x after the text */
static int text(const uf_font_t *f, int x, int y, const char *s, uint16_t c)
{
    int da = glyph(f, '0')->adv;
    for (; *s; s++) {
        const uf_glyph_t *g = glyph(f, (unsigned char)*s);
        int adv = is_digit(*s) ? da : g->adv;
        int gx = x + g->x + (is_digit(*s) ? (da - g->adv) / 2 : 0), gy = y + g->y;
        int bw = (g->w + 1) / 2;
        const uint8_t *p = f->px + g->off;
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) {
                int v = p[j * bw + i / 2];
                v = (i & 1) ? v & 15 : v >> 4;
                if (v) plot(gx + i, gy + j, c, v * 17);
            }
        x += adv;
    }
    return x;
}

/* The line top that centres digits and capitals on cy */
static int line_top(const uf_font_t *f, int cy)
{
    const uf_glyph_t *g = glyph(f, '0');
    return cy - (g->y + g->h / 2);
}

static void text_c(const uf_font_t *f, int cx, int cy, const char *s, uint16_t c)
{
    text(f, cx - text_w(f, s) / 2, line_top(f, cy), s, c);
}

static void text_l(const uf_font_t *f, int x, int cy, const char *s, uint16_t c)
{
    text(f, x, line_top(f, cy), s, c);
}

static void text_r(const uf_font_t *f, int x, int cy, const char *s, uint16_t c)
{
    text(f, x - text_w(f, s), line_top(f, cy), s, c);
}

/* Word-wrapped paragraph from the top of a line; returns the y below it */
static int text_wrap(const uf_font_t *f, int x, int y, int w, const char *s, uint16_t c)
{
    char ln[160];
    while (*s) {
        int n = 0, last = 0;
        for (int i = 0; s[i]; i++) {
            if (n >= (int)sizeof(ln) - 1) break;
            ln[n++] = s[i];
            ln[n] = 0;
            if (s[i] == ' ') last = n;
            if (text_w(f, ln) > w) { n = last ? last : (n > 1 ? n - 1 : 1); ln[n] = 0; break; }
        }
        if (n <= 0) { n = 1; ln[1] = 0; }
        text(f, x, y, ln, c);
        y += f->height + 2;
        s += n;
        while (*s == ' ') s++;
    }
    return y;
}

/* ── Hit testing ───────────────────────────────────────────────────────── */

enum {
    H_TAB = 1,                  /* + tab index */
    H_CLOSE = 10,
    H_MODE = 20,                /* + mode index */
    H_LEVEL_DN = 30, H_LEVEL_UP, H_SLIDER,
    H_LED = 40,
    H_MSG_NEXT = 50, H_MSG_CLEAR,
};

typedef struct { int16_t x, y, w, h; uint8_t id; bool rep; } hit_t;
static hit_t s_hit[48];
static int s_nhit, s_pressed = -1;

static void hit(int x, int y, int w, int h, int id, bool rep)
{
    if (s_nhit < (int)(sizeof(s_hit) / sizeof(s_hit[0])))
        s_hit[s_nhit++] = (hit_t){ (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (uint8_t)id, rep };
}

static const hit_t *hit_at(int x, int y)
{
    for (int i = s_nhit - 1; i >= 0; i--)
        if (x >= s_hit[i].x && x < s_hit[i].x + s_hit[i].w &&
            y >= s_hit[i].y && y < s_hit[i].y + s_hit[i].h)
            return &s_hit[i];
    return NULL;
}

static uint16_t pressed_color(int id, uint16_t c)
{
    return id == s_pressed ? mix(c, C_TEXT, 60) : c;
}

/* ── Widgets ───────────────────────────────────────────────────────────── */

static void panel(int x, int y, int w, int h, const char *title)
{
    rrect(x, y, w, h, 14, C_PANEL);
    if (title) text_l(&F_SMALL, x + 18, y + 24, title, C_DIM);
}

static void button(int x, int y, int w, int h, const char *label, uint16_t bg, int id)
{
    rrect(x, y, w, h, h / 2, pressed_color(id, bg));
    text_c(&F_MED, x + w / 2, y + h / 2, label, C_TEXT);
    hit(x, y, w, h, id, false);
}

static void round_button(float cx, float cy, float r, const char *label, uint16_t bg, int id, bool rep)
{
    disc(cx, cy, r, pressed_color(id, bg));
    text_c(&F_MED, (int)cx, (int)cy, label, C_TEXT);
    hit((int)(cx - r - 6), (int)(cy - r - 6), (int)(2 * r + 12), (int)(2 * r + 12), id, rep);
}

static void toggle(int x, int y, bool on, int id)
{
    rrect(x, y, 64, 34, 17, pressed_color(id, on ? C_GREEN : C_BORDER));
    disc(on ? x + 47 : x + 17, y + 17, 13, C_TEXT);
    hit(x - 10, y - 12, 84, 58, id, false);
}

/* Segmented control: n labels, the selected one filled */
static void segmented(int x, int y, int w, int h, const char *const *labels, int n, int sel, int base)
{
    rrect(x, y, w, h, h / 2, C_RAISED);
    int sw = w / n;
    for (int i = 0; i < n; i++) {
        int sx = x + i * sw;
        bool on = i == sel;
        if (on) rrect(sx + 3, y + 3, sw - 6, h - 6, (h - 6) / 2, pressed_color(base + i, C_BLUE));
        else if (base + i == s_pressed) rrect(sx + 3, y + 3, sw - 6, h - 6, (h - 6) / 2, C_BORDER);
        text_c(&F_MED, sx + sw / 2, y + h / 2, labels[i], on ? C_TEXT : C_DIM);
        hit(sx, y, sw, h, base + i, false);
    }
}

#define SL_X 240
#define SL_Y 186
#define SL_W 300
#define SL_H 14

static void slider(int val)
{
    rrect(SL_X, SL_Y, SL_W, SL_H, SL_H / 2, C_RAISED);
    int fw = val * SL_W / 100;
    if (fw > 0) rrect(SL_X, SL_Y, fw, SL_H, SL_H / 2, C_BLUE);
    disc(SL_X + fw, SL_Y + SL_H / 2.0f, 15, C_TEXT);
    hit(SL_X - 16, SL_Y - 24, SL_W + 32, SL_H + 48, H_SLIDER, false);
}

static void level_from_x(int x)
{
    int v = (x - SL_X) * 100 / SL_W;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if (v != s_level) { s_level = v; touched("touch"); }
}

/* A label / value row inside a panel */
static int row(int x, int y, const char *label, const char *value, uint16_t vc)
{
    text_l(&F_SMALL, x, y, label, C_DIM);
    text_l(&F_SMALL, x + 160, y, value, vc);
    return y + 34;
}

/* ── Tab bar ───────────────────────────────────────────────────────────── */

static void draw_bar(void)
{
    fill(0, 0, W, BAR, C_PANEL);
    fill(0, BAR - 1, W, 1, C_BORDER);
    int tw = 150;
    for (int i = 0; i < T_N; i++) {
        int x = 16 + i * tw;
        bool on = i == s_tab;
        if (on) {
            rrect(x + 6, 12, tw - 12, BAR - 28, 10, C_RAISED);
            fill(x + 6 + 14, BAR - 8, tw - 12 - 28, 3, C_BLUE);
        }
        text_c(&F_MED, x + tw / 2, BAR / 2 - 3, k_tab[i], on ? C_TEXT : C_DIM);
        hit(x, 0, tw, BAR - 4, H_TAB + i, false);
    }
    /* Close */
    disc(W - 44, BAR / 2.0f, 22, pressed_color(H_CLOSE, C_RED));
    line(W - 52, BAR / 2.0f - 8, W - 36, BAR / 2.0f + 8, 3.0f, C_TEXT);
    line(W - 36, BAR / 2.0f - 8, W - 52, BAR / 2.0f + 8, 3.0f, C_TEXT);
    hit(W - 72, 0, 72, BAR - 4, H_CLOSE, false);
}

/* ── Home: what the launcher is giving us ──────────────────────────────── */

static void draw_home(int64_t now)
{
    char b[64];
    panel(16, BAR + 16, 380, H - BAR - 32, "BOARD");
    int y = BAR + 70, x = 34;

    y = row(x, y, "Board", papp_board_id(), C_TEXT);

    uint32_t abi = papp_launcher_abi();
    snprintf(b, sizeof(b), "%u.%u", (unsigned)(abi >> 16), (unsigned)(abi & 0xFFFF));
    y = row(x, y, "Launcher ABI", b, C_TEXT);

    papp_disp_info_t di;
    if (papp_disp_info(&di)) snprintf(b, sizeof(b), "%ux%u @ %u Hz", di.width, di.height, di.refresh_hz);
    else snprintf(b, sizeof(b), "none");
    y = row(x, y, "Display", b, C_TEXT);

    snprintf(b, sizeof(b), "%dx%d, scaled to fit", W, H);
    y = row(x, y, "Our surface", b, C_TEXT);

    snprintf(b, sizeof(b), "%u MB", (unsigned)(papp_mem_free_size(PAPP_MEM_LARGE) / (1024 * 1024)));
    y = row(x, y, "Free PSRAM", b, C_TEXT);

    snprintf(b, sizeof(b), "%u KB", (unsigned)(papp_mem_free_size(PAPP_MEM_FAST) / 1024));
    y = row(x, y, "Free internal", b, C_TEXT);

    y = row(x, y, "SD card", s_sd ? "mounted, writable" : "not available",
            s_sd ? C_GREEN : C_ORANGE);

    papp_datetime_t t;
    if (papp_local_time(&t)) snprintf(b, sizeof(b), "%02d:%02d:%02d", t.hour, t.minute, t.second);
    else snprintf(b, sizeof(b), "not set");
    y = row(x, y, "Clock", b, C_TEXT);

    snprintf(b, sizeof(b), "%d s", (int)(now / 1000000));
    y = row(x, y, "Running for", b, C_TEXT);

    y = row(x, y, "App version", PAPP_APP_VERSION, C_TEXT);

    /* Which launcher interfaces this build found: the ABI, made visible */
    panel(412, BAR + 16, W - 412 - 16, H - BAR - 32, "LAUNCHER INTERFACES");
    const struct { const char *name; const papp_iface_hdr_t *h; } ifs[] = {
        { "SYS  system, time",    (const papp_iface_hdr_t *)papp_if_sys() },
        { "MEM  allocation",      (const papp_iface_hdr_t *)papp_if_mem() },
        { "FS   files",           (const papp_iface_hdr_t *)papp_fs() },
        { "DISP display, touch",  (const papp_iface_hdr_t *)papp_if_disp() },
        { "AUDI audio",           (const papp_iface_hdr_t *)papp_if_audi() },
        { "INPT input",           (const papp_iface_hdr_t *)papp_if_inpt() },
        { "THRD threads",         (const papp_iface_hdr_t *)papp_if_thrd() },
        { "UI   launcher screens",(const papp_iface_hdr_t *)papp_if_ui() },
    };
    y = BAR + 70;
    for (int i = 0; i < (int)(sizeof(ifs) / sizeof(ifs[0])); i++) {
        text_l(&F_SMALL, 430, y, ifs[i].name, C_DIM);
        if (ifs[i].h) snprintf(b, sizeof(b), "v%u", ifs[i].h->version);
        else snprintf(b, sizeof(b), "absent");
        text_r(&F_SMALL, W - 34, y, b, ifs[i].h ? C_GREEN : C_FAINT);
        y += 30;
    }
    y += 16;
    text_wrap(&F_SMALL, 430, y, W - 412 - 50,
              "No networking here. The About tab says what the firmware adds.", C_FAINT);
}

/* ── Control: the shared state ─────────────────────────────────────────── */

static void draw_control(void)
{
    char b[64];

    /* Mode */
    text_l(&F_MED, 40, 110, "Mode", C_TEXT);
    segmented(240, 86, 340, 48, k_mode, M_N, s_mode, H_MODE);
    disc(640, 110, 13, k_mode_col[s_mode]);
    text_l(&F_SMALL, 666, 110, k_mode[s_mode], C_DIM);

    /* Level */
    text_l(&F_MED, 40, 193, "Level", C_TEXT);
    slider(s_level);
    round_button(588, 193, 22, "-", C_RAISED, H_LEVEL_DN, true);
    round_button(644, 193, 22, "+", C_RAISED, H_LEVEL_UP, true);
    snprintf(b, sizeof(b), "%d", s_level);
    text_r(&F_LARGE, 770, 193, b, C_TEXT);

    /* Indicator */
    text_l(&F_MED, 40, 272, "Indicator", C_TEXT);
    toggle(240, 255, s_led, H_LED);
    if (s_led) {
        disc(400, 272, 30, mix(C_BG, C_GREEN, 60));      /* glow */
        disc(400, 272, 20, C_GREEN);
    } else {
        ring(400, 272, 20, 3, C_FAINT);
    }
    text_l(&F_SMALL, 450, 272, s_led ? "on" : "off", C_DIM);

    /* Message */
    text_l(&F_MED, 40, 340, "Message", C_TEXT);
    rrect(240, 318, 530, 44, 12, C_RAISED);
    if (s_msg[0]) text_l(&F_MED, 256, 340, s_msg, C_TEXT);
    else text_l(&F_MED, 256, 340, "(empty)", C_FAINT);

    button(240, 384, 120, 44, "Next", C_BLUE, H_MSG_NEXT);
    button(376, 384, 120, 44, "Clear", C_RAISED, H_MSG_CLEAR);
    text_l(&F_SMALL, 516, 406, "a USB keyboard types here", C_FAINT);

    /* Footer: where the last change came from, and whether the card has it */
    fill(40, 440, W - 80, 1, C_BORDER);
    snprintf(b, sizeof(b), "last change from: %s", s_src);
    text_l(&F_SMALL, 40, 462, b, C_DIM);
    const char *sd = !s_sd ? "SD: no card" : s_dirty_at ? "SD: saving..." : "SD: saved";
    text_r(&F_SMALL, W - 40, 462, sd, !s_sd ? C_ORANGE : s_dirty_at ? C_ORANGE : C_GREEN);
}

/* ── Input: the launcher's input API, live ─────────────────────────────── */

static int s_touch_x = -1, s_touch_y = -1;
static char s_last_key[24] = "-";

#define TB_X 34                   /* the touch pad box */
#define TB_Y (BAR + 54)
#define TB_W 344
#define TB_H 190

static void draw_input(void)
{
    char b[64];

    /* Touch: only presses landing inside the box count, and the highlight stays inside it */
    panel(16, BAR + 16, 380, H - BAR - 32, "TOUCH");
    rrect(TB_X, TB_Y, TB_W, TB_H, 10, C_BG);
    bool in_box = s_touch_x >= TB_X && s_touch_x < TB_X + TB_W &&
                  s_touch_y >= TB_Y && s_touch_y < TB_Y + TB_H;
    if (in_box) {
        clip_to(TB_X, TB_Y, TB_W, TB_H);
        fill(TB_X, s_touch_y, TB_W, 1, C_BORDER);          /* crosshair across the box */
        fill(s_touch_x, TB_Y, 1, TB_H, C_BORDER);
        disc(s_touch_x, s_touch_y, 30, mix(C_BG, C_BLUE, 70));
        disc(s_touch_x, s_touch_y, 17, C_BLUE);
        disc(s_touch_x, s_touch_y, 6, C_TEXT);
        clip_reset();
        snprintf(b, sizeof(b), "x %d   y %d", s_touch_x, s_touch_y);
    } else {
        text_c(&F_SMALL, TB_X + TB_W / 2, TB_Y + TB_H / 2, "press inside this box", C_FAINT);
        snprintf(b, sizeof(b), s_touch_x >= 0 ? "outside the box" : "not touching");
    }
    text_l(&F_MED, 34, BAR + 272, b, in_box ? C_TEXT : C_DIM);
    text_wrap(&F_SMALL, 34, BAR + 296, 344,
              "Only presses inside the box are shown, in surface coordinates - the same ones "
              "you draw in.", C_FAINT);

    /* Gamepad + keyboard */
    panel(412, BAR + 16, W - 412 - 16, H - BAR - 32, "GAMEPAD AND KEYBOARD");
    papp_gamepad_t gp = { 0 };
    bool have = papp_input_gamepad(0, &gp);
    text_l(&F_MED, 430, BAR + 70, have ? (gp.name[0] ? gp.name : "gamepad") : "no gamepad",
           have ? C_TEXT : C_FAINT);
    text_l(&F_SMALL, 430, BAR + 96, "shown only here - START for the menu", C_FAINT);

    static const struct { const char *n; uint32_t bit; int x, y; } btn[] = {
        { "Up",   PAPP_BTN_UP,     452, BAR + 168 }, { "Dn", PAPP_BTN_DOWN,  452, BAR + 228 },
        { "Lf",   PAPP_BTN_LEFT,   422, BAR + 198 }, { "Rt", PAPP_BTN_RIGHT, 482, BAR + 198 },
        { "A",    PAPP_BTN_A,      610, BAR + 228 }, { "B",  PAPP_BTN_B,     650, BAR + 198 },
        { "X",    PAPP_BTN_X,      570, BAR + 198 }, { "Y",  PAPP_BTN_Y,     610, BAR + 168 },
        { "L1",   PAPP_BTN_L1,     540, BAR + 128 }, { "R1", PAPP_BTN_R1,    586, BAR + 128 },
        { "Sel",  PAPP_BTN_SELECT, 632, BAR + 128 }, { "St", PAPP_BTN_START, 678, BAR + 128 },
    };
    for (int i = 0; i < (int)(sizeof(btn) / sizeof(btn[0])); i++) {
        bool on = have && (gp.buttons & btn[i].bit);
        disc(btn[i].x, btn[i].y, 18, on ? C_BLUE : C_RAISED);
        text_c(&F_SMALL, btn[i].x, btn[i].y, btn[i].n, on ? C_TEXT : C_FAINT);
    }

    /* Sticks */
    for (int i = 0; i < 2; i++) {
        int cx = 450 + i * 110, cy = BAR + 280;
        ring(cx, cy, 34, 2, C_BORDER);
        int ax = i ? gp.rx : gp.lx, ay = i ? gp.ry : gp.ly;
        if (!have) ax = ay = 0;
        disc(cx + ax * 28.0f / 32768.0f, cy + ay * 28.0f / 32768.0f, 8, C_BLUE);
        text_c(&F_SMALL, cx, cy + 50, i ? "right" : "left", C_FAINT);
    }

    snprintf(b, sizeof(b), "last key: %s", s_last_key);
    text_l(&F_SMALL, 430, BAR + 360, b, C_DIM);
    int an = papp_input_analog(PAPP_ANALOG_PADDLE);
    if (an >= 0) snprintf(b, sizeof(b), "paddle %d", an);
    else snprintf(b, sizeof(b), "no paddle");
    text_r(&F_SMALL, W - 34, BAR + 360, b, C_FAINT);
}

/* ── About ─────────────────────────────────────────────────────────────── */

static void draw_about(void)
{
    text_l(&F_MED, 40, BAR + 20, "The teaching platform, and what a PAPP can hold", C_TEXT);
    int y = BAR + 56;

    y = text_wrap(&F_SMALL, 40, y, W - 80,
        "This is the demo half of the Guition P4 teaching firmware: one shared control state - "
        "mode, level, indicator, message - that several surfaces edit at once.", C_DIM);
    y += 10;
    y = text_wrap(&F_SMALL, 40, y, W - 80,
        "The firmware gives that state four surfaces: the touchscreen, a web page the board "
        "serves, a Bluetooth terminal, and the SD card that survives a reboot.", C_DIM);
    y += 10;
    y = text_wrap(&F_SMALL, 40, y, W - 80,
        "A launcher app reaches only the launcher: display, input, audio, files, threads and the "
        "clock. No WiFi, Bluetooth, HTTP or firmware update - so this app keeps the two surfaces "
        "it can, the screen and the SD card. The Home tab lists what it found.", C_DIM);
    y += 10;
    text_wrap(&F_SMALL, 40, y, W - 80,
        "Firmware, with the full five-tab UI, provisioning, web page and A/B updates: "
        "github.com/giltal/ESP32P4_Guition4_7_Project", C_FAINT);

    fill(40, 420, W - 80, 1, C_BORDER);
    text_l(&F_SMALL, 40, 440, "Touch: the tabs above; X closes the app.", C_FAINT);
    text_l(&F_SMALL, 40, 464, "Pad: L1/R1 tab (not on Input, which shows them), START menu, "
                              "A indicator, X mode, Y message, Up/Down level.", C_FAINT);
}

/* ── Acting on a control ───────────────────────────────────────────────── */

static void act(int id, const char *src)
{
    if (id >= H_TAB && id < H_TAB + T_N) { s_tab = id - H_TAB; return; }
    if (id == H_CLOSE) { s_quit = true; return; }
    if (id >= H_MODE && id < H_MODE + M_N) {
        if (s_mode != id - H_MODE) { s_mode = id - H_MODE; touched(src); }
        return;
    }
    switch (id) {
    case H_LEVEL_DN: if (s_level > 0)   { s_level--; touched(src); } break;
    case H_LEVEL_UP: if (s_level < 100) { s_level++; touched(src); } break;
    case H_LED:      s_led = !s_led; touched(src); break;
    case H_MSG_NEXT:
        s_msg_i = (s_msg_i + 1) % MSGS;
        snprintf(s_msg, sizeof(s_msg), "%s", k_msg[s_msg_i]);
        touched(src);
        break;
    case H_MSG_CLEAR: s_msg[0] = 0; touched(src); break;
    default: break;
    }
}

/* Auto-repeat for the held directions */
static uint32_t pad_repeat(uint32_t held, int64_t now)
{
    static uint32_t prev_held;
    static int64_t next;
    const uint32_t REP = PAPP_BTN_UP | PAPP_BTN_DOWN;
    uint32_t r = 0;
    if ((held & REP) != (prev_held & REP)) next = now + 400000;
    else if ((held & REP) && now >= next) { r = held & REP; next = now + 60000; }
    prev_held = held;
    return r;
}

static void pad(uint32_t press)
{
    /* The Input tab exists to show the buttons, so it must not navigate on them: there you
     * would press Left to watch Left light up and leave the tab instead. Touch the tabs or
     * use START to get out. */
    if (s_tab != T_INPUT) {
        if (press & (PAPP_BTN_R1 | PAPP_BTN_RIGHT)) s_tab = (s_tab + 1) % T_N;
        if (press & (PAPP_BTN_L1 | PAPP_BTN_LEFT))  s_tab = (s_tab + T_N - 1) % T_N;
    }
    if (s_tab != T_CONTROL) return;
    if (press & PAPP_BTN_UP)   act(H_LEVEL_UP, "pad");
    if (press & PAPP_BTN_DOWN) act(H_LEVEL_DN, "pad");
    if (press & PAPP_BTN_A)    act(H_LED, "pad");
    if (press & PAPP_BTN_X)    act(H_MODE + (s_mode + 1) % M_N, "pad");
    if (press & PAPP_BTN_Y)    act(H_MSG_NEXT, "pad");
}

/* A USB keyboard edits the message */
static void keyboard(void)
{
    papp_input_event_t ev;
    while (papp_input_poll_event(&ev)) {
        if (ev.type != PAPP_EV_KEY_DOWN) continue;
        if (ev.code == 0x2A) {                             /* Backspace */
            size_t n = strlen(s_msg);
            if (n) { s_msg[n - 1] = 0; touched("keyboard"); }
            snprintf(s_last_key, sizeof(s_last_key), "Backspace");
            continue;
        }
        if (ev.text >= 32 && ev.text < 127) {
            snprintf(s_last_key, sizeof(s_last_key), "%c", (char)ev.text);
            if (s_tab == T_CONTROL) {
                size_t n = strlen(s_msg);
                if (n + 1 < sizeof(s_msg)) {
                    s_msg[n] = (char)ev.text;
                    s_msg[n + 1] = 0;
                    touched("keyboard");
                }
            }
        } else {
            snprintf(s_last_key, sizeof(s_last_key), "usage 0x%02X", ev.code);
        }
    }
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    papp_surface_t *s = papp_disp_surface_create(&cfg);
    if (!s) { papp_log("guition43: no surface\n"); return 1; }

    /* Drawing into our own PSRAM buffer and handing it over with papp_disp_present_ext was
     * measured on this board: identical frame time, 768 KB more PSRAM. The surface buffer is
     * not the bottleneck — PSRAM write bandwidth is. Left as lock / present. */

    load();

    uint32_t prev = 0;
    bool down = false, wait_release = false, repeated = false;
    int down_id = -1;
    int64_t rep_next = 0;
    papp_touch_point_t tp = { 0 };
    int64_t start = papp_time_us();

    while (!papp_exit_requested() && !s_quit) {
        int64_t now = papp_time_us();

        papp_gamepad_t gp = { 0 };
        papp_input_gamepad(0, &gp);
        uint32_t press = gp.buttons & ~prev;
        prev = gp.buttons;
        press |= pad_repeat(gp.buttons, now);

        keyboard();

        /* Touch: taps act on release; +/- repeat while held; the slider tracks the finger */
        papp_touch_point_t cur;
        bool touch = papp_disp_touch(s, &cur, 1) > 0;
        s_touch_x = touch ? cur.x : -1;
        s_touch_y = touch ? cur.y : -1;
        if (wait_release) {
            wait_release = touch;
        } else if (touch) {
            tp = cur;
            if (!down) {
                down = true;
                repeated = false;
                const hit_t *h = hit_at(cur.x, cur.y);
                down_id = h ? h->id : -1;
                if (down_id == H_SLIDER) level_from_x(cur.x);
                else if (h && h->rep) { act(down_id, "touch"); repeated = true; rep_next = now + 400000; }
            } else if (down_id == H_SLIDER) {
                level_from_x(cur.x);
            } else if (repeated && down_id >= 0 && now >= rep_next) {
                act(down_id, "touch");
                rep_next = now + 70000;
            }
        } else if (down) {
            down = false;
            const hit_t *h = hit_at(tp.x, tp.y);
            if (!repeated && down_id != H_SLIDER && h && h->id == down_id) act(down_id, "touch");
            down_id = -1;
        }
        s_pressed = down ? down_id : -1;

        if (press & PAPP_BTN_START) {
            static const char *const items[] = { "Home", "Control", "Input", "About", "Exit" };
            int r = papp_ui_menu("Guition 4.3 Demo", items, 5, s_tab);
            if (r == T_N) break;
            if (r >= 0) s_tab = r;
            prev = 0xFFFFFFFFu;                 /* ignore what closed the menu until released */
            down = false;
            wait_release = true;
            continue;
        }
        pad(press);

        /* Write the state once it has been quiet for a moment */
        if (s_dirty_at && now - s_dirty_at > SAVE_QUIET_US) save();

        s_px = papp_disp_lock(s, &s_stride);
        clip_reset();
        s_nhit = 0;
        fill(0, 0, W, H, C_BG);
        draw_bar();
        switch (s_tab) {
        case T_HOME:    draw_home(now - start); break;
        case T_CONTROL: draw_control(); break;
        case T_INPUT:   draw_input(); break;
        case T_ABOUT:   draw_about(); break;
        default: break;
        }
        papp_disp_present(s);
    }

    if (s_dirty_at) save();                     /* don't lose the last change on the way out */
    papp_disp_surface_destroy(s);
    return 0;
}
