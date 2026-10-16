<div align="center">

<img src="mala-idea-wallpaper/icon.png" width="140" alt="Mala Idea">

**Mods de la comunitat AZ-MODS per a l'XDJ-AZ de Mala Idea**

</div>

Mods per a l'[AZ-MODS](https://github.com/Kyle-Hosman/xdj-az-mods) de l'XDJ-AZ, fets amb
l'[AZ-MODS SDK](https://github.com/Kyle-Hosman/az-mods-sdk). Cal tenir el deck amb el
firmware d'AZ-MODS (2.01, core 1.2.3 o més nou).

| Mod | Què fa |
|:--|:--|
| [**Mala Idea Wallpaper**](mala-idea-wallpaper) | Pantalla de repòs: el logo de Mala Idea en ASCII sobre negre |

<p align="center"><img src="images/wallpaper.png" width="720" alt="El fons de Mala Idea"></p>

## Mala Idea Wallpaper

Quan el deck queda quiet (cap toc a la pantalla, cap gir del browse, cap deck sonant), el
logo de Mala Idea tapa tota la pantalla: la bombolla i les lletres resseguides en ASCII
blanc sobre negre, a partir del logo original (`tools/mala-idea-logo.png`).

En tocar la pantalla, girar el browse, carregar o posar en marxa un tema, el deck torna.
El toc que treu el fons no arriba al deck, així que no prem res del que hi havia a sota.

Al **Mod Menu** surt la fila *Mala Idea Wallpaper*: activar-lo o no, **Show after**
(1, 2, 5, 10 o 30 minuts; per defecte 5) i **Show now** per veure'l a l'instant.

> [!NOTE]
> Per què no és un fons *darrere* les ones? AZ-MODS deixa que un mod de la comunitat pinti
> només **per sobre** de la pantalla del deck, mai per sota. Un fons de veritat darrere la
> interfície voldria hooks al dibuix del firmware (adreces que canvien a cada versió), així
> que de moment és una pantalla de repòs.

## Instal·lar-lo

Un cop publicat (vegeu *Publicar*), al deck: **Mod Menu > Community Mods > Repositories**,
afegeix `eruizimade/az-mods-malaidea` i instal·la *Mala Idea Wallpaper*. També es pot fer
des de la pestanya *Community Mods* de l'app d'AZ-MODS.

Per provar-lo sense publicar: compila'l i empaqueta'l (a sota) i segueix
[Testing on your deck](https://github.com/Kyle-Hosman/az-mods-sdk/blob/main/docs/04-testing.md).

## Compilar-lo

Cal Docker (amb `linux/arm64`), Python 3 i el SDK al costat:

```sh
git clone https://github.com/Kyle-Hosman/az-mods-sdk ../az-mods-sdk
../az-mods-sdk/tools/azmod-pack build mala-idea-wallpaper     # compila i valida
../az-mods-sdk/tools/azmod-pack package mala-idea-wallpaper   # dist/mala-idea-wallpaper-0.1.0.azmod
```

Les proves de l'SDK (al Mac, amb clang, sense Docker):

```sh
make -C mala-idea-wallpaper test AZ_SDK=$PWD/../az-mods-sdk AZ_SDK_INCLUDE=$PWD/../az-mods-sdk/include
```

## Canviar el fons

`tools/make_wallpaper.py` ressegueix `tools/mala-idea-logo.png` en ASCII i en fa el fons i l'icona (Pillow + numpy):

```sh
python3 -m pip install pillow numpy
python3 tools/make_wallpaper.py
```

Escriu `images/wallpaper.png` (la vista prèvia), `images/wallpaper-on-deck.png` (com es veurà,
amb 256 colors) i `mala-idea-wallpaper/assets/wallpaper.miw`, el que carrega el mod: un format
mínim (paleta de 256 colors + un byte per píxel) perquè el mod no necessiti cap llibreria
d'imatges.

## Publicar

Puja el repo a GitHub i fes un tag amb la versió de `azmod.json`:

```sh
git tag mala-idea-wallpaper-0.1.0 && git push origin mala-idea-wallpaper-0.1.0
```

El workflow de `.github/workflows/release.yml` el compila al contenidor de l'SDK, el valida i
penja el `.azmod` i l'icona a la release. Per sortir al Mod Index, PR a
[az-mods-index](https://github.com/Kyle-Hosman/az-mods-index).

## Llicència

GPL-3.0-only, com AZ-MODS i el seu SDK ([`LICENSE`](LICENSE)).

<p align="center"><sub>No afiliat amb AlphaTheta, Pioneer DJ ni rekordbox; les marques són dels seus propietaris.</sub></p>
