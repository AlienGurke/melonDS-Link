# melonDS Link – 3DS-App

Macht deinen 3DS zum Controller für melonDS am PC (Tasten + Touchscreen),
zeigt den unteren DS-Bildschirm auf dem 3DS an und gibt melonDS Zugriff auf
die SD-Karte (Spielstände, später ROMs).

## Benutzung
1. `melonDS-Link.cia` mit FBI installieren (Symbol im Home-Menü) – oder
   `melonDS-Link.3dsx` nach `sd:/3ds/` kopieren und über den Homebrew Launcher starten.
2. Oben steht die IP-Adresse. In melonDS: **3DS Link → Mit 3DS verbinden…** und die IP eingeben.
3. Auf dem 3DS ein Spiel aus der Liste wählen (A). melonDS kopiert die ROM beim
   ersten Mal vom 3DS (`melonDS-Link/ROMs/` neben melonDS), holt den aktuellen
   Spielstand vom 3DS und startet das Spiel.
4. Spielen. Der untere 3DS-Bildschirm zeigt den unteren DS-Bildschirm –
   egal, was am PC-Monitor angezeigt wird (auch bei „nur oberer Bildschirm“).

**Spielstände:** Gespeichert wird immer auf dem 3DS (`<ROM-Ordner>/saves/<Spiel>.sav`,
wie TWiLight Menu++). Jedes Speichern im Spiel wird sofort auf den 3DS geschrieben –
erst in eine Zwischendatei, dann ausgetauscht, so dass ein WLAN-Abbruch nichts
beschädigt. Ist der 3DS gerade nicht erreichbar, holt melonDS das nach. Die letzten
5 Versionen liegen zusätzlich in `melonDS-Link/Backups/<Spiel>/` neben melonDS.

**Spieleliste:** zeigt alle `.nds` aus `/roms/nds` (änderbar: `romdir=` in
`sd:/3ds/melonDS-Link.cfg`). Steuerkreuz/Circle Pad wählen, L/R blättern,
A startet, Y lädt die Liste neu, B schließt sie. **C-Stick nach unten** öffnet die
Liste jederzeit; solange sie offen ist, bekommt das Spiel keine Eingaben.

**Anzeigemodi** (C-Stick nach oben wechselt, die Wahl wird gespeichert):
- *Vergrößert* (Standard): auf den ganzen 3DS-Bildschirm (320×240) skaliert.
- *Original*: 256×192 Pixel 1:1, mittig – gestochen scharf.

Das Bild wird kachelweise übertragen und mit Farbpaletten komprimiert. Jede
Änderung wird immer komplett gesendet – passt sie nicht ins Datenbudget, kommt
sie kurz in halber oder Viertel-Auflösung und wird danach scharf nachgeliefert.
Der 3DS bestätigt empfangene Pakete; verlorene Kacheln schickt melonDS sofort
neu. Ein Bild wird erst angezeigt, wenn alles dafür Nötige angekommen ist, so
gibt es keine halb alten, halb neuen Bilder. Die Datenrate passt sich ans WLAN an.

| 3DS            | melonDS                      |
|----------------|------------------------------|
| A B X Y L R    | A B X Y L R                  |
| Start / Select | Start / Select               |
| Steuerkreuz / Circle Pad | Steuerkreuz        |
| Touchscreen    | Touchscreen (passend zum Anzeigemodus) |
| C-Stick hoch   | Original / Vergrößert        |
| C-Stick runter | Spieleliste                  |
| ZR (halten)    | Vorspulen                    |
| ZL             | Bildschirme tauschen         |

**Beenden:** SELECT + START ca. 1 Sekunde halten.

## Bauen
Mit devkitPro/devkitARM: `make -C link3ds`. GitHub Actions baut die App
automatisch (Workflow „3DS (melonDS Link)“).

Einstellungen: `sd:/3ds/melonDS-Link.cfg`.

Protokoll: `src/frontend/qt_sdl/LinkProto.h` (TCP 6543, UDP 6544 Eingaben, UDP 6545 Bild).
PC-App und 3DS-App müssen dieselbe Protokollversion haben.
