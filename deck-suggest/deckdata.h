/* SPDX-License-Identifier: GPL-3.0-only
 *
 * deckdata: Backspins' deck data (BACKSPINS/deck-data.tsv, written by deck_export.py) and the
 * suggestions worked out from it, on the deck, with no computer.
 *
 * The scoring is a port of Backspins' own (affinity.suggest, setbuilder.feel_fit and the
 * deck filters of decklink.suggest_lines), kept line for line in the same order so both give
 * the same answer for the same library; tests/parity.sh checks that they do.
 *
 * Plain C, no AZ-MODS services: it builds and runs on a computer too.
 */
#ifndef DECKDATA_H
#define DECKDATA_H

#include <stddef.h>

#define DD_WAVE_COLS 120

typedef struct {
    char *path, *title, *artist, *key, *genre, *search;
    double bpm;          /* 0 = unknown */
    double energy;       /* -1 = unknown */
    int stars, secs;
    int *tags, ntags;    /* tag ids, in the library's order (MyTags, then genres) */
    int *fams, nfams;    /* genre family ids */
    int has_wave;
    unsigned char wave[DD_WAVE_COLS][3]; /* low, mid, high: 0..15 */
} dd_row;

typedef struct dd_lib dd_lib;

/* Read deck-data.tsv; NULL with the reason in err. */
dd_lib *dd_load(const char *path, char *err, size_t errn);
void dd_free(dd_lib *L);

int dd_count(const dd_lib *L);
const dd_row *dd_get(const dd_lib *L, int i);
/* The row whose path is rel ("/Contents/..."), or -1. */
int dd_find_path(const dd_lib *L, const char *rel);
/* The row with this title (and artist, if given), compared without case or accents; -1. */
int dd_find_title(const dd_lib *L, const char *title, const char *artist);

/* The filter groups the panel offers (TIMING, MOOD, TYPE, FAMILY, GENRE). */
int dd_groups(const dd_lib *L);
const char *dd_group_label(const dd_lib *L, int g);
int dd_group_ntags(const dd_lib *L, int g);
int dd_group_tag(const dd_lib *L, int g, int i);
const char *dd_tag_name(const dd_lib *L, int tag);

typedef struct {
    int key_mode;        /* 0 strict, 1 open, 2 any */
    double bpm_pct;      /* allowed tempo difference in %, < 0 = any */
    int energy;          /* 0 any, 1 up, 2 same, 3 down */
    int stars;           /* minimum */
    int tags[8], ntags;  /* tag ids every suggestion must carry */
} dd_filters;

typedef struct {
    int row;
    int fit;             /* 0..100, as Sets shows it */
    char why[72];
} dd_pick;

/* The best follow-ups of row `now`, given what this session has played (rows, oldest first)
 * and the track on the other deck (-1 = none). Returns how many picks were written; *n_pass
 * gets how many tracks pass the filters. */
int dd_suggest(const dd_lib *L, const int *session, int nsession, int now, int other,
               const dd_filters *f, dd_pick *out, int max, int *n_pass);

#endif
