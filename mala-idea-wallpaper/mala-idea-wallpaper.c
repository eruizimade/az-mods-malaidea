/* SPDX-License-Identifier: GPL-3.0-only
 *
 * mala-idea-wallpaper: the Mala Idea wallpaper on the XDJ-AZ screen.
 *
 * AZ-MODS lets a community mod paint only over EP147's screen, never under it, so the
 * wallpaper is an idle screen: when nothing has been touched for a while and no deck is
 * playing, it covers the whole screen; the first touch, browse-knob turn, deck load or play
 * takes it away. A touch that wakes it is kept from EP147, so it never presses what was
 * under the wallpaper. "Show now" in the Mod Menu shows it straight away.
 *
 * The picture is assets/wallpaper.miw (tools/make_wallpaper.py writes it): "MIW1", u16 width,
 * u16 height, a 256-colour RGB palette, then one palette index per pixel.
 *
 * Threads: init(), tick(), the rows and the surface run on the core's UI thread; the touch
 * and frame filters run on the bus thread and only set atomics that tick() reads.
 */
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

static const az_core_v1 *C;
static az_surface *wall;
static unsigned char *pixels; /* premultiplied RGBA, img_w * img_h * 4 */
static int img_w, img_h;

static int row_on = -1, row_delay = -1;
static const char *const DELAYS[] = { "1 min", "2 min", "5 min", "10 min", "30 min" };
static const int DELAY_S[] = { 60, 120, 300, 600, 1800 };

static int shown;
static double last_activity;
static double grace_until;    /* "Show now" was pressed with the knob: ignore that press */
static atomic_int activity;   /* set by the filters: something was touched or turned */
static atomic_int wall_up;    /* mirrors `shown` for the touch filter */
static unsigned char mixer_prev[4];
static int mixer_seen;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- the picture ----------------------------------------------------------------------- */

static int load_wallpaper(void)
{
    const char *dir = AZ_HAS(C, assets_dir) ? C->assets_dir(MOD_ID) : NULL;
    if (!dir) {
        C->log(MOD_ID, "no assets folder (is the USB it was installed from in the deck?)");
        return -1;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/wallpaper.miw", dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        C->log(MOD_ID, "cannot open %s", path);
        return -1;
    }
    unsigned char hdr[8], pal[768];
    int ok = fread(hdr, 1, 8, f) == 8 && memcmp(hdr, "MIW1", 4) == 0 && fread(pal, 1, 768, f) == 768;
    int w = hdr[4] | hdr[5] << 8, h = hdr[6] | hdr[7] << 8;
    if (!ok || w <= 0 || h <= 0 || w > SCREEN_W || h > SCREEN_H) {
        C->log(MOD_ID, "%s is not a wallpaper this mod can read", path);
        fclose(f);
        return -1;
    }
    unsigned char *idx = malloc((size_t)w * h);
    unsigned char *rgba = malloc((size_t)w * h * 4);
    if (!idx || !rgba || fread(idx, 1, (size_t)w * h, f) != (size_t)w * h) {
        C->log(MOD_ID, "could not read %s", path);
        free(idx);
        free(rgba);
        fclose(f);
        return -1;
    }
    fclose(f);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const unsigned char *c = pal + idx[i] * 3;
        rgba[i * 4 + 0] = c[0];
        rgba[i * 4 + 1] = c[1];
        rgba[i * 4 + 2] = c[2];
        rgba[i * 4 + 3] = 255; /* opaque, so premultiplied is the colour itself */
    }
    free(idx);
    pixels = rgba;
    img_w = w;
    img_h = h;
    return 0;
}

static void paint(az_draw *d, az_rect a, void *user)
{
    (void)user;
    C->fill_rect(d, a.x, a.y, a.w, a.h, 0x100a07);
    if (pixels)
        C->draw_image(d, a.x + (a.w - img_w) / 2, a.y + (a.h - img_h) / 2, pixels, img_w, img_h);
}

/* Taps reach us only while it is up; the touch filter already woke it. */
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
    C->surface_show(wall, on);
    if (on)
        C->surface_redraw(wall);
}

/* ---- what counts as activity ----------------------------------------------------------- */

/* While the wallpaper is up, the finger that wakes it is ours: EP147 never sees that touch. */
static int on_touch(const az_touch *t, void *user)
{
    (void)user;
    if (!t->down)
        return AZ_TOUCH_PASS;
    atomic_store(&activity, 1);
    return atomic_load(&wall_up) ? AZ_TOUCH_TAKE : AZ_TOUCH_PASS;
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

/* The rows' values; without Mod Menu rows (an older core) it is on, after 5 minutes. */
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
    grace_until = last_activity + 1.5;
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
    return pixels != NULL;
}

static void register_rows(void)
{
    static const az_row_v1 on = {
        .size = sizeof(az_row_v1), .key = "on", .label = "Mala Idea Wallpaper",
        .kind = AZ_ROW_TOGGLE, .flags = AZ_ROW_DEFAULT_ON,
        .help = "When the deck sits idle and nothing plays, the Mala Idea wallpaper "
                "covers the screen. Touch it or turn the browse knob to get the deck back.",
        .available = have_picture, .on_change = on_toggle,
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
        .help = "Shows the wallpaper right away. Touch the screen to close it.",
        .available = have_picture, .on_activate = show_now,
    };
    row_on = C->register_row(MOD_ID, &on);
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
    load_wallpaper(); /* without it the rows are grayed and nothing is shown */
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
    if (AZ_HAS(C, frame_filter))
        C->frame_filter(MOD_ID, (1u << AZ_BUS_MIXER_IN) | AZ_FILTER_RAW, on_mixer, NULL);
    C->on_deck_event(MOD_ID, on_deck, NULL);
    last_activity = now_s();
    return 0;
}

static void tick(void)
{
    if (!wall)
        return;
    double t = now_s();
    if (atomic_exchange(&activity, 0) && t >= grace_until) {
        last_activity = t;
        show(0);
        return;
    }
    if (shown || !pixels || !enabled())
        return;
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
