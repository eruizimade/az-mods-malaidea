/* SPDX-License-Identifier: GPL-3.0-only
 *
 * deckdata: see deckdata.h. The scoring below follows Backspins' Python in the same order,
 * step for step, so ties break the same way:
 *   affinity.suggest      what you play after what, the kind of night, freshness, key, tempo
 *   setbuilder.feel_fit   the fit shown on each row (mood, genre family, energy, tempo)
 *   decklink._passes      the panel's filters
 */
#include "deckdata.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* affinity.py */
#define RECENT 3
#define BPM_REACH 14.0
#define PER_ARTIST 2
#define PER_REASON 3
/* setbuilder.FEEL_WEIGHTS */
#define W_MOODS 0.40
#define W_FAMILY 0.25
#define W_ENERGY 0.20
#define W_BPM 0.15

#define MAX_GROUPS 6
#define MAX_GTAGS 10
#define MAX_FLOW_BANKS 8

typedef struct { char *bank, *name; } tag_t;

struct dd_lib {
    dd_row *rows;
    int n, cap;
    tag_t *tags;
    int ntags, tagcap;
    char *mood_bank, *type_bank;
    char *flow_banks[MAX_FLOW_BANKS];
    int n_flow_banks;
    /* per tag: is it a move tag (a FLOW bank), a type-of-night tag, a mood tag */
    unsigned char *is_flow, *is_type, *is_mood;
    /* the model, dense: flow[a * ntags + b], party[type * ntags + tag]; NAN = no such pair */
    float *flow, *party;
    int key_level[24][24]; /* 0 = no move */
    char g_label[MAX_GROUPS][16];
    int g_tag[MAX_GROUPS][MAX_GTAGS], g_n[MAX_GROUPS], ngroups;
    int *by_path;          /* row indices sorted by path */
};

/* ---- reading ----------------------------------------------------------------------------- */

static char *dup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

/* Split line in place at tabs; returns the number of fields (at most max). */
static int split(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;
    while (p && n < max) {
        f[n++] = p;
        p = strchr(p, '\t');
        if (p)
            *p++ = 0;
    }
    return n;
}

static int *ids(const char *s, int *n)
{
    *n = 0;
    if (!s || !*s || !strcmp(s, "-"))
        return NULL;
    int cap = 1;
    for (const char *p = s; *p; p++)
        cap += *p == ',';
    int *out = malloc(sizeof(int) * (size_t)cap);
    if (!out)
        return NULL;
    for (const char *p = s; *p;) {
        out[(*n)++] = atoi(p);
        p = strchr(p, ',');
        if (!p)
            break;
        p++;
    }
    return out;
}

/* "8A" -> 0..23, -1 if it is not a key */
static int key_index(const char *k)
{
    if (!k || !isdigit((unsigned char)k[0]))
        return -1;
    int n = atoi(k);
    const char *l = k + (isdigit((unsigned char)k[1]) ? 2 : 1);
    if (n < 1 || n > 12 || (*l != 'A' && *l != 'B') || l[1])
        return -1;
    return (n - 1) * 2 + (*l == 'B');
}

static int hexv(char c)
{
    return c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0';
}

static const dd_lib *sort_lib;
static int by_path_cmp(const void *a, const void *b)
{
    return strcmp(sort_lib->rows[*(const int *)a].path, sort_lib->rows[*(const int *)b].path);
}

static int add_tag(dd_lib *L, int id, const char *bank, const char *name)
{
    if (id < 0 || id > 100000)
        return -1;
    if (id >= L->tagcap) {
        int cap = id * 2 + 16;
        tag_t *t = realloc(L->tags, sizeof(tag_t) * (size_t)cap);
        if (!t)
            return -1;
        memset(t + L->tagcap, 0, sizeof(tag_t) * (size_t)(cap - L->tagcap));
        L->tags = t;
        L->tagcap = cap;
    }
    L->tags[id].bank = dup(bank);
    L->tags[id].name = dup(name);
    if (id + 1 > L->ntags)
        L->ntags = id + 1;
    return 0;
}

dd_lib *dd_load(const char *path, char *err, size_t errn)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        snprintf(err, errn, "no deck data on the drive (BACKSPINS/deck-data.tsv)");
        return NULL;
    }
    dd_lib *L = calloc(1, sizeof *L);
    size_t cap = 4096;
    char *line = malloc(cap);
    if (!L || !line) {
        free(L);
        free(line);
        fclose(fp);
        snprintf(err, errn, "out of memory");
        return NULL;
    }
    /* two passes over F and P lines: the model needs the tag count first */
    long model_at = -1;
    int version = 0;
    for (;;) {
        size_t len = 0;
        int c;
        long at = ftell(fp);
        while ((c = fgetc(fp)) != EOF && c != '\n') {
            if (len + 1 >= cap) {
                char *b = realloc(line, cap * 2);
                if (!b)
                    break;
                line = b;
                cap *= 2;
            }
            line[len++] = (char)c;
        }
        if (c == EOF && len == 0)
            break;
        line[len] = 0;
        char *f[16];
        int nf = split(line, f, 16);
        if (nf < 1 || f[0][0] == '#')
            continue;
        switch (f[0][0]) {
        case 'V':
            version = nf > 1 ? atoi(f[1]) : 0;
            break;
        case 'M':
            if (nf >= 3) {
                L->mood_bank = dup(f[1]);
                L->type_bank = dup(f[2]);
            }
            break;
        case 'B':
            if (nf >= 2 && L->n_flow_banks < MAX_FLOW_BANKS)
                L->flow_banks[L->n_flow_banks++] = dup(f[1]);
            break;
        case 'T':
            if (nf >= 4)
                add_tag(L, atoi(f[1]), f[2], f[3]);
            break;
        case 'G':
            if (nf >= 3 && L->ngroups < MAX_GROUPS) {
                int g = L->ngroups++;
                snprintf(L->g_label[g], sizeof L->g_label[g], "%s", f[1]);
                for (int i = 2; i < nf && L->g_n[g] < MAX_GTAGS; i++)
                    L->g_tag[g][L->g_n[g]++] = atoi(f[i]);
            }
            break;
        case 'F':
        case 'P':
            if (model_at < 0)
                model_at = at;
            break;
        case 'K':
            if (nf >= 4) {
                int a = key_index(f[1]), b = key_index(f[2]);
                if (a >= 0 && b >= 0)
                    L->key_level[a][b] = atoi(f[3]);
            }
            break;
        case 'R':
            if (nf >= 14) {
                if (L->n == L->cap) {
                    int nc = L->cap ? L->cap * 2 : 1024;
                    dd_row *r = realloc(L->rows, sizeof(dd_row) * (size_t)nc);
                    if (!r)
                        break;
                    L->rows = r;
                    L->cap = nc;
                }
                dd_row *r = &L->rows[L->n++];
                memset(r, 0, sizeof *r);
                r->path = dup(f[1]);
                r->title = dup(f[2]);
                r->artist = dup(f[3]);
                r->bpm = strcmp(f[4], "-") ? atof(f[4]) : 0;
                r->key = dup(f[5]);
                r->energy = strcmp(f[6], "-") ? atof(f[6]) : -1;
                r->stars = atoi(f[7]);
                r->secs = atoi(f[8]);
                r->tags = ids(f[9], &r->ntags);
                r->fams = ids(f[10], &r->nfams);
                r->genre = dup(f[11]);
                r->search = dup(f[12]);
                if (strlen(f[13]) >= DD_WAVE_COLS * 3) {
                    for (int k = 0; k < DD_WAVE_COLS * 3; k++)
                        r->wave[k / 3][k % 3] = (unsigned char)(hexv(f[13][k]) & 15);
                    r->has_wave = 1;
                }
            }
            break;
        }
        if (c == EOF)
            break;
    }
    if (version != 1 || !L->n) {
        snprintf(err, errn, version != 1 ? "deck data of another version: export again from Backspins"
                                         : "the deck data lists no tracks on this drive");
        free(line);
        fclose(fp);
        dd_free(L);
        return NULL;
    }
    int T = L->ntags ? L->ntags : 1;
    L->is_flow = calloc((size_t)T, 1);
    L->is_type = calloc((size_t)T, 1);
    L->is_mood = calloc((size_t)T, 1);
    L->flow = malloc(sizeof(float) * (size_t)T * (size_t)T);
    L->party = malloc(sizeof(float) * (size_t)T * (size_t)T);
    L->by_path = malloc(sizeof(int) * (size_t)L->n);
    if (!L->is_flow || !L->is_type || !L->is_mood || !L->flow || !L->party || !L->by_path) {
        snprintf(err, errn, "out of memory");
        free(line);
        fclose(fp);
        dd_free(L);
        return NULL;
    }
    for (int i = 0; i < L->ntags; i++) {
        if (!L->tags[i].bank)
            continue;
        for (int b = 0; b < L->n_flow_banks; b++)
            if (!strcmp(L->tags[i].bank, L->flow_banks[b]))
                L->is_flow[i] = 1;
        L->is_type[i] = L->type_bank && !strcmp(L->tags[i].bank, L->type_bank);
        L->is_mood[i] = L->mood_bank && !strcmp(L->tags[i].bank, L->mood_bank);
    }
    for (long i = 0; i < (long)T * T; i++)
        L->flow[i] = L->party[i] = NAN;
    if (model_at >= 0) {
        fseek(fp, model_at, SEEK_SET);
        while (fgets(line, (int)cap, fp)) {
            if (line[0] != 'F' && line[0] != 'P')
                continue;
            line[strcspn(line, "\r\n")] = 0;
            char *f[4];
            if (split(line, f, 4) < 4)
                continue;
            int a = atoi(f[1]), b = atoi(f[2]);
            if (a < 0 || b < 0 || a >= L->ntags || b >= L->ntags)
                continue;
            (line[0] == 'F' ? L->flow : L->party)[a * L->ntags + b] = (float)atof(f[3]);
        }
    }
    free(line);
    fclose(fp);
    for (int i = 0; i < L->n; i++)
        L->by_path[i] = i;
    sort_lib = L;
    qsort(L->by_path, (size_t)L->n, sizeof(int), by_path_cmp);
    return L;
}

void dd_free(dd_lib *L)
{
    if (!L)
        return;
    for (int i = 0; i < L->n; i++) {
        dd_row *r = &L->rows[i];
        free(r->path);
        free(r->title);
        free(r->artist);
        free(r->key);
        free(r->genre);
        free(r->search);
        free(r->tags);
        free(r->fams);
    }
    for (int i = 0; i < L->ntags; i++) {
        free(L->tags[i].bank);
        free(L->tags[i].name);
    }
    for (int b = 0; b < L->n_flow_banks; b++)
        free(L->flow_banks[b]);
    free(L->rows);
    free(L->tags);
    free(L->mood_bank);
    free(L->type_bank);
    free(L->is_flow);
    free(L->is_type);
    free(L->is_mood);
    free(L->flow);
    free(L->party);
    free(L->by_path);
    free(L);
}

int dd_count(const dd_lib *L) { return L ? L->n : 0; }
const dd_row *dd_get(const dd_lib *L, int i) { return L && i >= 0 && i < L->n ? &L->rows[i] : NULL; }
int dd_groups(const dd_lib *L) { return L ? L->ngroups : 0; }
const char *dd_group_label(const dd_lib *L, int g) { return L->g_label[g]; }
int dd_group_ntags(const dd_lib *L, int g) { return L->g_n[g]; }
int dd_group_tag(const dd_lib *L, int g, int i) { return L->g_tag[g][i]; }
const char *dd_tag_name(const dd_lib *L, int t)
{
    return L && t >= 0 && t < L->ntags && L->tags[t].name ? L->tags[t].name : "";
}

int dd_find_path(const dd_lib *L, const char *rel)
{
    int lo = 0, hi = L ? L->n - 1 : -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(L->rows[L->by_path[mid]].path, rel);
        if (!c)
            return L->by_path[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

/* letters and digits only, lower case: "Sol de Nit!" -> "soldenit" (the data is ASCII already) */
static void squash(const char *s, char *out, size_t n)
{
    size_t k = 0;
    for (; s && *s && k + 1 < n; s++)
        if (isalnum((unsigned char)*s))
            out[k++] = (char)tolower((unsigned char)*s);
    out[k] = 0;
}

int dd_find_title(const dd_lib *L, const char *title, const char *artist)
{
    if (!L || !title || !*title)
        return -1;
    char t[200], a[200], rt[200], ra[200];
    char tb[200], ab[200];
    /* the deck's title can be the file name, "Title - Artist" */
    snprintf(tb, sizeof tb, "%s", title);
    snprintf(ab, sizeof ab, "%s", artist ? artist : "");
    char *dash = strstr(tb, " - ");
    if (dash && !ab[0]) {
        snprintf(ab, sizeof ab, "%s", dash + 3);
        *dash = 0;
    }
    squash(tb, t, sizeof t);
    squash(ab, a, sizeof a);
    int best = -1;
    for (int i = 0; i < L->n; i++) {
        squash(L->rows[i].title, rt, sizeof rt);
        if (strcmp(rt, t))
            continue;
        squash(L->rows[i].artist, ra, sizeof ra);
        if (!a[0] || !strcmp(ra, a))
            return i;
        if (best < 0)
            best = i;
    }
    return best;
}

/* ---- the rules --------------------------------------------------------------------------- */

/* setbuilder.bpm_distance: relative, allowing half and double time */
static double bpm_distance(double a, double b)
{
    if (!a || !b)
        return -1;
    double best = fabs(a - b) / a;
    double f2 = fabs(a - b * 2.0) / a, f05 = fabs(a - b * 0.5) / a;
    if (f2 < best)
        best = f2;
    if (f05 < best)
        best = f05;
    return best;
}

static int key_level(const dd_lib *L, const char *a, const char *b)
{
    int i = key_index(a), j = key_index(b);
    return i < 0 || j < 0 ? 0 : L->key_level[i][j];
}

static int has_key(const char *k) { return k && *k && strcmp(k, "-"); }

static int row_has_tag(const dd_row *r, int tag)
{
    for (int i = 0; i < r->ntags; i++)
        if (r->tags[i] == tag)
            return 1;
    return 0;
}

/* decklink._passes */
static int passes(const dd_lib *L, const dd_row *t, const dd_row *now, const dd_filters *f)
{
    if (f->key_mode < 2 && has_key(now->key) && has_key(t->key)) {
        int lvl = key_level(L, now->key, t->key);
        int ok = lvl && lvl <= (f->key_mode == 0 ? 1 : 2);
        if (!ok && strcmp(t->key, now->key))
            return 0;
    }
    if (f->bpm_pct >= 0 && now->bpm && t->bpm) {
        double d = bpm_distance(now->bpm, t->bpm);
        if (d >= 0 && d > f->bpm_pct / 100.0)
            return 0;
    }
    if (f->energy) {
        double e0 = now->energy, e1 = t->energy;
        if (e0 < 0 || e1 < 0)
            return 0;
        if ((f->energy == 1 && e1 <= e0) || (f->energy == 3 && e1 >= e0) ||
            (f->energy == 2 && fabs(e1 - e0) > 0.5))
            return 0;
    }
    if (f->stars > t->stars)
        return 0;
    for (int i = 0; i < f->ntags; i++)
        if (!row_has_tag(t, f->tags[i]))
            return 0;
    return 1;
}

/* ---- the fit (setbuilder.set_feel / feel_fit) --------------------------------------------- */

typedef struct {
    int n;
    double *moods;   /* share per tag id, 0 = absent */
    double *fams;
    double moods_total, fams_total;
    int has_moods, has_fams;
    double energy, bpm;
    int has_energy, has_bpm;
} feel_t;

static void set_feel(const dd_lib *L, const int *set, int nset, feel_t *F)
{
    memset(F->moods, 0, sizeof(double) * (size_t)L->ntags);
    memset(F->fams, 0, sizeof(double) * (size_t)L->ntags);
    F->n = nset;
    F->moods_total = F->fams_total = 0;
    F->has_moods = F->has_fams = F->has_energy = F->has_bpm = 0;
    if (!nset)
        return;
    double es = 0, bs = 0;
    int ne = 0, nb = 0;
    for (int k = 0; k < nset; k++) {
        const dd_row *t = &L->rows[set[k]];
        for (int i = 0; i < t->ntags; i++)
            if (L->is_mood[t->tags[i]])
                F->moods[t->tags[i]] += 1;
        for (int i = 0; i < t->nfams; i++)
            F->fams[t->fams[i]] += 1;
        if (t->energy >= 0) {
            es += t->energy;
            ne++;
        }
        if (t->bpm) {
            bs += t->bpm;
            nb++;
        }
    }
    for (int i = 0; i < L->ntags; i++) {
        if (F->moods[i]) {
            F->moods[i] /= nset;
            F->moods_total += F->moods[i];
            F->has_moods = 1;
        }
        if (F->fams[i]) {
            F->fams[i] /= nset;
            F->fams_total += F->fams[i];
            F->has_fams = 1;
        }
    }
    if (ne) {
        F->energy = es / ne;
        F->has_energy = 1;
    }
    if (nb) {
        F->bpm = bs / nb;
        F->has_bpm = 1;
    }
}

static double round_to(double x, double scale) { return rint(x * scale) / scale; }

static double feel_fit(const dd_lib *L, const dd_row *t, const feel_t *F)
{
    if (!F->n)
        return 1.0;
    double sum = 0, wsum = 0;
    int any_mood = 0;
    for (int i = 0; i < t->ntags; i++)
        any_mood |= L->is_mood[t->tags[i]];
    if (F->has_moods && any_mood) {
        double got = 0;
        /* each mood of the set counted once, whatever the track lists twice */
        for (int m = 0; m < L->ntags; m++)
            if (F->moods[m] && row_has_tag(t, m))
                got += F->moods[m];
        double v = got / (F->moods_total ? F->moods_total : 1.0);
        sum += (v < 1 ? v : 1) * W_MOODS;
        wsum += W_MOODS;
    }
    if (F->has_fams && t->nfams) {
        double got = 0;
        for (int m = 0; m < L->ntags; m++)
            if (F->fams[m])
                for (int i = 0; i < t->nfams; i++)
                    if (t->fams[i] == m) {
                        got += F->fams[m];
                        break;
                    }
        double v = got / (F->fams_total ? F->fams_total : 1.0);
        sum += (v < 1 ? v : 1) * W_FAMILY;
        wsum += W_FAMILY;
    }
    if (t->energy >= 0 && F->has_energy) {
        double v = 1.0 - fabs(t->energy - F->energy) / 3.0;
        sum += (v > 0 ? v : 0) * W_ENERGY;
        wsum += W_ENERGY;
    }
    if (t->bpm && F->has_bpm) {
        double drift = fabs(t->bpm - F->bpm) / F->bpm;
        double v = 1.0 - drift / 0.12;
        sum += (v > 0 ? v : 0) * W_BPM;
        wsum += W_BPM;
    }
    if (wsum == 0)
        return 0.5;
    return round_to(sum / wsum, 1000.0);
}

/* ---- the suggestions (affinity.suggest) --------------------------------------------------- */

typedef struct {
    int row;
    double score;      /* rounded to 4 decimals, as Python sorts it */
    int order;         /* the pool order, for a stable sort */
    int reason_a, reason_b;
    char why[72];
} cand_t;

static int cand_cmp(const void *a, const void *b)
{
    const cand_t *x = a, *y = b;
    if (x->score != y->score)
        return x->score < y->score ? 1 : -1;
    return x->order - y->order;
}

/* decklink._ascii(s, 64): cut with "~" */
static void put_why(char *dst, size_t cap, const char *s)
{
    size_t n = strlen(s);
    if (n <= 64) {
        snprintf(dst, cap, "%s", s);
        return;
    }
    char b[72];
    memcpy(b, s, 63);
    b[63] = 0;
    size_t k = 63;
    while (k > 0 && b[k - 1] == ' ')
        b[--k] = 0;
    snprintf(dst, cap, "%s~", b);
}

static int lower_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

static int affinity(const dd_lib *L, const int *set, int nset, const int *pool, int npool,
                    dd_pick *out, int limit)
{
    int T = L->ntags;
    int has_flow = 0;
    for (long i = 0; i < (long)T * T && !has_flow; i++)
        has_flow = !isnan(L->flow[i]);
    if (!has_flow)
        return 0;

    /* where we are now: the last RECENT records, the last one counting most */
    int rs = nset > RECENT ? nset - RECENT : 0, nrec = nset - rs;
    int rtag[RECENT * 64];
    double rw[RECENT * 64];
    int nr = 0;
    for (int i = 0; i < nrec; i++) {
        const dd_row *t = &L->rows[set[rs + i]];
        double w = (double)(i + 1) / nrec;
        for (int k = 0; k < t->ntags && nr < RECENT * 64; k++)
            if (L->is_flow[t->tags[k]]) {
                rtag[nr] = t->tags[k];
                rw[nr++] = w;
            }
    }
    /* the kind of night: TYPE tags counted over the set, in order of first appearance */
    int ty_tag[64], ty_n[64], nty = 0, ty_top = 0;
    unsigned char *seen = calloc((size_t)(T ? T : 1), 1);
    cand_t *cs = malloc(sizeof(cand_t) * (size_t)(npool ? npool : 1));
    if (!seen || !cs) {
        free(seen);
        free(cs);
        return 0;
    }
    for (int s = 0; s < nset; s++) {
        const dd_row *t = &L->rows[set[s]];
        for (int k = 0; k < t->ntags; k++) {
            int g = t->tags[k];
            if (L->is_flow[g])
                seen[g] = 1;
            if (!L->is_type[g])
                continue;
            int j = 0;
            while (j < nty && ty_tag[j] != g)
                j++;
            if (j == nty && nty < 64) {
                ty_tag[nty] = g;
                ty_n[nty++] = 0;
            }
            if (j < nty)
                ty_n[j]++;
        }
    }
    for (int j = 0; j < nty; j++)
        if (ty_n[j] > ty_top)
            ty_top = ty_n[j];
    int night = -1;
    for (int j = 0; j < nty; j++)
        if (night < 0 || ty_n[j] > ty_n[night])
            night = j;

    const dd_row *last = nset ? &L->rows[set[nset - 1]] : NULL;
    double last_bpm = last ? last->bpm : 0;
    const char *last_key = last ? last->key : NULL;

    int nc = 0;
    for (int p = 0; p < npool; p++) {
        const dd_row *c = &L->rows[pool[p]];
        int ct[64], nct = 0;
        for (int k = 0; k < c->ntags && nct < 64; k++)
            if (L->is_flow[c->tags[k]])
                ct[nct++] = c->tags[k];
        if (!nct)
            continue;

        /* 1. does this move look like a move you make? */
        int have_best = 0, ba = -1, bb = -1;
        double bv = 0, flow_score = 0;
        for (int r = 0; r < nr; r++)
            for (int k = 0; k < nct; k++) {
                float v = L->flow[rtag[r] * T + ct[k]];
                if (isnan(v))
                    continue;
                double vv = v * rw[r];
                flow_score += vv;
                if (!have_best || vv > bv) {
                    have_best = 1;
                    bv = vv;
                    ba = rtag[r];
                    bb = ct[k];
                }
            }
        flow_score /= nct;

        /* 2. does it belong to this kind of night? */
        double party_score = 0;
        for (int j = 0; j < nty; j++) {
            double w = (double)ty_n[j] / ty_top;
            for (int k = 0; k < nct; k++) {
                float v = L->party[ty_tag[j] * T + ct[k]];
                party_score += w * (isnan(v) ? 0.0 : v);
            }
        }
        party_score /= nct;

        /* 3. what the set has not had yet */
        int fresh_n = 0;
        for (int k = 0; k < nct; k++)
            fresh_n += !seen[ct[k]];
        double fresh = (double)fresh_n / nct;

        /* 4. can it actually follow? */
        double tempo = 1.0;
        if (last_bpm && c->bpm) {
            double gap = fabs(c->bpm - last_bpm);
            double g2 = fabs(c->bpm * 2 - last_bpm), g05 = fabs(c->bpm / 2 - last_bpm);
            if (g2 < gap)
                gap = g2;
            if (g05 < gap)
                gap = g05;
            if (gap > BPM_REACH)
                continue;
            tempo = 1.0 - (gap / BPM_REACH) * 0.5;
        }
        double keyed = 0;
        if (has_key(last_key) && has_key(c->key)) {
            int lvl = key_level(L, last_key, c->key);
            if (lvl)
                keyed = 0.45 / (lvl > 1 ? lvl : 1);
        }

        double score = (flow_score + 0.6 * party_score + 0.5 * fresh + keyed) * tempo;
        cand_t *o = &cs[nc++];
        o->row = pool[p];
        o->score = round_to(score, 10000.0);
        o->order = p;
        o->reason_a = have_best ? ba : -1;
        o->reason_b = have_best ? bb : -1;
        char why[200] = "";
        if (have_best && bv > 0.05)
            snprintf(why, sizeof why, "despres de \"%s\" sols anar a \"%s\"", dd_tag_name(L, ba),
                     dd_tag_name(L, bb));
        else if (party_score > 0.05 && nty)
            snprintf(why, sizeof why, "encaixa amb un set de \"%s\"", dd_tag_name(L, ty_tag[night]));
        else if (fresh > 0.5) {
            char list[120] = "";
            int shown = 0;
            for (int k = 0; k < nct && shown < 2; k++)
                if (!seen[ct[k]]) {
                    size_t l = strlen(list);
                    snprintf(list + l, sizeof list - l, "%s%s", shown ? ", " : "", dd_tag_name(L, ct[k]));
                    shown++;
                }
            snprintf(why, sizeof why, "aporta %s, que encara no hi es", list);
        } else if (keyed > 0.2)
            snprintf(why, sizeof why, "la tonalitat lliga amb \"%s\"", last_key);
        put_why(o->why, sizeof o->why, why);
    }
    qsort(cs, (size_t)nc, sizeof(cand_t), cand_cmp);

    /* spread the answer out: at most PER_ARTIST per artist, PER_REASON per kind of move */
    int picked = 0;
    int pick_ra[64], pick_rb[64];
    if (limit > 64)
        limit = 64;
    for (int i = 0; i < nc && picked < limit; i++) {
        const dd_row *c = &L->rows[cs[i].row];
        int same_artist = 0, same_reason = 0;
        for (int j = 0; j < picked; j++) {
            const dd_row *q = &L->rows[out[j].row];
            same_artist += lower_eq(q->artist, c->artist);
        }
        if (same_artist >= PER_ARTIST)
            continue;
        if (cs[i].reason_a >= 0) {
            for (int j = 0; j < picked; j++)
                same_reason += pick_ra[j] == cs[i].reason_a && pick_rb[j] == cs[i].reason_b;
            if (same_reason >= PER_REASON)
                continue;
        }
        pick_ra[picked] = cs[i].reason_a;
        pick_rb[picked] = cs[i].reason_b;
        out[picked].row = cs[i].row;
        snprintf(out[picked].why, sizeof out[picked].why, "%s", cs[i].why);
        picked++;
    }
    free(seen);
    free(cs);
    return picked;
}

/* ---- decklink.suggest_lines --------------------------------------------------------------- */

static int fit_cmp_row(const void *a, const void *b)
{
    const cand_t *x = a, *y = b;
    if (x->score != y->score)
        return x->score < y->score ? 1 : -1;
    return x->order - y->order;
}

int dd_suggest(const dd_lib *L, const int *session, int nsession, int now, int other,
               const dd_filters *f, dd_pick *out, int max, int *n_pass)
{
    *n_pass = 0;
    if (!L || now < 0 || now >= L->n || max <= 0)
        return 0;
    const dd_row *nw = &L->rows[now];
    int *set = malloc(sizeof(int) * (size_t)(nsession + 1));
    int *pool = malloc(sizeof(int) * (size_t)L->n);
    unsigned char *skip = calloc((size_t)L->n, 1);
    feel_t F = { 0 };
    F.moods = calloc((size_t)(L->ntags ? L->ntags : 1), sizeof(double));
    F.fams = calloc((size_t)(L->ntags ? L->ntags : 1), sizeof(double));
    if (!set || !pool || !skip || !F.moods || !F.fams) {
        free(set);
        free(pool);
        free(skip);
        free(F.moods);
        free(F.fams);
        return 0;
    }
    int nset = 0;
    for (int i = 0; i < nsession; i++)
        if (session[i] >= 0 && session[i] < L->n) {
            skip[session[i]] = 1;
            if (session[i] != now)
                set[nset++] = session[i];
        }
    set[nset++] = now;
    skip[now] = 1;
    if (other >= 0 && other < L->n)
        skip[other] = 1;
    int np = 0;
    for (int i = 0; i < L->n; i++)
        if (!skip[i] && passes(L, &L->rows[i], nw, f))
            pool[np++] = i;
    *n_pass = np;
    set_feel(L, set, nset, &F);

    int n = affinity(L, set, nset, pool, np, out, max);
    for (int i = 0; i < n; i++)
        out[i].fit = (int)rint(feel_fit(L, &L->rows[out[i].row], &F) * 100.0);
    if (n < max) {
        /* nothing learned, or too little passes: the best fit among what passes */
        cand_t *rest = malloc(sizeof(cand_t) * (size_t)(np ? np : 1));
        int nrest = 0;
        if (rest) {
            for (int p = 0; p < np; p++) {
                int dupl = 0;
                for (int i = 0; i < n; i++)
                    dupl |= out[i].row == pool[p];
                if (dupl)
                    continue;
                rest[nrest].row = pool[p];
                rest[nrest].score = feel_fit(L, &L->rows[pool[p]], &F);
                rest[nrest].order = p;
                nrest++;
            }
            qsort(rest, (size_t)nrest, sizeof(cand_t), fit_cmp_row);
            for (int i = 0; i < nrest && n < max; i++) {
                out[n].row = rest[i].row;
                out[n].fit = (int)rint(rest[i].score * 100.0);
                snprintf(out[n].why, sizeof out[n].why, "fits the set so far");
                n++;
            }
            free(rest);
        }
    }
    free(set);
    free(pool);
    free(skip);
    free(F.moods);
    free(F.fams);
    return n;
}
