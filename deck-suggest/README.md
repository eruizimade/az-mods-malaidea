# Deck Suggestions

Load a track on the XDJ-AZ and a panel shows the six best tracks to play next, worked out on
the deck itself with no computer: the fit (as in Backspins' Sets), your stars, BPM, key,
title, artist, genre, why, the letters to type in the deck's SEARCH, and rekordbox's
waveform. Filters on the panel (key, BPM range, energy, stars, your MyTag groups and
genres) recalculate at once. Listening is the deck's own: search the letters, then Touch
Preview in the browser.

It reads `BACKSPINS/deck-data.tsv`, which Backspins writes to the drive after an export
(`deck_export.py`). `deckdata.c` is a C port of Backspins' scoring (affinity.suggest,
setbuilder.feel_fit and the filters); `tests/parity.py` checks both agree, case for case.

It reads the drive only. No network, no writes, no change to any control, audio or library.

See the [repository README](../README.md) to build, test and publish it.
