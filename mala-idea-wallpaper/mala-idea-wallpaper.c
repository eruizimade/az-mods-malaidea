/* SPDX-License-Identifier: GPL-3.0-only
 *
 * mala-idea-wallpaper: the Mala Idea wallpaper on the XDJ-AZ screen, animated.
 *
 * AZ-MODS lets a community mod paint only over EP147's screen, never under it, so the
 * wallpaper covers the whole screen while it is up. It comes up when the deck powers on and
 * when the deck has sat untouched for a while with no deck playing, and it stays until SHIFT
 * is pressed twice in a row (either side). Touches while it is up are kept from EP147, so they
 * never press what is under it; a finger held on the screen for 3 seconds also closes it, in
 * case SHIFT is ever not seen. "Show now" in the Mod Menu shows it straight away.
 *
 * The picture is "Corrent": the logo in AZ-MODS orange on black, the inside of the bubble a
 * slow wave of characters, the letters dense and swept by a band of light, loose grey
 * characters drifting left outside. assets/corrent.mic (tools/make_wallpaper.py writes it)
 * holds the logo as 6x12 px character cells and the coverage of each character, so the mod
 * draws every frame itself, with no font of its own. preview/fons-animat.html shows the same
 * animation in a browser.
 *
 * Threads: init(), tick(), the rows and the surface run on the core's UI thread; the touch
 * and frame filters run on the bus threads and only set atomics that tick() reads.
 */
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "azmod-mod.h"

#define MOD_ID "mala-idea-wallpaper"
#define SCREEN_W 1280
#define SCREEN_H 800
#define ALL_VIEWS (AZ_VIEW_BIT(AZ_VIEW_OTHER) | AZ_VIEW_BIT(AZ_VIEW_SOURCE) | \
                   AZ_VIEW_BIT(AZ_VIEW_BROWSE) | AZ_VIEW_BIT(AZ_VIEW_SEARCH) | \
                   AZ_VIEW_BIT(AZ_VIEW_DECK) | AZ_VIEW_BIT(AZ_VIEW_MENU))

#define FPS 12              /* frames a second while it is up (MAX_REDRAWS in the Makefile) */
#define SHIFT_GAP 0.6       /* the most seconds between the two SHIFT presses */
#define HOLD_TO_CLOSE 3.0   /* a finger held this long on the screen also closes it */

/* The look, as in the preview: AZ-MODS orange and its grey, on black. */
#define ORANGE_R 255
#define ORANGE_G 140
#define ORANGE_B 0
#define GREY 150
static const char RAMP[] = " .:-=+*";  /* the bubble's inside, by the wave's height */
static const char DENSE[] = "@#%&8$";  /* the letters */
static const char DRIFT[] = "!#$%&*+-:;=?@[]<>/\\|^~01MALIDE";

static const az_core_v1 *C;
static az_surface *wall;

/* ---- assets/corrent.mic ---------------------------------------------------------------- */

static int cols, rows, cw, ch, n_glyphs;
static unsigned char glyph_of[256];   /* character -> glyph index, 255 if it has none */
static unsigned char *cover;          /* n_glyphs * cw * ch coverage bytes */
static unsigned char *kind;           /* cols * rows: 0 outside, 1 bubble, 2 outline, 3 letter */
static unsigned char *edge;           /* cols * rows: the outline's glyph, 255 for none */
static unsigned char *drift;          /* cols * rows: a loose character outside, 255 for none */
static unsigned char *frame_px;       /* SCREEN_W * SCREEN_H * 4, premultiplied RGBA */
static float *sin_col, *sin_row, *sin_diag;
static int ramp_g[sizeof RAMP - 1], dense_g[sizeof DENSE - 1];

static int row_on = -1, row_delay = -1, row_boot = -1;
static const char *const DELAYS[] = { "1 min", "2 min", "5 min", "10 min", "30 min" };
static const int DELAY_S[] = { 60, 120, 300, 600, 1800 };

static int shown;
static int boot_pending;
static double started, shown_at, last_activity, last_frame;
static atomic_int activity;   /* set by the filters: something was touched or turned */
static atomic_int close_req;  /* set by the filters: SHIFT twice, or a long hold */
static atomic_int wall_up;    /* mirrors `shown` for the filters */
static atomic_int finger_down;
static _Atomic double finger_since;
static unsigned char mixer_prev[4];
static int mixer_seen;
static int shift_prev[2];
static double shift_last;     /* the ERP filter's own: one bus, one thread */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int load_corrent(void)
{
    const char *dir = AZ_HAS(C, assets_dir) ? C->assets_dir(MOD_ID) : NULL;
    if (!dir) {
        C->log(MOD_ID, "no assets folder (is the USB it was installed from in the deck?)");
        return -1;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/corrent.mic", dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        C->log(MOD_ID, "cannot open %s", path);
        return -1;
    }
    unsigned char h[11];
    int ok = fread(h, 1, sizeof h, f) == sizeof h && memcmp(h, "MIC1", 4) == 0;
    cols = h[4] | h[5] << 8;
    rows = h[6] | h[7] << 8;
    cw = h[8];
    ch = h[9];
    n_glyphs = h[10];
    if (!ok || cols <= 0 || rows <= 0 || cw <= 0 || ch <= 0 || n_glyphs <= 0 ||
        cols * cw > SCREEN_W || rows * ch > SCREEN_H) {
        C->log(MOD_ID, "%s is not a wallpaper this mod can read", path);
        fclose(f);
        return -1;
    }
    size_t cells = (size_t)cols * rows, gsize = (size_t)cw * ch;
    char chars[256];
    cover = malloc(n_glyphs * gsize);
    kind = malloc(cells);
    edge = malloc(cells);
    drift = malloc(cells);
    frame_px = calloc((size_t)SCREEN_W * SCREEN_H, 4);
    sin_col = malloc(cols * sizeof *sin_col);
    sin_row = malloc(rows * sizeof *sin_row);
    sin_diag = malloc((cols + rows) * sizeof *sin_diag);
    if (!cover || !kind || !edge || !drift || !frame_px || !sin_col || !sin_row || !sin_diag ||
        fread(chars, 1, n_glyphs, f) != (size_t)n_glyphs ||
        fread(cover, 1, n_glyphs * gsize, f) != n_glyphs * gsize ||
        fread(kind, 1, cells, f) != cells || fread(edge, 1, cells, f) != cells) {
        C->log(MOD_ID, "could not read %s", path);
        fclose(f);
        free(cover), free(kind), free(edge), free(drift), free(frame_px);
        free(sin_col), free(sin_row), free(sin_diag);
        cover = kind = edge = drift = frame_px = NULL;
        return -1;
    }
    fclose(f);
    memset(glyph_of, 255, sizeof glyph_of);
    for (int i = 0; i < n_glyphs; i++)
        glyph_of[(unsigned char)chars[i]] = (unsigned char)i;
    for (size_t i = 0; i < cells; i++)
        if (edge[i] >= n_glyphs)
            edge[i] = 255;
    for (size_t i = 0; i < sizeof RAMP - 1; i++)
        ramp_g[i] = glyph_of[(unsigned char)RAMP[i]];
    for (size_t i = 0; i < sizeof DENSE - 1; i++)
        dense_g[i] = glyph_of[(unsigned char)DENSE[i]];
    /* one cell in twenty outside the bubble carries a loose character */
    unsigned seed = 1;
    for (size_t i = 0; i < cells; i++) {
        seed = seed * 1103515245u + 12345u;
        unsigned r = seed >> 16;
        drift[i] = (r % 20 == 0) ? glyph_of[(unsigned char)DRIFT[(r / 20) % (sizeof DRIFT - 1)]] : 255;
    }
    return 0;
}

/* ---- drawing one frame of Corrent ------------------------------------------------------- */

/* One character at cell (c, r): colour (r8, g8, b8) at strength a (0..1) over black. */
static void put(int c, int r, int g, int r8, int g8, int b8, float a)
{
    if (g == 255 || a <= 0.0f)
        return;
    if (a > 1.0f)
        a = 1.0f;
    int cr = (int)(r8 * a), cg = (int)(g8 * a), cb = (int)(b8 * a);
    const unsigned char *cv = cover + (size_t)g * cw * ch;
    for (int y = 0; y < ch; y++) {
        unsigned char *p = frame_px + ((size_t)(r * ch + y) * SCREEN_W + (size_t)c * cw) * 4;
        for (int x = 0; x < cw; x++, p += 4) {
            int k = *cv++;
            p[0] = (unsigned char)(cr * k / 255);
            p[1] = (unsigned char)(cg * k / 255);
            p[2] = (unsigned char)(cb * k / 255);
        }
    }
}

/* Orange moved `w` (0..1) towards white. */
static void orange(float w, int *r8, int *g8, int *b8)
{
    if (w < 0.0f)
        w = 0.0f;
    if (w > 1.0f)
        w = 1.0f;
    *r8 = ORANGE_R + (int)((255 - ORANGE_R) * w);
    *g8 = ORANGE_G + (int)((255 - ORANGE_G) * w);
    *b8 = ORANGE_B + (int)((255 - ORANGE_B) * w);
}

static void draw_frame(double secs)
{
    float t = (float)(secs * 12.0); /* the preview's time unit: 12 a second */
    float s = t * 0.06f;
    float band = fmodf(t * 0.9f, (float)(cols + 60)) - 30.0f;
    int shift = (int)(t / 2.0f) % cols;

    for (int c = 0; c < cols; c++)
        sin_col[c] = sinf(c * 0.09f + s);
    for (int r = 0; r < rows; r++)
        sin_row[r] = sinf(r * 0.21f - s * 1.3f);
    for (int d = 0; d < cols + rows; d++)
        sin_diag[d] = sinf(d * 0.05f + s * 0.7f);

    for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) {
        frame_px[i * 4 + 0] = frame_px[i * 4 + 1] = frame_px[i * 4 + 2] = 0;
        frame_px[i * 4 + 3] = 255;
    }
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            size_t i = (size_t)r * cols + c;
            float n = ((sin_col[c] + sin_row[r] + sin_diag[c + r]) / 3.0f + 1.0f) / 2.0f;
            float hl = 1.0f - fabsf(c - band) / 10.0f;
            if (hl < 0.0f)
                hl = 0.0f;
            int r8, g8, b8;
            switch (kind[i]) {
            case 3: {
                int k = (int)(n * (sizeof DENSE - 1));
                if (k > (int)sizeof DENSE - 2)
                    k = (int)sizeof DENSE - 2;
                orange(hl * 3.0f, &r8, &g8, &b8);
                put(c, r, dense_g[k], r8, g8, b8, 0.7f + 0.3f * n);
                break;
            }
            case 2:
                orange(hl * 2.0f, &r8, &g8, &b8);
                put(c, r, edge[i], r8, g8, b8, 0.85f + 0.15f * hl);
                break;
            case 1: {
                int k = (int)(n * (sizeof RAMP - 1));
                if (k > (int)sizeof RAMP - 2)
                    k = (int)sizeof RAMP - 2;
                put(c, r, ramp_g[k], ORANGE_R, ORANGE_G, ORANGE_B, 0.15f + 0.35f * n + 0.3f * hl);
                break;
            }
            default:
                put(c, r, drift[(size_t)r * cols + (c + shift) % cols], GREY, GREY, GREY, 0.18f);
            }
        }
    }
}

static void paint(az_draw *d, az_rect a, void *user)
{
    (void)user;
    C->fill_rect(d, a.x, a.y, a.w, a.h, 0x000000);
    if (!frame_px)
        return;
    draw_frame(now_s() - shown_at);
    int ox = a.x + (a.w - cols * cw) / 2, oy = a.y + (a.h - rows * ch) / 2;
    C->draw_image(d, ox, oy, frame_px, SCREEN_W, rows * ch);
}

/* Taps reach us only while it is up; the touch filter already has them. */
static int surface_input(const az_event *ev, void *user)
{
    (void)ev;
    (void)user;
    return 1;
}

static void show(int on)
{
    if (!wall || shown == on)
        return;
    shown = on;
    atomic_store(&wall_up, on);
    atomic_store(&close_req, 0);
    C->surface_show(wall, on);
    if (on) {
        shown_at = last_frame = now_s();
        C->surface_redraw(wall);
    }
}

/* ---- what counts as activity, and what closes it ---------------------------------------- */

/* While the wallpaper is up, every touch is ours: EP147 never sees it. A finger held for
 * HOLD_TO_CLOSE seconds closes it (tick() measures it, as a finger at rest sends nothing). */
static int on_touch(const az_touch *t, void *user)
{
    (void)user;
    if (t->down) {
        if (!atomic_exchange(&finger_down, 1))
            atomic_store(&finger_since, now_s());
        atomic_store(&activity, 1);
    } else {
        atomic_store(&finger_down, 0);
    }
    return atomic_load(&wall_up) ? AZ_TOUCH_TAKE : AZ_TOUCH_PASS;
}

/* SHIFT: ERP byte 0x09 bit 7 (side 1) and 0x11 bit 7 (side 2). Two presses within SHIFT_GAP
 * seconds, on either side, close the wallpaper. */
static int on_erp(int bus, unsigned char *frame, int len, void *user)
{
    (void)bus;
    (void)user;
    if (len < 0x12)
        return AZ_FRAME_OK;
    for (int side = 0; side < 2; side++) {
        int down = (frame[side ? 0x11 : 0x09] & 0x80) != 0;
        if (down && !shift_prev[side]) {
            double t = now_s();
            atomic_store(&activity, 1);
            if (t - shift_last <= SHIFT_GAP) {
                atomic_store(&close_req, 1);
                shift_last = 0;
            } else {
                shift_last = t;
            }
        }
        shift_prev[side] = down;
    }
    return AZ_FRAME_OK;
}

/* The browse knob and its buttons: mixer bytes 31 (knob press), 32 (MENU), 33 (TAG) and 34
 * (knob position). Faders and knobs are left out on purpose: they drift by a step at rest. */
static int on_mixer(int bus, unsigned char *frame, int len, void *user)
{
    (void)bus;
    (void)user;
    if (len < 35)
        return AZ_FRAME_OK;
    if (mixer_seen && memcmp(mixer_prev, frame + 31, 4) != 0)
        atomic_store(&activity, 1);
    memcpy(mixer_prev, frame + 31, 4);
    mixer_seen = 1;
    return AZ_FRAME_OK;
}

static void on_deck(int deck, int what, void *user)
{
    (void)deck;
    (void)user;
    if (what == AZ_DECK_LOADED || what == AZ_DECK_PLAY)
        atomic_store(&activity, 1);
}

/* The rows' values; without Mod Menu rows (an older core) it is on, at power-on and after
 * 5 minutes. */
static int enabled(void)
{
    return row_on < 0 || C->row_value(row_on) == 1;
}

static int delay_s(void)
{
    int d = row_delay < 0 ? 2 : C->row_value(row_delay);
    if (d < 0 || d >= (int)(sizeof DELAY_S / sizeof DELAY_S[0]))
        d = 2;
    return DELAY_S[d];
}

static int any_deck_playing(void)
{
    for (int i = 0; i < 4; i++) {
        az_deck_info di = { .size = sizeof di };
        if (C->deck_info(i, &di) == 0 && di.playing == 1)
            return 1;
    }
    return 0;
}

/* ---- Mod Menu -------------------------------------------------------------------------- */

static void show_now(void *user)
{
    (void)user;
    last_activity = now_s();
    show(1);
}

static void on_toggle(int value, void *user)
{
    (void)user;
    if (!value)
        show(0);
    last_activity = now_s();
}

static int have_picture(void *user)
{
    (void)user;
    return frame_px != NULL;
}

static void register_rows(void)
{
    static const az_row_v1 on = {
        .size = sizeof(az_row_v1), .key = "on", .label = "Mala Idea Wallpaper",
        .kind = AZ_ROW_TOGGLE, .flags = AZ_ROW_DEFAULT_ON,
        .help = "The animated Mala Idea wallpaper covers the screen at power-on and when the "
                "deck sits idle. Press SHIFT twice to get the deck back.",
        .available = have_picture, .on_change = on_toggle,
    };
    static const az_row_v1 boot = {
        .size = sizeof(az_row_v1), .key = "boot", .label = "At power-on",
        .kind = AZ_ROW_TOGGLE, .flags = AZ_ROW_DEFAULT_ON, .parent = "on",
        .help = "Shows the wallpaper when the deck starts, until SHIFT is pressed twice.",
    };
    static const az_row_v1 delay = {
        .size = sizeof(az_row_v1), .key = "delay", .label = "Show after",
        .kind = AZ_ROW_CHOICE, .parent = "on",
        .help = "How long the deck must sit untouched, with no deck playing, before the "
                "wallpaper comes up.",
        .choices = DELAYS, .n_choices = 5, .default_choice = 2,
    };
    static const az_row_v1 now = {
        .size = sizeof(az_row_v1), .key = "now", .label = "Show now",
        .kind = AZ_ROW_ACTION, .parent = "on",
        .help = "Shows the wallpaper right away. Press SHIFT twice to close it.",
        .available = have_picture, .on_activate = show_now,
    };
    row_on = C->register_row(MOD_ID, &on);
    row_boot = C->register_row(MOD_ID, &boot);
    row_delay = C->register_row(MOD_ID, &delay);
    C->register_row(MOD_ID, &now);
}

/* ---- the mod --------------------------------------------------------------------------- */

static int init(const az_core_v1 *core)
{
    C = core;
    if (!AZ_HAS(C, surface_create) || !AZ_HAS(C, draw_image) || !AZ_HAS(C, deck_info) ||
        !AZ_HAS(C, on_deck_event)) {
        C->log(MOD_ID, "this core is older than 1.2.3; nothing to show");
        return 0;
    }
    load_corrent(); /* without it the rows are grayed and nothing is shown */
    if (AZ_HAS(C, register_row))
        register_rows();

    static const az_surface_v1 desc = {
        .size = sizeof(az_surface_v1),
        .layer = AZ_LAYER_TOP,
        .views = ALL_VIEWS,
        .on_paint = paint,
        .on_input = surface_input,
    };
    wall = C->surface_create(MOD_ID, &desc);
    if (!wall)
        return 1; /* the reason is in the log */
    C->surface_place(wall, (az_rect){ 0, 0, SCREEN_W, SCREEN_H });

    if (AZ_HAS(C, touch_filter))
        C->touch_filter(MOD_ID, on_touch, NULL);
    if (AZ_HAS(C, frame_filter)) {
        C->frame_filter(MOD_ID, (1u << AZ_BUS_MIXER_IN) | AZ_FILTER_RAW, on_mixer, NULL);
        C->frame_filter(MOD_ID, (1u << AZ_BUS_ERP_IN) | AZ_FILTER_RAW, on_erp, NULL);
    }
    C->on_deck_event(MOD_ID, on_deck, NULL);
    started = last_activity = now_s();
    boot_pending = 1;
    return 0;
}

static void tick(void)
{
    if (!wall)
        return;
    double t = now_s();
    int act = atomic_exchange(&activity, 0);
    if (shown) {
        int held = atomic_load(&finger_down) && t - atomic_load(&finger_since) >= HOLD_TO_CLOSE;
        if (atomic_exchange(&close_req, 0) || held || !enabled()) {
            last_activity = t;
            show(0);
            return;
        }
        if (t - last_frame >= 1.0 / FPS) {
            last_frame = t;
            C->surface_redraw(wall);
        }
        return;
    }
    atomic_store(&close_req, 0);
    if (act)
        last_activity = t;
    if (!frame_px || !enabled())
        return;
    /* at power-on, once the deck's own screen is up */
    if (boot_pending && t - started >= 0.5) {
        boot_pending = 0;
        if (row_boot < 0 || C->row_value(row_boot) == 1) {
            show(1);
            return;
        }
    }
    static double last_check;
    if (t - last_check < 1.0) /* the decks are looked at once a second, not every tick */
        return;
    last_check = t;
    if (any_deck_playing()) {
        last_activity = t;
        return;
    }
    if (t - last_activity >= delay_s())
        show(1);
}

static const char *const TAGS[] = { "any", NULL };
static const az_mod_v1 MOD = { AZ_MOD_ABI, sizeof(az_mod_v1), MOD_ID, TAGS, init, tick, NULL };

AZ_MOD_EXPORT const az_mod_v1 *az_mod_v1_entry(void)
{
    return &MOD;
}
