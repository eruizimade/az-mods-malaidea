/* SPDX-License-Identifier: GPL-3.0-only
 *
 * deck-suggest: what to play next, worked out on the deck from what Backspins knows.
 *
 * When a deck loads a track, a panel shows the six best follow-ups: the fit (as Sets shows
 * it), your stars, BPM, key, title, artist, genre, why, the letters to type in the deck's
 * SEARCH, and rekordbox's waveform. Filters on the panel (key, tempo, energy, stars, your
 * MyTags and genres) narrow them at once. No computer during the set: Backspins writes
 * BACKSPINS/deck-data.tsv to the drive when you export (deck_export.py), and deckdata.c
 * works out the suggestions here, the same way Backspins does (tests/parity.py).
 *
 * Threads: init(), tick(), the rows, the page and the deck events run on the core's UI
 * thread and never touch the disk: they queue jobs. One thread of ours reads the drive and
 * works out the suggestions; the answer is handed back under a mutex, and on_paint (the
 * overlay thread) copies it under the same mutex.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "azmod-mod.h"
#include "deckdata.h"


#define MOD_ID "deck-suggest"
#define ROWS 6
#define SESSION_GAP_S (45 * 60.0)
#define PLAYED_AFTER_S 45.0
#define PANEL_W 1040
#define ROW_H 42
#define HEAD_H 40
#define FILT_H 46
#define PANEL_H (HEAD_H + 2 * FILT_H + ROWS * ROW_H + 12)
#define GROUPS 6
#define GTAGS 10
#define WAVE_COLS 120
#define WAVE_W (WAVE_COLS * 2)    /* two pixels a column */
#define WAVE_H (ROW_H - 6)
#define TAB_W 150
#define TAB_H 44
#define CLOSE_W 64

static const az_core_v1 *C;

/* ---- what the panel shows (guarded by mu) ---------------------------------------------- */

typedef struct {
    int fit, stars;            /* fit 0..100 (as in Sets), stars 0..5 (your rekordbox rating) */
    char bpm[8], key[6], title[64], artist[44], genre[32], why[72], search[16];
    int secs, has_wave;
    unsigned char wave[WAVE_COLS][3];   /* low, mid, high: 0..15 (rekordbox's 3-band overview) */
} row_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int shown_deck = -1;          /* 0..3, the deck these suggestions are for */
static char now_title[64];
static row_t rows[ROWS];
static int n_rows, n_match = -1;    /* n_match: how many tracks pass the filters */
static char note[96] = "Load a track";
/* The filter groups and their tags, from the deck data (names for the panel, ids for the
 * filters). */
static int n_groups;
static char g_label[GROUPS][16];
static char g_tag[GROUPS][GTAGS][44];
static int g_tagid[GROUPS][GTAGS];
static int g_ntags[GROUPS];
static char data_state[120] = "reading the deck data...";
/* What the Status page shows to find out why a track does not match (guarded by mu). */
static char diag[4][200];
static atomic_int fresh;             /* the thread has a new answer for tick to show */
static atomic_int regrouped;         /* new filter tags arrived: repaint if the panel is up */
/* The row tapped on the panel (-1 = none): its SEARCH letters go big in the header. */
static int sel_row = -1;

/* ---- jobs for our thread (guarded by mu) ------------------------------------------------- */

enum { JOB_SUGGEST = 1, JOB_PLAYED, JOB_LOAD };
typedef struct {
    int kind, deck;
    unsigned id;               /* the content id on the drive the deck loaded from */
    char title[128], artist[128];
    unsigned other_id;         /* the track playing on another deck */
    char other_title[128], other_artist[128];
    dd_filters f;              /* the panel's filters (JOB_SUGGEST) */
} job_t;

static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static job_t jobs[8];
static int job_head, job_n;

static void push_job(const job_t *j)
{
    pthread_mutex_lock(&mu);
    if (j->kind == JOB_SUGGEST) /* only the newest suggestion matters */
        for (int i = 0; i < job_n; i++) {
            job_t *q = &jobs[(job_head + i) % 8];
            if (q->kind == JOB_SUGGEST) {
                *q = *j;
                pthread_mutex_unlock(&mu);
                pthread_cond_signal(&cv);
                return;
            }
        }
    if (job_n < 8) {
        jobs[(job_head + job_n) % 8] = *j;
        job_n++;
    }
    pthread_mutex_unlock(&mu);
    pthread_cond_signal(&cv);
}

/* ---- the deck data, and the drive's own library (our thread only) ------------------------- */

static char drive_root[300];         /* set in init(): where the drive is mounted */
static char data_path[400];          /* <drive>/BACKSPINS/deck-data.tsv */
static dd_lib *lib;                  /* the worker's; replaced by a reload, never freed (below) */
static atomic_int lib_gen;           /* bumped by each load */
static int session[256], n_session;  /* rows played this session, oldest first */
static double last_play;

/* Every track of the deck's drives (lib_tracks): its id on that drive, its title and artist
 * as that drive's library has them, and the deck data row it is (-1 = none). Built by a
 * thread of its own (mapper) and published under map_mu, sorted by id. ⚠ On its own thread
 * because reading a drive's library can take its time, or not come back at all (an empty
 * port): suggestions must never wait for it. Until it is ready, tracks are found by title. */
typedef struct { unsigned id; int stick, row; char *title, *artist; } idrow_t;
static pthread_mutex_t map_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t map_cv = PTHREAD_COND_INITIALIZER;
static idrow_t *ids;
static int n_ids, map_gen = -1;      /* map_gen: the lib_gen the published ids belong to */
static dd_lib *map_want;             /* what the mapper should map next (map_mu) */
static int map_want_gen = -1;

static void set_state(const char *fmt, ...)
{
    char b[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&mu);
    snprintf(data_state, sizeof data_state, "%s", b);
    pthread_mutex_unlock(&mu);
}

static void set_diag(int line, const char *fmt, ...)
{
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&mu);
    snprintf(diag[line], sizeof diag[line], "%s", b);
    pthread_mutex_unlock(&mu);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static char *dupstr(const char *s)
{
    size_t n = strlen(s ? s : "") + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s ? s : "", n);
    return p;
}

static int id_cmp(const void *a, const void *b)
{
    unsigned x = ((const idrow_t *)a)->id, y = ((const idrow_t *)b)->id;
    return x < y ? -1 : x > y;
}

/* ---- the mapper thread -------------------------------------------------------------------- */

typedef struct {
    const dd_lib *L;
    int stick;
    idrow_t *v;
    int n, cap, mapped;
    char sample[200];
} collect_t;

/* lib_tracks: every track of one drive. ⚠ Matched by the END of its path, whatever the
 * mount point is called, and without caring how accents are written. */
static int collect(const az_track *t, void *user)
{
    collect_t *c = user;
    if (c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 2048;
        idrow_t *b = realloc(c->v, sizeof(idrow_t) * (size_t)nc);
        if (!b)
            return 1;
        c->v = b;
        c->cap = nc;
    }
    if (!c->sample[0] && t->path)
        snprintf(c->sample, sizeof c->sample, "%s", t->path);
    idrow_t *e = &c->v[c->n++];
    e->id = t->id;
    e->stick = c->stick;
    e->row = dd_find_path_suffix(c->L, t->path);
    e->title = dupstr(t->title);
    e->artist = dupstr(t->artist);
    c->mapped += e->row >= 0;
    return 0;
}

/* Add one drive's tracks to the published map (if it still belongs to the current load). */
static void publish(const collect_t *c, int gen)
{
    pthread_mutex_lock(&map_mu);
    if (gen == atomic_load(&lib_gen)) {
        if (map_gen != gen) {
            free(ids); /* the strings of an older map are left: a reload is rare */
            ids = NULL;
            n_ids = 0;
            map_gen = gen;
        }
        idrow_t *b = realloc(ids, sizeof(idrow_t) * (size_t)(n_ids + c->n + 1));
        if (b) {
            ids = b;
            memcpy(ids + n_ids, c->v, sizeof(idrow_t) * (size_t)c->n);
            n_ids += c->n;
            qsort(ids, (size_t)n_ids, sizeof(idrow_t), id_cmp);
        }
    }
    pthread_mutex_unlock(&map_mu);
}

static void *mapper(void *arg)
{
    (void)arg;
    int done_gen = -1;
    for (;;) {
        pthread_mutex_lock(&map_mu);
        while (map_want_gen == done_gen || !map_want)
            pthread_cond_wait(&map_cv, &map_mu);
        const dd_lib *L = map_want;
        int gen = map_want_gen;
        pthread_mutex_unlock(&map_mu);
        done_gen = gen;
        char line[2][100] = { "", "" };
        char sample[200] = "";
        /* USB 1 first and on its own: whatever USB 2 does, USB 1's ids are in use */
        for (int stick = 0; stick < 2 && gen == atomic_load(&lib_gen); stick++) {
            set_diag(1, "Deck library: reading USB%d...%s%s", stick + 1, line[0][0] ? " " : "", line[0]);
            collect_t c = { L, stick, NULL, 0, 0, 0, "" };
            double t0 = now_s();
            int rc = C->lib_tracks(MOD_ID, stick, 0, collect, &c);
            snprintf(line[stick], sizeof line[stick], "USB%d %d tracks, %d in the deck data (%.1f s%s)",
                     stick + 1, c.n, c.mapped, now_s() - t0, rc < 0 ? ", none" : "");
            if (!sample[0] && c.sample[0])
                snprintf(sample, sizeof sample, "%s", c.sample);
            publish(&c, gen);
            free(c.v); /* the entries were copied; their strings now belong to the map */
            set_diag(1, "Deck library: %s%s%s", line[0], line[1][0] ? "; " : "", line[1]);
            set_diag(2, "Path example: %s", sample[0] ? sample : "(none)");
        }
    }
    return NULL;
}

/* ---- the worker --------------------------------------------------------------------------- */

static void load_data(void)
{
    /* ⚠ The old deck data is not freed: the mapper may still be matching against it, and a
     * reload is a rare, manual thing. */
    lib = NULL;
    atomic_fetch_add(&lib_gen, 1);
    n_session = 0;
    char err[160] = "";
    if (data_path[0])
        lib = dd_load(data_path, err, sizeof err);
    else
        snprintf(err, sizeof err, "the drive this mod was installed from is not in the deck");
    if (!lib) {
        set_state("%s", err);
        pthread_mutex_lock(&mu);
        n_groups = 0;
        pthread_mutex_unlock(&mu);
        atomic_store(&regrouped, 1);
        return;
    }
    pthread_mutex_lock(&mu);
    n_groups = dd_groups(lib) < GROUPS ? dd_groups(lib) : GROUPS;
    for (int g = 0; g < n_groups; g++) {
        snprintf(g_label[g], sizeof g_label[g], "%s", dd_group_label(lib, g));
        g_ntags[g] = dd_group_ntags(lib, g) < GTAGS ? dd_group_ntags(lib, g) : GTAGS;
        for (int i = 0; i < g_ntags[g]; i++) {
            g_tagid[g][i] = dd_group_tag(lib, g, i);
            snprintf(g_tag[g][i], sizeof g_tag[g][i], "%s", dd_tag_name(lib, g_tagid[g][i]));
        }
    }
    pthread_mutex_unlock(&mu);
    atomic_store(&regrouped, 1);
    set_state("%d tracks in Backspins' deck data", dd_count(lib));
    set_diag(0, "Drive: %s", drive_root[0] ? drive_root : "(not found)");
    /* the drive's library, on the mapper's own time */
    if (AZ_HAS(C, lib_tracks)) {
        pthread_mutex_lock(&map_mu);
        map_want = lib;
        map_want_gen = atomic_load(&lib_gen);
        pthread_mutex_unlock(&map_mu);
        pthread_cond_signal(&map_cv);
    } else {
        set_diag(1, "Deck library: this core cannot read it; tracks are found by title");
    }
}

/* the folded title of a row starts the deck's title, or the other way round */
static int title_like(const char *row_title, const char *title)
{
    char a[200], b[200];
    dd_fold(row_title, a, sizeof a, 1);
    dd_fold(title, b, sizeof b, 1);
    if (!b[0])
        return 1;
    return !strncmp(b, a, strlen(a)) || !strncmp(a, b, strlen(b));
}

/* The deck data row of what a deck has loaded, or -1:
 *   1. by its id on the drive, if the title agrees (⚠ a b2b partner's drive numbers its
 *      tracks too, and can use the same number);
 *   2. by the title and artist the deck reports;
 *   3. with no title from the deck, by the title the drive's own library has for that id. */
static int row_for(unsigned id, const char *title, const char *artist)
{
    if (!lib)
        return -1;
    const char *how = "not found";
    int row = -1, first = -1, ready;
    char dtitle[200] = "", dartist[200] = "";
    pthread_mutex_lock(&map_mu);
    ready = map_gen == atomic_load(&lib_gen);
    if (ready && id) {
        int lo = 0, hi = n_ids - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (ids[mid].id < id)
                lo = mid + 1;
            else {
                if (ids[mid].id == id)
                    first = mid;
                hi = mid - 1;
            }
        }
        for (int i = first; i >= 0 && i < n_ids && ids[i].id == id && row < 0; i++) {
            if (ids[i].row >= 0 && title_like(dd_get(lib, ids[i].row)->title, title)) {
                row = ids[i].row;
                how = "by id";
            }
            if (!dtitle[0]) {
                snprintf(dtitle, sizeof dtitle, "%s", ids[i].title);
                snprintf(dartist, sizeof dartist, "%s", ids[i].artist);
            }
        }
    }
    pthread_mutex_unlock(&map_mu);
    if (row < 0 && (row = dd_find_title(lib, title, artist)) >= 0)
        how = "by title";
    /* only when the deck reports no title: with one, a different song under the same id is
     * a b2b partner's track, not ours */
    if (row < 0 && !(title && title[0]) && dtitle[0] && (row = dd_find_title(lib, dtitle, dartist)) >= 0)
        how = "by the drive's title";
    set_diag(3, "Last track: id %u \"%.60s\" / \"%.40s\": %s%s", id, title ? title : "",
             artist ? artist : "", how,
             !ready ? " (drive library still being read)" : first < 0 && id ? " (id not in the deck library)" : "");
    return row;
}

static void show_answer(int deck, int now, const dd_pick *p, int n, int pass)
{
    row_t got[ROWS];
    memset(got, 0, sizeof got);
    for (int i = 0; i < n && i < ROWS; i++) {
        const dd_row *t = dd_get(lib, p[i].row);
        row_t *r = &got[i];
        r->fit = p[i].fit;
        r->stars = t->stars;
        if (t->bpm)
            snprintf(r->bpm, sizeof r->bpm, "%.1f", t->bpm);
        else
            snprintf(r->bpm, sizeof r->bpm, "-");
        snprintf(r->key, sizeof r->key, "%s", t->key);
        snprintf(r->title, sizeof r->title, "%s", t->title);
        snprintf(r->artist, sizeof r->artist, "%s", t->artist);
        snprintf(r->genre, sizeof r->genre, "%s", t->genre);
        snprintf(r->why, sizeof r->why, "%s", p[i].why);
        snprintf(r->search, sizeof r->search, "%s", t->search);
        r->secs = t->secs;
        r->has_wave = t->has_wave;
        memcpy(r->wave, t->wave, sizeof r->wave);
    }
    pthread_mutex_lock(&mu);
    shown_deck = deck;
    snprintf(now_title, sizeof now_title, "%s", dd_get(lib, now)->title);
    memcpy(rows, got, sizeof got);
    n_rows = n;
    n_match = pass;
    sel_row = -1;
    snprintf(note, sizeof note, "%s", n ? "" : "nothing passes these filters");
    pthread_mutex_unlock(&mu);
    atomic_store(&fresh, 1);
}

static void run_job(const job_t *j)
{
    if (j->kind == JOB_LOAD) {
        load_data();
        return;
    }
    int now = row_for(j->id, j->title, j->artist);
    if (j->kind == JOB_PLAYED) {
        if (now < 0)
            return;
        double t = now_s();
        if (n_session && t - last_play > SESSION_GAP_S)
            n_session = 0; /* a new session */
        last_play = t;
        if (n_session == (int)(sizeof session / sizeof session[0])) {
            memmove(session, session + 1, sizeof session - sizeof session[0]);
            n_session--;
        }
        session[n_session++] = now;
        return;
    }
    if (now < 0) {
        pthread_mutex_lock(&mu);
        shown_deck = j->deck;
        n_rows = 0;
        n_match = -1;
        snprintf(note, sizeof note, "%s", lib ? "this track is not in Backspins' deck data (Status says why)" : data_state);
        pthread_mutex_unlock(&mu);
        atomic_store(&fresh, 1);
        return;
    }
    int other = j->other_title[0] ? row_for(j->other_id, j->other_title, j->other_artist) : -1;
    dd_pick p[ROWS];
    int pass = 0;
    int n = dd_suggest(lib, session, n_session, now, other, &j->f, p, ROWS, &pass);
    show_answer(j->deck, now, p, n, pass);
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        job_t j;
        pthread_mutex_lock(&mu);
        while (job_n == 0)
            pthread_cond_wait(&cv, &mu);
        j = jobs[job_head];
        job_head = (job_head + 1) % 8;
        job_n--;
        pthread_mutex_unlock(&mu);
        run_job(&j);
    }
    return NULL;
}

/* ---- the panel ----------------------------------------------------------------------------- */

static az_surface *panel;
static az_rect panel_rect;
static int row_on = -1, row_pos = -1, row_hold = -1;
static const char *const POSITIONS[] = { "Top", "Middle", "Bottom" };
static const char *const HOLDS[] = { "10 s", "20 s", "40 s", "Until tapped" };
static const double HOLD_S[] = { 10, 20, 40, 0 };
static int expanded, visible;
static double touched_at;            /* the last time the panel opened or was touched */

/* The filters (guarded by mu: the UI thread changes them, on_paint reads them). */
static const char *const KEY_MODES[] = { "strict", "open", "any" };
static const char *const BPM_MODES[] = { "3", "6", "10", "any" };
static const int STAR_MODES[] = { 0, 3, 4, 5 };
static int f_key = 1, f_bpm = 1, f_energy, f_stars, f_group;
static char f_tag[GROUPS][44];       /* the chosen tag per group, "" = none */
static job_t last_job;               /* the last load, asked again when a filter changes */
static int have_last;

static int value(int row, int def)
{
    return row < 0 ? def : C->row_value(row);
}

/* The panel's filters as deckdata wants them; mu must be held. */
static void fill_filters(dd_filters *f)
{
    static const double BPM_PCT[] = { 3, 6, 10, -1 };
    memset(f, 0, sizeof *f);
    f->key_mode = f_key;
    f->bpm_pct = BPM_PCT[f_bpm];
    f->energy = f_energy;
    f->stars = STAR_MODES[f_stars];
    for (int g = 0; g < n_groups && f->ntags < 8; g++)
        for (int i = 0; f_tag[g][0] && i < g_ntags[g]; i++)
            if (!strcmp(f_tag[g], g_tag[g][i])) {
                f->tags[f->ntags++] = g_tagid[g][i];
                break;
            }
}

static void save_filters(void)
{
    if (!AZ_HAS(C, cfg_set))
        return;
    C->cfg_set(MOD_ID, "f_key", f_key);
    C->cfg_set(MOD_ID, "f_bpm", f_bpm);
    C->cfg_set(MOD_ID, "f_energy", f_energy);
    C->cfg_set(MOD_ID, "f_stars", f_stars);
}

static void load_filters(void)
{
    if (!AZ_HAS(C, cfg_get))
        return;
    int k = C->cfg_get(MOD_ID, "f_key", 1), b = C->cfg_get(MOD_ID, "f_bpm", 1);
    int e = C->cfg_get(MOD_ID, "f_energy", 0), st = C->cfg_get(MOD_ID, "f_stars", 0);
    f_key = k >= 0 && k < 3 ? k : 1;
    f_bpm = b >= 0 && b < 4 ? b : 1;
    f_energy = e >= 0 && e < 4 ? e : 0;
    f_stars = st >= 0 && st < 4 ? st : 0;
}

static void place(void)
{
    if (!panel)
        return;
    static const int Y[] = { 80, 210, 400 };
    int p = value(row_pos, 1);
    int y = Y[p >= 0 && p < 3 ? p : 1];
    if (expanded)
        panel_rect = (az_rect){ (1280 - PANEL_W) / 2, y, PANEL_W, PANEL_H };
    else
        panel_rect = (az_rect){ 1280 - TAB_W - 16, y, TAB_W, TAB_H };
    C->surface_place(panel, panel_rect);
    C->surface_redraw(panel);
}

static void set_visible(int on)
{
    if (!panel || visible == on)
        return;
    visible = on;
    C->surface_show(panel, on);
}

static void expand(int on)
{
    expanded = on;
    touched_at = now_s();
    place();
}

/* ---- layout: the same boxes for painting and for taps -------------------------------------- */

enum { HIT_NONE = 0, HIT_HEAD, HIT_KEY, HIT_BPM, HIT_ENERGY, HIT_STARS, HIT_GROUP, HIT_CLEAR,
       HIT_TAG /* + tag index */ = 100 };
typedef struct { int x, y, w, h, what; char text[40]; int on; } box_t;

/* The filter boxes, relative to the panel's top left; mu must be held. */
static int layout(box_t *bx, int max)
{
    int n = 0, px = 16, pad = 14, gap = 8;
    int y1 = HEAD_H + 6, y2 = HEAD_H + FILT_H + 6, h = FILT_H - 10;
    int x = 12;
    char t[40];
    static const char *const EN[] = { "any", "up", "=", "down" };
#define ADD(Y, WHAT, ON, ...)                                                           \
    do {                                                                                \
        if (n < max) {                                                                  \
            snprintf(t, sizeof t, __VA_ARGS__);                                         \
            int w = C->text_width(t, px) + 2 * pad;                                     \
            if (x + w > PANEL_W - 12)                                                   \
                break;                                                                  \
            bx[n] = (box_t){ x, Y, w, h, WHAT, "", ON };                                \
            snprintf(bx[n].text, sizeof bx[n].text, "%s", t);                           \
            n++;                                                                        \
            x += w + gap;                                                               \
        }                                                                               \
    } while (0)
    ADD(y1, HIT_KEY, f_key != 2, "KEY %s", KEY_MODES[f_key]);
    ADD(y1, HIT_BPM, f_bpm != 3, f_bpm == 3 ? "BPM any" : "BPM +-%s%%", BPM_MODES[f_bpm]);
    ADD(y1, HIT_ENERGY, f_energy != 0, "ENERGY %s", EN[f_energy]);
    ADD(y1, HIT_STARS, f_stars != 0, f_stars ? "STARS %d+" : "STARS any", STAR_MODES[f_stars]);
    x = 12;
    if (n_groups > 0) {
        int g = f_group < n_groups ? f_group : 0;
        ADD(y2, HIT_GROUP, f_tag[g][0] != 0, "%s >", g_label[g]);
        for (int i = 0; i < g_ntags[g]; i++)
            ADD(y2, HIT_TAG + i, !strcmp(f_tag[g], g_tag[g][i]), "%s", g_tag[g][i]);
        int any = 0;
        for (int k = 0; k < n_groups; k++)
            any |= f_tag[k][0] != 0;
        if (any)
            ADD(y2, HIT_CLEAR, 0, "CLEAR");
    }
#undef ADD
    return n;
}

/* draw_text, cut to fit inside a (with "~" when cut), and nothing at all below its bottom. */
static void text_to(az_draw *d, az_rect a, int x, int y, const char *s, int px, uint32_t rgb,
                    int right)
{
    if (y < a.y || y + px * 3 / 2 > a.y + a.h || x >= right)
        return;
    char buf[160];
    snprintf(buf, sizeof buf, "%s", s);
    size_t n = strlen(buf);
    while (n > 0 && x + C->text_width(buf, px) > right) {
        buf[--n] = 0;
        if (n > 0)
            buf[n - 1] = '~';
    }
    if (n)
        C->draw_text(d, x, y, buf, px, rgb);
}

static void text_in(az_draw *d, az_rect a, int x, int y, const char *s, int px, uint32_t rgb)
{
    text_to(d, a, x, y, s, px, rgb, a.x + a.w - 8);
}

/* ---- the waveform on each row, and the finger on it ----------------------------------------- */

/* Where the waveform of a row sits, relative to the panel's top left. */
static int wave_x(void) { return PANEL_W - 14 - WAVE_W; }
static int wave_y(int row) { return HEAD_H + 2 * FILT_H + 6 + row * ROW_H + 3; }


static void wave_paint(az_draw *d, int x, int y, const row_t *r, uint32_t bg, int cursor)
{
    static unsigned char px[WAVE_W * WAVE_H * 4];   /* on_paint is one thread: one buffer */
    static const unsigned char COL[3][3] = { { 32, 96, 255 }, { 255, 160, 32 }, { 255, 255, 255 } };
    for (int i = 0; i < WAVE_W * WAVE_H; i++) {
        px[i * 4 + 0] = (unsigned char)(bg >> 16);
        px[i * 4 + 1] = (unsigned char)(bg >> 8);
        px[i * 4 + 2] = (unsigned char)bg;
        px[i * 4 + 3] = 255;
    }
    for (int c = 0; c < WAVE_COLS; c++) {
        /* the tallest band first, so the smaller ones stay visible in front of it */
        int order[3] = { 0, 1, 2 };
        for (int i = 0; i < 2; i++)
            for (int k = i + 1; k < 3; k++)
                if (r->wave[c][order[k]] > r->wave[c][order[i]]) {
                    int t = order[i];
                    order[i] = order[k];
                    order[k] = t;
                }
        for (int o = 0; o < 3; o++) {
            int b = order[o], h = r->wave[c][b] * WAVE_H / 15;
            for (int yy = WAVE_H - h; yy < WAVE_H; yy++)
                for (int xx = c * 2; xx < c * 2 + 2; xx++) {
                    unsigned char *q = px + (yy * WAVE_W + xx) * 4;
                    q[0] = COL[b][0];
                    q[1] = COL[b][1];
                    q[2] = COL[b][2];
                }
        }
    }
    if (cursor >= 0) {
        int cx = cursor * (WAVE_W - 1) / 1000;
        for (int yy = 0; yy < WAVE_H; yy++)
            for (int xx = cx - 1; xx <= cx + 1; xx++)
                if (xx >= 0 && xx < WAVE_W) {
                    unsigned char *q = px + (yy * WAVE_W + xx) * 4;
                    q[0] = 255;
                    q[1] = 40;
                    q[2] = 40;
                }
    }
    C->draw_image(d, x, y, px, WAVE_W, WAVE_H);
}

static void paint(az_draw *d, az_rect a, void *user)
{
    (void)user;
    uint32_t bg = C->color(AZ_COL_BG), text = C->color(AZ_COL_TEXT);
    uint32_t dim = C->color(AZ_COL_TEXT_DIM), hi = C->color(AZ_COL_HI), hit = C->color(AZ_COL_HI_TEXT);
    row_t r[ROWS];
    box_t bx[24];
    int n, nb, deck, match, sel;
    char now[64], nt[96];
    pthread_mutex_lock(&mu);
    memcpy(r, rows, sizeof r);
    n = n_rows;
    match = n_match;
    deck = shown_deck;
    sel = sel_row < n_rows ? sel_row : -1;
    snprintf(now, sizeof now, "%s", now_title);
    snprintf(nt, sizeof nt, "%s", note);
    nb = expanded ? layout(bx, 24) : 0;
    pthread_mutex_unlock(&mu);

    C->fill_rect(d, a.x, a.y, a.w, a.h, bg);
    C->draw_rect(d, a.x, a.y, a.w, a.h, 2, hi);
    if (!expanded) {
        char t[24];
        snprintf(t, sizeof t, n ? "NEXT  %d" : "NEXT", n);
        text_in(d, a, a.x + 16, a.y + 10, t, 18, hi);
        return;
    }
    char h[160];
    C->fill_rect(d, a.x, a.y, a.w, HEAD_H, hi);
    /* the close button: folds the panel into the NEXT tab */
    int cx = a.x + a.w - CLOSE_W;
    C->fill_rect(d, cx, a.y, CLOSE_W, HEAD_H, bg);
    C->draw_line(d, cx + 18, a.y + 10, cx + CLOSE_W - 18, a.y + HEAD_H - 10, 3, hi);
    C->draw_line(d, cx + 18, a.y + HEAD_H - 10, cx + CLOSE_W - 18, a.y + 10, 3, hi);
    int right = cx - 12;
    if (match >= 0) {
        char m[24];
        snprintf(m, sizeof m, "%d match", match);
        int w = C->text_width(m, 15);
        text_in(d, a, right - w, a.y + 12, m, 15, hit);
        right -= w + 16;
    }
    if (sel >= 0 && r[sel].search[0]) {
        /* a row was tapped: what to type in the deck's SEARCH, big */
        snprintf(h, sizeof h, "SEARCH  %s", r[sel].search);
        text_to(d, a, a.x + 14, a.y + 6, h, 22, hit, right);
        int w = C->text_width(h, 22) + 24;
        snprintf(h, sizeof h, "%s - %s", r[sel].title, r[sel].artist);
        text_to(d, a, a.x + 14 + w, a.y + 12, h, 15, hit, right);
    } else {
        if (deck >= 0 && now[0])
            snprintf(h, sizeof h, "NEXT AFTER DECK %d: %s", deck + 1, now);
        else
            snprintf(h, sizeof h, "NEXT");
        text_to(d, a, a.x + 14, a.y + 10, h, 17, hit, right);
    }
    for (int i = 0; i < nb; i++) {
        box_t *b = &bx[i];
        if (b->on)
            C->fill_rect(d, a.x + b->x, a.y + b->y, b->w, b->h, hi);
        else
            C->draw_rect(d, a.x + b->x, a.y + b->y, b->w, b->h, 2, b->what == HIT_GROUP ? hi : dim);
        text_in(d, a, a.x + b->x + 14, a.y + b->y + (b->h - 24) / 2, b->text, 16, b->on ? hit : text);
    }
    int y0 = a.y + HEAD_H + 2 * FILT_H + 6;
    C->draw_line(d, a.x + 8, y0 - 3, a.x + a.w - 8, y0 - 3, 1, dim);
    if (!n) {
        text_in(d, a, a.x + 14, y0 + 12, nt, 17, dim);
        return;
    }
    for (int i = 0; i < n; i++) {
        int y = y0 + i * ROW_H;
        /* fit: a bar of five, as in Sets */
        int segs = (r[i].fit + 10) / 20;
        for (int k = 0; k < 5; k++) {
            int sx = a.x + 14 + k * 11;
            if (k < segs)
                C->fill_rect(d, sx, y + 8, 8, 16, hi);
            else
                C->draw_rect(d, sx, y + 8, 8, 16, 1, dim);
        }
        char s[160];
        snprintf(s, sizeof s, "%d", r[i].fit);
        text_in(d, a, a.x + 74, y + 4, s, 14, dim);
        char st[6] = "-----";
        for (int k = 0; k < r[i].stars && k < 5; k++)
            st[k] = '*';
        text_in(d, a, a.x + 112, y + 4, st, 16, r[i].stars ? text : dim);
        text_in(d, a, a.x + 186, y + 4, r[i].bpm, 16, hi);
        text_in(d, a, a.x + 252, y + 4, r[i].key, 16, hi);
        int right = r[i].has_wave ? a.x + wave_x() - 12 : a.x + a.w - 8;
        snprintf(s, sizeof s, "%s - %s", r[i].title, r[i].artist);
        text_to(d, a, a.x + 304, y + 2, s, 17, text, right);
        /* the letters to type in the deck's SEARCH, then genre and why */
        int sx = a.x + 304;
        if (r[i].search[0]) {
            snprintf(s, sizeof s, "SEARCH %s", r[i].search);
            text_to(d, a, sx, y + 24, s, 12, hi, right);
            sx += C->text_width(s, 12) + 12;
        }
        snprintf(s, sizeof s, "%s  |  %s", r[i].genre, r[i].why);
        text_to(d, a, sx, y + 24, s, 12, dim, right);
        if (r[i].has_wave) {
            /* the shape only: the panel cannot play sound (preview in the deck's browser) */
            wave_paint(d, a.x + wave_x(), a.y + wave_y(i), &r[i], bg, -1);
            if (r[i].secs > 0) {
                snprintf(s, sizeof s, "%d:%02d", r[i].secs / 60, r[i].secs % 60);
                int w = C->text_width(s, 12);
                C->fill_rect(d, a.x + wave_x() + WAVE_W - w - 6, a.y + wave_y(i), w + 6, 16, bg);
                text_in(d, a, a.x + wave_x() + WAVE_W - w - 3, a.y + wave_y(i) - 1, s, 12, dim);
            }
        }
        if (i == sel)
            C->draw_rect(d, a.x + 6, y, a.w - 12, ROW_H, 2, hi);
    }
}

/* Ask again for the last load, with the filters as they are now. UI thread. */
static void ask_again(void)
{
    if (!have_last)
        return;
    job_t j = last_job;
    pthread_mutex_lock(&mu);
    fill_filters(&j.f);
    pthread_mutex_unlock(&mu);
    push_job(&j);
}

static int surface_input(const az_event *ev, void *user)
{
    (void)user;
    if (ev->kind != AZ_EV_TAP)
        return ev->kind == AZ_EV_DOWN || ev->kind == AZ_EV_UP || ev->kind == AZ_EV_LONG;
    touched_at = now_s();
    if (!expanded) {
        expand(1);
        return 1;
    }
    int x = ev->x - panel_rect.x, y = ev->y - panel_rect.y;
    if (y < HEAD_H) {
        /* the close button, or the header: fold into the NEXT tab */
        expand(0);
        return 1;
    }
    int y0 = HEAD_H + 2 * FILT_H + 6;
    if (y >= y0) {
        /* a row: its SEARCH letters go big in the header (tap again to go back) */
        int i = (y - y0) / ROW_H;
        pthread_mutex_lock(&mu);
        if (i < n_rows)
            sel_row = sel_row == i ? -1 : i;
        pthread_mutex_unlock(&mu);
        C->surface_redraw(panel);
        return 1;
    }
    (void)x;
    box_t bx[24];
    int changed = 0;
    pthread_mutex_lock(&mu);
    int nb = layout(bx, 24), g = f_group < n_groups ? f_group : 0;
    for (int i = 0; i < nb; i++) {
        box_t *b = &bx[i];
        if (x < b->x || x >= b->x + b->w || y < b->y || y >= b->y + b->h)
            continue;
        switch (b->what) {
        case HIT_KEY: f_key = (f_key + 1) % 3; break;
        case HIT_BPM: f_bpm = (f_bpm + 1) % 4; break;
        case HIT_ENERGY: f_energy = (f_energy + 1) % 4; break;
        case HIT_STARS: f_stars = (f_stars + 1) % 4; break;
        case HIT_GROUP: f_group = n_groups ? (g + 1) % n_groups : 0; break;
        case HIT_CLEAR: memset(f_tag, 0, sizeof f_tag); break;
        default:
            if (b->what >= HIT_TAG && b->what - HIT_TAG < g_ntags[g]) {
                const char *t = g_tag[g][b->what - HIT_TAG];
                if (!strcmp(f_tag[g], t))
                    f_tag[g][0] = 0;
                else
                    snprintf(f_tag[g], sizeof f_tag[g], "%s", t);
            }
        }
        changed = b->what != HIT_GROUP;
        break;
    }
    pthread_mutex_unlock(&mu);
    if (changed) {
        save_filters();
        ask_again();
    }
    C->surface_redraw(panel);
    return 1;
}

/* ---- what the decks are doing ------------------------------------------------------------ */

typedef struct {
    char title[128];
    unsigned id;
    double played;     /* seconds it has played */
    int reported;
} deck_track_t;
static deck_track_t dt[4];

static void fill_job(job_t *j, int kind, int deck, const az_deck_info *di)
{
    memset(j, 0, sizeof *j);
    j->kind = kind;
    j->deck = deck;
    j->id = di->content_id;
    snprintf(j->title, sizeof j->title, "%s", di->title);
    snprintf(j->artist, sizeof j->artist, "%s", di->artist);
}

static void on_deck(int deck, int what, void *user)
{
    (void)user;
    if (what != AZ_DECK_LOADED || deck < 0 || deck > 3 || value(row_on, 1) != 1)
        return;
    az_deck_info di = { .size = sizeof di };
    if (C->deck_info(deck, &di) != 0 || !di.loaded)
        return;
    job_t j;
    fill_job(&j, JOB_SUGGEST, deck, &di);
    /* the other deck that is playing: not worth suggesting what is already on */
    for (int o = 0; o < 4; o++) {
        az_deck_info od = { .size = sizeof od };
        if (o != deck && C->deck_info(o, &od) == 0 && od.loaded && od.playing == 1) {
            j.other_id = od.content_id;
            snprintf(j.other_title, sizeof j.other_title, "%s", od.title);
            snprintf(j.other_artist, sizeof j.other_artist, "%s", od.artist);
            break;
        }
    }
    pthread_mutex_lock(&mu);
    fill_filters(&j.f);
    pthread_mutex_unlock(&mu);
    last_job = j;
    have_last = 1;
    push_job(&j);
    pthread_mutex_lock(&mu);
    shown_deck = deck;
    n_rows = 0;
    n_match = -1;
    snprintf(now_title, sizeof now_title, "%.63s", di.title);
    snprintf(note, sizeof note, "working it out...");
    pthread_mutex_unlock(&mu);
    atomic_store(&fresh, 1);
}

/* Once a second: count how long each deck's track has played; at 45 s it counts as played
 * this session (no repeats, and the set so far shapes the suggestions). */
static void watch_plays(double dt_s)
{
    for (int i = 0; i < 4; i++) {
        az_deck_info di = { .size = sizeof di };
        if (C->deck_info(i, &di) != 0 || !di.loaded)
            continue;
        deck_track_t *t = &dt[i];
        if (strcmp(t->title, di.title) != 0 || t->id != di.content_id) {
            snprintf(t->title, sizeof t->title, "%s", di.title);
            t->id = di.content_id;
            t->played = 0;
            t->reported = 0;
        }
        if (di.playing == 1)
            t->played += dt_s;
        if (!t->reported && t->played >= PLAYED_AFTER_S) {
            t->reported = 1;
            job_t j;
            fill_job(&j, JOB_PLAYED, i, &di);
            push_job(&j);
        }
    }
}

/* ---- Mod Menu ------------------------------------------------------------------------------ */

static void on_toggle(int v, void *user)
{
    (void)user;
    if (!v)
        set_visible(0);
}

static void on_pos(int v, void *user)
{
    (void)v;
    (void)user;
    place();
}

static void reload(void *user)
{
    (void)user;
    job_t j = { .kind = JOB_LOAD };
    push_job(&j);
}

static void page_draw(az_draw *d, az_rect a, void *user)
{
    (void)user;
    char st[120], cp[400], dg[4][200];
    pthread_mutex_lock(&mu);
    snprintf(st, sizeof st, "%s", data_state);
    memcpy(dg, diag, sizeof dg);
    pthread_mutex_unlock(&mu);
    snprintf(cp, sizeof cp, "%s", data_path[0] ? data_path : "(the drive is not in the deck)");
    int lh = C->row_height(d);
    C->draw_header(d, "Deck Suggestions");
    text_in(d, a, a.x + 24, a.y + lh + 20, "Deck data:", 20, C->color(AZ_COL_TEXT_DIM));
    text_in(d, a, a.x + 24, a.y + lh + 52, st, 20, C->color(AZ_COL_TEXT));
    text_in(d, a, a.x + 24, a.y + lh + 100, "File:", 20, C->color(AZ_COL_TEXT_DIM));
    text_in(d, a, a.x + 24, a.y + lh + 132, cp, 16, C->color(AZ_COL_TEXT));
    for (int i = 0; i < 4; i++)
        text_in(d, a, a.x + 24, a.y + lh + 176 + i * 30, dg[i], 15, C->color(AZ_COL_TEXT));
    text_in(d, a, a.x + 24, a.y + lh + 310,
            "After each export from rekordbox, run Backspins' deck export to this drive.", 16,
            C->color(AZ_COL_TEXT_DIM));
}

static int page_input(const az_event *ev, void *user)
{
    (void)ev;
    (void)user;
    return 0;
}

static void register_ui(void)
{
    static const az_screen_v1 page = { .size = sizeof(az_screen_v1), .on_draw = page_draw,
                                       .on_input = page_input };
    C->register_screen(MOD_ID, &page);
    if (!AZ_HAS(C, register_row))
        return;
    static const az_row_v1 on = {
        .size = sizeof(az_row_v1), .key = "on", .label = "Deck Suggestions",
        .kind = AZ_ROW_TOGGLE, .flags = AZ_ROW_DEFAULT_ON,
        .help = "When a deck loads a track, the six best tracks to play next, worked out on "
                "the deck from what Backspins wrote to this drive. No computer needed.",
        .on_change = on_toggle,
    };
    static const az_row_v1 pos = {
        .size = sizeof(az_row_v1), .key = "pos", .label = "Panel position", .kind = AZ_ROW_CHOICE,
        .parent = "on", .help = "Where the suggestions panel sits on the deck screen.",
        .choices = POSITIONS, .n_choices = 3, .default_choice = 1, .on_change = on_pos,
    };
    static const az_row_v1 hold = {
        .size = sizeof(az_row_v1), .key = "hold", .label = "Keep it open", .kind = AZ_ROW_CHOICE,
        .parent = "on", .help = "How long the panel stays open after a load before it folds "
                                "into the small NEXT tab. Tap the tab to open it again.",
        .choices = HOLDS, .n_choices = 4, .default_choice = 1,
    };
    static const az_row_v1 status = {
        .size = sizeof(az_row_v1), .key = "status", .label = "Status", .kind = AZ_ROW_SCREEN,
        .parent = "on", .help = "Whether Backspins' deck data was found on the drive, and how much.",
    };
    static const az_row_v1 again = {
        .size = sizeof(az_row_v1), .key = "reload", .label = "Reload deck data",
        .kind = AZ_ROW_ACTION, .parent = "on",
        .help = "Read BACKSPINS/deck-data.tsv again (it is read when the deck starts).",
        .on_activate = reload,
    };
    row_on = C->register_row(MOD_ID, &on);
    row_pos = C->register_row(MOD_ID, &pos);
    row_hold = C->register_row(MOD_ID, &hold);
    C->register_row(MOD_ID, &status);
    C->register_row(MOD_ID, &again);
}

/* ---- the mod --------------------------------------------------------------------------------- */

static int init(const az_core_v1 *core)
{
    C = core;
    if (!AZ_HAS(C, surface_create) || !AZ_HAS(C, deck_info) || !AZ_HAS(C, on_deck_event) ||
        !AZ_HAS(C, thread_start) || !AZ_HAS(C, data_dir)) {
        C->log(MOD_ID, "this core is older than 1.2.3; nothing to do");
        return 0;
    }
    /* The stick's root, from our data folder: <stick>/MODS/community-mods/<id>/data */
    const char *dd = C->data_dir(MOD_ID);
    if (dd) {
        char root[300];
        snprintf(root, sizeof root, "%s", dd);
        char *m = strstr(root, "/MODS/");
        if (m) {
            *m = 0;
            snprintf(drive_root, sizeof drive_root, "%s", root);
            snprintf(data_path, sizeof data_path, "%s/BACKSPINS/deck-data.tsv", root);
        }
    }
    register_ui();
    load_filters();

    static const az_surface_v1 desc = {
        .size = sizeof(az_surface_v1),
        .layer = AZ_LAYER_OVER_EP147,
        .views = AZ_VIEW_BIT(AZ_VIEW_DECK),
        .on_paint = paint,
        .on_input = surface_input,
    };
    panel = C->surface_create(MOD_ID, &desc);
    if (!panel)
        return 1; /* the reason is in the log */
    place();
    if (C->on_deck_event(MOD_ID, on_deck, NULL) != 0)
        return 2;
    if (C->thread_start(MOD_ID, "deck-suggest", worker, NULL) != 0)
        return 3;
    if (AZ_HAS(C, lib_tracks) && C->thread_start(MOD_ID, "deck-library", mapper, NULL) != 0)
        C->log(MOD_ID, "no thread for reading the drive's library; tracks are found by title");
    job_t l = { .kind = JOB_LOAD };
    push_job(&l);
    return 0;
}

static void tick(void)
{
    if (!panel)
        return;
    double t = now_s();
    if (atomic_exchange(&fresh, 0) && value(row_on, 1) == 1) {
        set_visible(1);
        expand(1);
    }
    if (visible && value(row_on, 1) != 1)
        set_visible(0);
    if (atomic_exchange(&regrouped, 0) && visible)
        C->surface_redraw(panel);
    int h = value(row_hold, 1);
    double hold = HOLD_S[h >= 0 && h < 4 ? h : 1];
    if (expanded && hold > 0 && t - touched_at > hold)
        expand(0);
    static double last;
    if (last == 0)
        last = t;
    if (t - last >= 1.0) {
        watch_plays(t - last);
        last = t;
    }
}

static const char *const TAGS[] = { "any", NULL };
static const az_mod_v1 MOD = { AZ_MOD_ABI, sizeof(az_mod_v1), MOD_ID, TAGS, init, tick, NULL };

AZ_MOD_EXPORT const az_mod_v1 *az_mod_v1_entry(void)
{
    return &MOD;
}
