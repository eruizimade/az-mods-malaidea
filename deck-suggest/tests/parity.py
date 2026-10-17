#!/usr/bin/env python3
"""Does the deck suggest exactly what Backspins suggests? (deckdata.c against decklink.py)

    python3 tests/parity.py /path/to/backspins-checkout [cases]

Builds a synthetic library (tags, genres, energy, stars, a learned model), exports it
to a fake drive with Backspins' deck_export, then asks both sides the same questions —
random track, session, other deck and filters — and compares every answer: the tracks,
their order, the fit and the reason. Exit code 0 only if all of them agree.
"""
import os, random, subprocess, sys, tempfile

BS = os.path.abspath(sys.argv[1])
CASES = int(sys.argv[2]) if len(sys.argv) > 2 else 300
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BS)
os.environ.setdefault('BACKSPINS_HOME', tempfile.mkdtemp())
import affinity as af, decklink, deck_export  # noqa: E402

rnd = random.Random(7)
MOODS = ['Party', 'Sexy', 'Dark', 'Happy', 'Deep', 'Euphoric']
TIMING = ['Opener', 'Warm-up', 'Peak', 'Closer', 'Filler']
FAMS = ['Vocal', 'Instrumental', 'Classic', 'Fresh']
TYPES = ['Club', 'Wedding', 'Bar', 'Festival']
GENRES = ['House', 'Tech House', 'Indie Dance', 'Techno', 'Nu Disco', 'Afro House', 'Hip Hop',
          'Reggaeton', 'Melodic Techno', 'Disco', 'Trance', 'Pop']
KEYS = ['%d%s' % (n, l) for n in range(1, 13) for l in 'AB']
ARTISTS = ['Artist %d' % i for i in range(70)] + ['Àlex Pérez', 'Mala Idea']

work = tempfile.mkdtemp()
music, drive = os.path.join(work, 'music'), os.path.join(work, 'SSD')
os.makedirs(music); os.makedirs(os.path.join(drive, 'PIONEER'))
tracks = []
for i in range(1, 401):
    banks = {}
    for bank, pool, k in (('MOOD', MOODS, 2), ('TIMING', TIMING, 1), ('FAMILIES', FAMS, 1), ('TYPE OF SET', TYPES, 2)):
        if rnd.random() < 0.85:
            banks[bank] = rnd.sample(pool, rnd.randint(1, k))
    # ⚠ some names with accents: the paths must reach the deck byte for byte
    name = ('track%03d.aiff' if i % 4 else 'pista_%03d_Pérez.aiff') % i
    p = os.path.join(music, name)
    data = os.urandom(rnd.randint(10, 60))
    open(p, 'wb').write(data)
    if i % 13:  # a few are not on the drive
        d = os.path.join(drive, 'Contents', 'A%d' % (i % 9) if i % 5 else 'Àlex Pérez')
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, name), 'wb').write(data)
    tracks.append({
        'id': str(i), 'title': 'Song %d %s' % (i, rnd.choice(['Night', 'Sun', 'Fire', 'Rain', ''])),
        'artist': rnd.choice(ARTISTS), 'path': p,
        'bpm': round(rnd.choice([rnd.uniform(118, 130), rnd.uniform(88, 100), rnd.uniform(134, 146)]), 2),
        'key': rnd.choice(KEYS + ['']), 'energy': rnd.choice([None] + list(range(1, 11))),
        'rating': rnd.randint(0, 5), 'genres': rnd.sample(GENRES, rnd.randint(0, 2)),
        'banks': banks, 'seconds': rnd.randint(150, 480)})
follows = {}
for _ in range(1500):
    a, b = rnd.choice(tracks)['id'], rnd.choice(tracks)['id']
    follows.setdefault(a, {})[b] = follows.get(a, {}).get(b, 0) + rnd.randint(1, 3)
lib = {'tracks': tracks, 'moodBank': 'MOOD', 'timingBank': 'TIMING', 'typeBank': 'TYPE OF SET'}
model = af.learn(tracks, follows)
n_on, _ = deck_export.write(drive, lib, model, wave=lambda t: '-')
data = os.path.join(drive, 'BACKSPINS', 'deck-data.tsv')

# what the drive holds, and the filter groups as the deck reads them
on_drive = {}
tag_ids, groups = {}, {}
for line in open(data, encoding='utf-8'):
    f = line.rstrip('\n').split('\t')
    if f[0] == 'R':
        on_drive[f[1].rsplit('/', 1)[1]] = f[1]
    elif f[0] == 'T':
        tag_ids[int(f[1])] = (f[2], f[3])
    elif f[0] == 'G':
        groups[f[1]] = [int(x) for x in f[2:]]
drive_tracks = [t for t in tracks if os.path.basename(t['path']) in on_drive]
rel = {t['id']: on_drive[os.path.basename(t['path'])] for t in drive_tracks}

# Backspins sees the drive's tracks only (the deck cannot suggest what it does not have)
decklink.start(lambda: dict(lib, tracks=drive_tracks), lambda t=None: model)
binary = os.path.join(work, 'parity_main')
subprocess.check_call(['cc', '-O1', '-o', binary, os.path.join(HERE, 'parity_main.c'),
                       os.path.join(HERE, '..', 'deckdata.c'), '-lm'])
KEYM, BPMM, ENM = ['strict', 'open', 'any'], ['3', '6', '10', 'any'], ['any', 'up', 'same', 'down']
bad = 0
for case in range(CASES):
    now = rnd.choice(drive_tracks)
    session = rnd.sample(drive_tracks, rnd.randint(0, 6))
    other = rnd.choice([None, rnd.choice(drive_tracks)])
    k, b, e, st = rnd.randrange(3), rnd.randrange(4), rnd.choice([0, 0, 1, 2, 3]), rnd.choice([0, 0, 3, 4])
    chosen = []
    for label, ids in groups.items():
        if ids and rnd.random() < 0.25:
            chosen.append((label, rnd.choice(ids)))
    decklink._current_session = lambda s=session: [{'id': t['id']} for t in s]
    form = {'id': now['id'], 'title': now['title'], 'key': KEYM[k], 'bpm': BPMM[b], 'energy': ENM[e],
            'stars': str(st), 'tags': '|'.join('%s:%s' % (lab, tag_ids[i][1]) for lab, i in chosen)}
    if other:
        form['other_id'], form['other_title'] = other['id'], other['title']
    py = decklink.suggest_lines(form)
    by_title = {}
    want = [l for l in py if l.startswith('count')] + []
    by_t = {decklink._ascii(t['title'], 60): rel[t['id']] for t in drive_tracks}
    for l in py:
        if l.startswith('s\t'):
            f = l.split('\t')
            want.append('%s\t%s\t%s' % (by_t[f[5]], f[1], f[8]))
    args = [binary, data, rel[now['id']], rel[other['id']] if other else '-', str(k),
            '-1' if b == 3 else BPMM[b], str(e), str(st), ','.join(str(i) for _l, i in chosen) or '-']
    args += [rel[t['id']] for t in session]
    got = subprocess.run(args, capture_output=True, text=True).stdout.splitlines()
    got = [g.replace('count ', 'count\t') for g in got]
    if got != want:
        bad += 1
        if bad <= 5:
            print('case %d differs\n  python: %s\n  deck:   %s' % (case, want, got))
print('%d tracks on the drive, %d cases, %d differ' % (n_on, CASES, bad))
sys.exit(1 if bad else 0)
