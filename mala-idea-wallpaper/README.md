# Mala Idea Wallpaper

An animated screen for the XDJ-AZ (AZ-MODS core 1.2.3+): the Mala Idea logo in AZ-MODS
orange ASCII on black ("Corrent"). The bubble's outline is drawn in line characters, its
inside is a slow wave of characters, the letters are dense and swept by a band of light, and
loose grey characters drift outside.

It comes up when the deck powers on (Mod Menu > Mala Idea Wallpaper > At power-on) and when
nothing is touched and no deck plays for the chosen time (Show after). Press SHIFT twice, on
either side, to get the deck back. Touches while it is up never reach the deck; a finger held
on the screen for 3 seconds also closes it.

The logo's cells and the characters' shapes are `assets/corrent.mic`, written by
`../tools/make_wallpaper.py`; the mod draws each frame itself, 12 a second.
`../preview/fons-animat.html` shows the same animation in a browser.

See the [repository README](../README.md) to build, test and publish it.
