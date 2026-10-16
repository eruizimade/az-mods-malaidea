# Deck Suggestions

Load a track on the XDJ-AZ (from a USB stick or from rekordbox over Pro DJ Link) and a panel
shows the six best tracks to play next, picked by Backspins (the rekordbox toolkit) on your
computer: the fit (as in Sets), your stars, BPM, key, title, artist and why. Filters on the
panel (key, BPM range, energy, stars and your MyTag groups) ask again at once. Tracks that play for 45 seconds are reported back,
so what you play on the deck teaches the suggestions.

It needs Backspins with Deck link on (`decklink.py`, port 47866) on the same network, and
`BACKSPINS/deck-link.cfg` on the deck's stick, written by Backspins (`decklink.py
--write-stick`). If the computer's address changed, the mod finds it with a UDP broadcast.

Network: one thread of its own, never the UI thread. It sends what is loaded (title, artist,
BPM, key, rekordbox id) and what played, to Backspins only. It changes no control, audio or
library.

See the [repository README](../README.md) to build, test and publish it.
