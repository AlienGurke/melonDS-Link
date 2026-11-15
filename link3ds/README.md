# melonDS Link – 3DS-App

Macht deinen 3DS zum Controller für melonDS am PC (Tasten + Touchscreen)
und gibt melonDS Zugriff auf die SD-Karte (Spielstände, später ROMs).

## Benutzung
1. `melonDS-Link.3dsx` nach `sd:/3ds/` kopieren.
2. Auf dem 3DS über den Homebrew Launcher starten. Oben steht die IP-Adresse.
3. In melonDS: **3DS Link → Mit 3DS verbinden…** und die IP eingeben.
4. Spielen. Der untere 3DS-Bildschirm zeigt das DS-Touchfeld (256×192, mittig).

| 3DS            | melonDS                      |
|----------------|------------------------------|
| A B X Y L R    | A B X Y L R                  |
| Start / Select | Start / Select               |
| Steuerkreuz / Circle Pad | Steuerkreuz        |
| Touchscreen    | Touchscreen (1:1, mittig)    |
| ZR (halten)    | Vorspulen                    |
| ZL             | Bildschirme tauschen         |

**Beenden:** SELECT + START ca. 1 Sekunde halten.

## Bauen
Mit devkitPro/devkitARM: `make -C link3ds`. GitHub Actions baut die App
automatisch (Workflow „3DS (melonDS Link)“).

Protokoll: `src/frontend/qt_sdl/LinkProto.h` (TCP 6543, UDP 6544).
