/* SPDX-License-Identifier: GPL-3.0-only
 *
 * deck-suggest: what to play next, on the deck, from Backspins on your computer.
 *
 * When a deck loads a track (from a USB stick or from rekordbox over Pro DJ Link), the mod
 * asks Backspins (the rekordbox toolkit, decklink.py) what fits after it and shows the five
 * best follow-ups in a panel on the deck screen: BPM, key, title, artist and why. When a
 * track has played for 45 seconds it tells Backspins, so what you really play on the deck
 * teaches the suggestions.
 *
 * Where Backspins is: <stick>/BACKSPINS/deck-link.cfg, written by Backspins (host=, port=,
 * key=). If none of its hosts answers, the mod asks the network (UDP broadcast "BACKSPINS?"
 * on the port) and Backspins answers "BACKSPINS <port>".
 *
 * Threads: init(), tick(), the rows, the page and the deck events run on the core's UI
 * thread and never touch the network: they queue jobs. One thread of ours does the network.
 * The answer is handed back under a mutex; on_paint (the overlay thread) copies it under the
 * same mutex.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "azmod-mod.h"

#ifndef MSG_NOSIGNAL /* the test harness builds on macOS too */
#define MSG_NOSIGNAL 0
#endif

#define MOD_ID "deck-suggest"
#define ROWS 5
#define MAX_HOSTS 6
#define PLAYED_AFTER_S 45.0
#define PANEL_W 760
#define ROW_H 40
#define HEAD_H 40
#define PANEL_H (HEAD_H + ROWS * ROW_H + 10)
#define TAB_W 150
#define TAB_H 44

static const az_core_v1 *C;

/* ---- what the panel shows (guarded by mu) ---------------------------------------------- */

typedef struct {
    char bpm[8], key[6], title[64], artist[44], why[68];
} row_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int shown_deck = -1;          /* 0..3, the deck these suggestions are for */
static char now_title[64];
static row_t rows[ROWS];
static int n_rows;
static char note[96] = "Load a track";  /* a status line when there are no rows */
static char link_state[96] = "not connected yet";
static atomic_int fresh;             /* the thread has a new answer for tick to show */

/* ---- jobs for the network thread (guarded by mu) --------------------------------------- */

enum { JOB_SUGGEST = 1, JOB_PLAYED };
typedef struct {
    int kind, deck;
    unsigned id;
    char title[128], artist[128], key[8];
    double bpm;
    unsigned other_id;
    char other_title[128];
} job_t;

static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static job_t jobs[8];
static int job_head, job_n;
static atomic_int reload_cfg = 1;

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

/* ---- the link: where Backspins is ------------------------------------------------------ */

static char cfg_path[400];           /* set in init(): <stick>/BACKSPINS/deck-link.cfg */
static char hosts[MAX_HOSTS][48];
static int n_hosts, port = 47866, good_host = -1;
static char key[64];
static char found_ip[48];            /* from the broadcast answer */

static void set_link(const char *s)
{
    pthread_mutex_lock(&mu);
    snprintf(link_state, sizeof link_state, "%s", s);
    pthread_mutex_unlock(&mu);
}

static void read_cfg(void)
{
    n_hosts = 0;
    key[0] = 0;
    good_host = -1;
    found_ip[0] = 0;
    FILE *f = cfg_path[0] ? fopen(cfg_path, "r") : NULL;
    if (!f) {
        set_link("no BACKSPINS/deck-link.cfg on the stick: write it from Backspins");
        return;
    }
    char line[160];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "host=", 5) && n_hosts < MAX_HOSTS)
            snprintf(hosts[n_hosts++], sizeof hosts[0], "%s", line + 5);
        else if (!strncmp(line, "port=", 5))
            port = atoi(line + 5);
        else if (!strncmp(line, "key=", 4))
            snprintf(key, sizeof key, "%s", line + 4);
    }
    fclose(f);
    if (port <= 0 || port > 65535)
        port = 47866;
    set_link(key[0] ? "link file read" : "deck-link.cfg has no key: write it again from Backspins");
}

/* A TCP connection to ip:port within ms milliseconds, or -1. */
static int dial(const char *ip, int prt, int ms)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((unsigned short)prt) };
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1)
        return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS) {
        close(s);
        return -1;
    }
    struct pollfd p = { .fd = s, .events = POLLOUT };
    int err = 0;
    socklen_t el = sizeof err;
    if (poll(&p, 1, ms) != 1 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
        close(s);
        return -1;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) & ~O_NONBLOCK);
    struct timeval tv = { 3, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return s;
}

/* Ask the network where Backspins is: UDP broadcast, first answer wins. */
static int discover(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((unsigned short)port),
                             .sin_addr.s_addr = htonl(INADDR_BROADCAST) };
    sendto(s, "BACKSPINS?", 10, 0, (struct sockaddr *)&a, sizeof a);
    struct pollfd p = { .fd = s, .events = POLLIN };
    int rc = -1;
    if (poll(&p, 1, 1200) == 1) {
        char buf[64];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n > 10 && !strncmp(buf, "BACKSPINS ", 10)) {
            buf[n] = 0;
            int pr = atoi(buf + 10);
            if (pr > 0 && pr < 65536)
                port = pr;
            inet_ntop(AF_INET, &from.sin_addr, found_ip, sizeof found_ip);
            rc = 0;
        }
    }
    close(s);
    return rc;
}

static int connect_backspins(void)
{
    if (good_host >= 0) {
        int s = dial(hosts[good_host], port, 1500);
        if (s >= 0)
            return s;
    }
    if (found_ip[0]) {
        int s = dial(found_ip, port, 1500);
        if (s >= 0)
            return s;
    }
    for (int i = 0; i < n_hosts; i++) {
        int s = dial(hosts[i], port, 1200);
        if (s >= 0) {
            good_host = i;
            return s;
        }
    }
    if (discover() == 0) {
        int s = dial(found_ip, port, 1500);
        if (s >= 0)
            return s;
    }
    return -1;
}

/* key=value lines; tabs and newlines in values would break the format, so they go. */
static void put(char *out, size_t cap, const char *k, const char *v)
{
    size_t len = strlen(out);
    if (len + strlen(k) + 3 >= cap)
        return;
    len += (size_t)snprintf(out + len, cap - len, "%s=", k);
    for (; *v && len + 2 < cap; v++)
        out[len++] = (*v == '\n' || *v == '\r' || *v == '\t') ? ' ' : *v;
    out[len++] = '\n';
    out[len] = 0;
}

/* POST path with body; the response body (after the headers) into resp. 0 = 200 OK. */
static int post(const char *path, const char *body, char *resp, size_t cap)
{
    int s = connect_backspins();
    if (s < 0)
        return -1;
    char head[256];
    int hl = snprintf(head, sizeof head,
                      "POST %s HTTP/1.0\r\nHost: backspins\r\nX-Deck-Key: %s\r\n"
                      "Content-Type: text/plain\r\nContent-Length: %zu\r\n\r\n",
                      path, key, strlen(body));
    int ok = send(s, head, (size_t)hl, MSG_NOSIGNAL) == hl &&
             send(s, body, strlen(body), MSG_NOSIGNAL) == (ssize_t)strlen(body);
    size_t got = 0;
    while (ok && got + 1 < cap) {
        ssize_t n = recv(s, resp + got, cap - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(s);
    resp[got] = 0;
    if (!ok || got < 12)
        return -2;
    int code = atoi(resp + 9); /* "HTTP/1.0 200" */
    char *b = strstr(resp, "\r\n\r\n");
    if (b)
        memmove(resp, b + 4, strlen(b + 4) + 1);
    return code == 200 ? 0 : code;
}

static void copy_field(char *dst, size_t cap, const char *src, const char *end)
{
    size_t n = (size_t)(end - src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* The answer: "ok 1", "now\ttitle\tartist", then "s\tbpm\tkey\ttitle\tartist\twhy" rows,
 * or "ok 0" and "msg <text>". */
static void take_answer(int deck, char *body)
{
    row_t got[ROWS];
    int n = 0;
    char now[64] = "", msg[96] = "";
    char *save = NULL;
    for (char *line = strtok_r(body, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "now\t", 4)) {
            char *e = strchr(line + 4, '\t');
            copy_field(now, sizeof now, line + 4, e ? e : line + strlen(line));
        } else if (!strncmp(line, "msg ", 4)) {
            snprintf(msg, sizeof msg, "%s", line + 4);
        } else if (!strncmp(line, "s\t", 2) && n < ROWS) {
            char *f[6] = { 0 };
            char *p = line + 2;
            for (int i = 0; i < 5 && p; i++) {
                f[i] = p;
                p = strchr(p, '\t');
                if (p)
                    *p++ = 0;
            }
            if (!f[4])
                continue;
            row_t *r = &got[n++];
            snprintf(r->bpm, sizeof r->bpm, "%s", f[0]);
            snprintf(r->key, sizeof r->key, "%s", f[1]);
            snprintf(r->title, sizeof r->title, "%s", f[2]);
            snprintf(r->artist, sizeof r->artist, "%s", f[3]);
            snprintf(r->why, sizeof r->why, "%s", f[4]);
        }
    }
    pthread_mutex_lock(&mu);
    shown_deck = deck;
    snprintf(now_title, sizeof now_title, "%s", now);
    memcpy(rows, got, sizeof got);
    n_rows = n;
    snprintf(note, sizeof note, "%s", n ? "" : (msg[0] ? msg : "nothing fits yet"));
    pthread_mutex_unlock(&mu);
    atomic_store(&fresh, 1);
}

static void run_job(const job_t *j)
{
    char body[1024] = "", resp[4096];
    char num[32];
    snprintf(num, sizeof num, "%d", j->deck);
    put(body, sizeof body, "deck", num);
    if (j->id) {
        snprintf(num, sizeof num, "%u", j->id);
        put(body, sizeof body, "id", num);
    }
    put(body, sizeof body, "title", j->title);
    put(body, sizeof body, "artist", j->artist);
    put(body, sizeof body, "key", j->key);
    snprintf(num, sizeof num, "%.2f", j->bpm);
    put(body, sizeof body, "bpm", num);
    if (j->kind == JOB_SUGGEST) {
        if (j->other_id) {
            snprintf(num, sizeof num, "%u", j->other_id);
            put(body, sizeof body, "other_id", num);
        }
        if (j->other_title[0])
            put(body, sizeof body, "other_title", j->other_title);
    }
    int rc = post(j->kind == JOB_SUGGEST ? "/deck/suggest" : "/deck/played", body, resp, sizeof resp);
    char st[96];
    if (rc == 0) {
        snprintf(st, sizeof st, "connected to %s:%d", good_host >= 0 ? hosts[good_host] : found_ip, port);
        set_link(st);
        if (j->kind == JOB_SUGGEST)
            take_answer(j->deck, resp);
    } else {
        if (rc == -1)
            snprintf(st, sizeof st, "Backspins not found on the network (is Deck link on?)");
        else if (rc == 403)
            snprintf(st, sizeof st, "Backspins refused the key: write the stick again");
        else
            snprintf(st, sizeof st, "Backspins answered %d", rc);
        set_link(st);
        if (j->kind == JOB_SUGGEST) {
            pthread_mutex_lock(&mu);
            shown_deck = j->deck;
            n_rows = 0;
            snprintf(note, sizeof note, "%s", st);
            pthread_mutex_unlock(&mu);
            atomic_store(&fresh, 1);
        }
    }
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
        if (atomic_exchange(&reload_cfg, 0))
            read_cfg();
        if (key[0])
            run_job(&j);
        else
            set_link("no link file on the stick: write it from Backspins");
    }
    return NULL;
}

/* ---- the panel --------------------------------------------------------------------------- */

static az_surface *panel;
static int row_on = -1, row_pos = -1, row_hold = -1;
static const char *const POSITIONS[] = { "Top", "Middle", "Bottom" };
static const char *const HOLDS[] = { "10 s", "20 s", "40 s", "Until tapped" };
static const double HOLD_S[] = { 10, 20, 40, 0 };
static int expanded, visible;
static double expanded_at;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int value(int row, int def)
{
    return row < 0 ? def : C->row_value(row);
}

static void place(void)
{
    if (!panel)
        return;
    static const int Y[] = { 96, 300, 520 };
    int p = value(row_pos, 1);
    int y = Y[p >= 0 && p < 3 ? p : 1];
    if (expanded)
        C->surface_place(panel, (az_rect){ (1280 - PANEL_W) / 2, y, PANEL_W, PANEL_H });
    else
        C->surface_place(panel, (az_rect){ 1280 - TAB_W - 16, y, TAB_W, TAB_H });
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
    expanded_at = now_s();
    place();
}

/* draw_text, cut to fit inside a (with "~" when cut), and nothing at all below its bottom. */
static void text_in(az_draw *d, az_rect a, int x, int y, const char *s, int px, uint32_t rgb)
{
    int right = a.x + a.w - 8;
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

static void paint(az_draw *d, az_rect a, void *user)
{
    (void)user;
    uint32_t bg = C->color(AZ_COL_BG), text = C->color(AZ_COL_TEXT);
    uint32_t dim = C->color(AZ_COL_TEXT_DIM), hi = C->color(AZ_COL_HI);
    row_t r[ROWS];
    int n, deck;
    char now[64], nt[96];
    pthread_mutex_lock(&mu);
    memcpy(r, rows, sizeof r);
    n = n_rows;
    deck = shown_deck;
    snprintf(now, sizeof now, "%s", now_title);
    snprintf(nt, sizeof nt, "%s", note);
    pthread_mutex_unlock(&mu);

    C->fill_rect(d, a.x, a.y, a.w, a.h, bg);
    C->draw_rect(d, a.x, a.y, a.w, a.h, 2, hi);
    if (!expanded) {
        char t[24];
        snprintf(t, sizeof t, n ? "NEXT  %d" : "NEXT", n);
        text_in(d, a, a.x + 16, a.y + 10, t, 18, hi);
        return;
    }
    char h[128];
    if (deck >= 0 && now[0])
        snprintf(h, sizeof h, "NEXT AFTER DECK %d: %s", deck + 1, now);
    else
        snprintf(h, sizeof h, "NEXT");
    C->fill_rect(d, a.x, a.y, a.w, HEAD_H, hi);
    text_in(d, a, a.x + 14, a.y + 10, h, 17, C->color(AZ_COL_HI_TEXT));
    if (!n) {
        text_in(d, a, a.x + 14, a.y + HEAD_H + 16, nt, 17, dim);
        return;
    }
    for (int i = 0; i < n; i++) {
        int y = a.y + HEAD_H + 6 + i * ROW_H;
        char s[140];
        text_in(d, a, a.x + 14, y + 2, r[i].bpm, 17, hi);
        text_in(d, a, a.x + 84, y + 2, r[i].key, 17, hi);
        snprintf(s, sizeof s, "%s - %s", r[i].title, r[i].artist);
        text_in(d, a, a.x + 140, y + 1, s, 17, text);
        text_in(d, a, a.x + 140, y + 22, r[i].why, 12, dim);
    }
}

static int surface_input(const az_event *ev, void *user)
{
    (void)user;
    if (ev->kind == AZ_EV_TAP) {
        expand(!expanded);
        return 1;
    }
    return ev->kind == AZ_EV_DOWN || ev->kind == AZ_EV_UP || ev->kind == AZ_EV_LONG;
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
    j->bpm = di->bpm;
    snprintf(j->title, sizeof j->title, "%s", di->title);
    snprintf(j->artist, sizeof j->artist, "%s", di->artist);
    snprintf(j->key, sizeof j->key, "%s", di->key);
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
            break;
        }
    }
    push_job(&j);
    pthread_mutex_lock(&mu);
    shown_deck = deck;
    n_rows = 0;
    snprintf(now_title, sizeof now_title, "%s", di.title);
    snprintf(note, sizeof note, "asking Backspins...");
    pthread_mutex_unlock(&mu);
    atomic_store(&fresh, 1);
}

/* Once a second: count how long each deck's track has played; at 45 s, report the play. */
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

static void reconnect(void *user)
{
    (void)user;
    atomic_store(&reload_cfg, 1);
    set_link("will read the link file again on the next load");
}

static void page_draw(az_draw *d, az_rect a, void *user)
{
    (void)user;
    char st[96], cp[400];
    pthread_mutex_lock(&mu);
    snprintf(st, sizeof st, "%s", link_state);
    pthread_mutex_unlock(&mu);
    snprintf(cp, sizeof cp, "%s", cfg_path[0] ? cfg_path : "(no stick)");
    int lh = C->row_height(d);
    C->draw_header(d, "Deck Suggestions");
    text_in(d, a, a.x + 24, a.y + lh + 20, "Link to Backspins:", 20, C->color(AZ_COL_TEXT_DIM));
    text_in(d, a, a.x + 24, a.y + lh + 52, st, 20, C->color(AZ_COL_TEXT));
    text_in(d, a, a.x + 24, a.y + lh + 100, "Link file:", 20, C->color(AZ_COL_TEXT_DIM));
    text_in(d, a, a.x + 24, a.y + lh + 132, cp, 16, C->color(AZ_COL_TEXT));
    text_in(d, a, a.x + 24, a.y + lh + 190,
                 "In Backspins: switch Deck link on, plug this stick in, write the link file.", 16,
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
        .help = "When a deck loads a track, Backspins on your computer suggests the five best "
                "tracks to play next. It needs Deck link on in Backspins and the same network.",
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
        .size = sizeof(az_row_v1), .key = "status", .label = "Link status", .kind = AZ_ROW_SCREEN,
        .parent = "on", .help = "Whether the deck reaches Backspins, and where the link file is.",
    };
    static const az_row_v1 again = {
        .size = sizeof(az_row_v1), .key = "reload", .label = "Read link file again",
        .kind = AZ_ROW_ACTION, .parent = "on",
        .help = "After writing the stick again from Backspins (a new address or key).",
        .on_activate = reconnect,
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
            snprintf(cfg_path, sizeof cfg_path, "%s/BACKSPINS/deck-link.cfg", root);
        }
    }
    register_ui();

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
    if (C->thread_start(MOD_ID, "backspins", worker, NULL) != 0)
        return 3;
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
    int h = value(row_hold, 1);
    double hold = HOLD_S[h >= 0 && h < 4 ? h : 1];
    if (expanded && hold > 0 && t - expanded_at > hold)
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
