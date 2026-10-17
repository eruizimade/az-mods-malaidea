/* Host test: the deck's suggestions for one case, printed for tests/parity.py to compare.
 *   parity_main deck-data.tsv NOW_PATH OTHER_PATH|- KEY BPM ENERGY STARS TAGS|- [SESSION_PATH...]
 * TAGS: tag ids joined by commas. Prints "count N", then "path<TAB>fit<TAB>why" per pick. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../deckdata.h"
int main(int argc, char **argv)
{
    if (argc < 9)
        return 2;
    char err[200];
    dd_lib *L = dd_load(argv[1], err, sizeof err);
    if (!L) {
        printf("error %s\n", err);
        return 1;
    }
    int now = dd_find_path(L, argv[2]);
    int other = strcmp(argv[3], "-") ? dd_find_path(L, argv[3]) : -1;
    dd_filters f = { atoi(argv[4]), atof(argv[5]), atoi(argv[6]), atoi(argv[7]), { 0 }, 0 };
    if (strcmp(argv[8], "-"))
        for (char *p = argv[8]; p && f.ntags < 8; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL)
            f.tags[f.ntags++] = atoi(p);
    int session[64], ns = 0;
    for (int i = 9; i < argc && ns < 64; i++)
        session[ns++] = dd_find_path(L, argv[i]);
    dd_pick out[6];
    int pass = 0, n = dd_suggest(L, session, ns, now, other, &f, out, 6, &pass);
    printf("count %d\n", pass);
    for (int i = 0; i < n; i++)
        printf("%s\t%d\t%s\n", dd_get(L, out[i].row)->path, out[i].fit, out[i].why);
    dd_free(L);
    return 0;
}
