<div align="center">

<img src="mala-idea-wallpaper/icon.png" width="140" alt="Mala Idea">

**Mods de la comunitat AZ-MODS per a l'XDJ-AZ de Mala Idea**

</div>

Mods per a l'[AZ-MODS](https://github.com/Kyle-Hosman/xdj-az-mods) de l'XDJ-AZ, fets amb
l'[AZ-MODS SDK](https://github.com/Kyle-Hosman/az-mods-sdk). Cal tenir el deck amb el
firmware d'AZ-MODS (2.01, core 1.2.3 o més nou).

| Mod | Què fa |
|:--|:--|
| [**Mala Idea Wallpaper**](mala-idea-wallpaper) | El logo de Mala Idea animat en ASCII taronja sobre negre, en engegar i quan el deck queda quiet |

<p align="center"><img src="images/wallpaper.png" width="720" alt="El logo de Mala Idea en ASCII"></p>

## Mala Idea Wallpaper

El logo de Mala Idea tapa tota la pantalla en el taronja d'AZ-MODS, animat («Corrent»): el
contorn de la bombolla en caràcters de línia, l'interior una ona lenta de caràcters, les
lletres denses i travessades per una franja de llum, i caràcters grisos solts que deriven a
fora. `preview/fons-animat.html` mostra l'animació al navegador.

Surt **en engegar la taula**, quan apareix la pantalla del deck, i quan el deck queda quiet
(cap toc, cap gir del browse, cap deck sonant) el temps que triïs. **SHIFT dues vegades**
(qualsevol costat) el treu. Els tocs mentre hi és no arriben al deck; mantenir el dit 3 s a
la pantalla també el treu.

Al **Mod Menu** surt la fila *Mala Idea Wallpaper*: activar-lo o no, **At power-on**,
**Show after** (1, 2, 5, 10 o 30 minuts; per defecte 5) i **Show now**.

> [!NOTE]
> Per què no és un fons *darrere* les ones? AZ-MODS deixa que un mod de la comunitat pinti
> només **per sobre** de la pantalla del deck, mai per sota. El logo d'AlphaTheta d'abans
> d'arrencar tampoc: surt abans que es carreguin els mods.

## Instal·lar-lo

Al deck: **Mod Menu > Community Mods > Repositories**, afegeix `eruizimade/az-mods-malaidea`
i instal·la'l. També es pot fer des de la pestanya *Community Mods* de l'app d'AZ-MODS.

Per provar-lo sense publicar: compila'l i empaqueta'l (a sota) i segueix
[Testing on your deck](https://github.com/Kyle-Hosman/az-mods-sdk/blob/main/docs/04-testing.md).

## Compilar-lo

Cal Docker (amb `linux/arm64`; al Mac n'hi ha prou amb Colima), Python 3 i el SDK al costat:

```sh
git clone https://github.com/Kyle-Hosman/az-mods-sdk ../az-mods-sdk
../az-mods-sdk/tools/azmod-pack build mala-idea-wallpaper     # compila i valida
../az-mods-sdk/tools/azmod-pack package mala-idea-wallpaper   # dist/mala-idea-wallpaper-<versió>.azmod
```

Les proves de l'SDK (al Mac, amb clang, sense Docker):

```sh
make -C mala-idea-wallpaper test AZ_SDK=$PWD/../az-mods-sdk AZ_SDK_INCLUDE=$PWD/../az-mods-sdk/include
```

## Canviar el fons

`tools/make_wallpaper.py` ressegueix `tools/mala-idea-logo.png` en ASCII (Pillow + numpy):

```sh
python3 -m pip install pillow numpy
python3 tools/make_wallpaper.py
```

Escriu `images/wallpaper.png` (una imatge fixa del logo en ASCII), l'icona i
`mala-idea-wallpaper/assets/corrent.mic`, el que anima el mod: el logo en cel·les de 6×12 px i
la forma de cada caràcter, perquè el mod dibuixi cada fotograma sense cap font ni llibreria
d'imatges.

## Publicar

Fes un tag amb la versió de `azmod.json`:

```sh
git tag mala-idea-wallpaper-0.2.1 && git push origin mala-idea-wallpaper-0.2.1
```

El workflow de `.github/workflows/` el compila al contenidor de l'SDK, el valida, penja el
`.azmod` i l'icona a la release i afegeix la versió a `azmods-repo.json`. Per sortir al Mod
Index, PR a [az-mods-index](https://github.com/Kyle-Hosman/az-mods-index).

## Llicència

GPL-3.0-only, com AZ-MODS i el seu SDK ([`LICENSE`](LICENSE)).

<p align="center"><sub>No afiliat amb AlphaTheta, Pioneer DJ ni rekordbox; les marques són dels seus propietaris.</sub></p>
