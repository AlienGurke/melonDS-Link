# melonDS Link – 3DS-App

Macht deinen 3DS zum Controller für melonDS am PC (Tasten + Touchscreen),
zeigt den unteren DS-Bildschirm auf dem 3DS an und gibt melonDS Zugriff auf
die SD-Karte (Spielstände, später ROMs).

## Benutzung
1. `melonDS-Link.3dsx` nach `sd:/3ds/` kopieren.
2. Auf dem 3DS über den Homebrew Launcher starten. Oben steht die IP-Adresse.
3. In melonDS: **3DS Link → Mit 3DS verbinden…** und die IP eingeben.
4. Spielen. Der untere 3DS-Bildschirm zeigt den unteren DS-Bildschirm –
   egal, was am PC-Monitor angezeigt wird (auch bei „nur oberer Bildschirm“).

**Anzeigemodi** (C-Stick nach oben wechselt, die Wahl wird gespeichert):
- *Vergrößert* (Standard): auf den ganzen 3DS-Bildschirm (320×240) skaliert.
- *Original*: 256×192 Pixel 1:1, mittig – gestochen scharf.

Das Bild wird kachelweise übertragen und mit Farbpaletten komprimiert. Der 3DS
bestätigt empfangene Pakete; verlorene Kacheln schickt melonDS sofort neu.

| 3DS            | melonDS                      |
|----------------|------------------------------|
| A B X Y L R    | A B X Y L R                  |
| Start / Select | Start / Select               |
| Steuerkreuz / Circle Pad | Steuerkreuz        |
| Touchscreen    | Touchscreen (passend zum Anzeigemodus) |
| C-Stick hoch   | Original / Vergrößert        |
| ZR (halten)    | Vorspulen                    |
| ZL             | Bildschirme tauschen         |

**Beenden:** SELECT + START ca. 1 Sekunde halten.

## Bauen
Mit devkitPro/devkitARM: `make -C link3ds`. GitHub Actions baut die App
automatisch (Workflow „3DS (melonDS Link)“).

Einstellungen: `sd:/3ds/melonDS-Link.cfg`.

Protokoll: `src/frontend/qt_sdl/LinkProto.h` (TCP 6543, UDP 6544 Eingaben, UDP 6545 Bild).
PC-App und 3DS-App müssen dieselbe Protokollversion haben.
