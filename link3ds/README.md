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
5 Versionen liegen zusätzlich in `melonDS-Link/Backups/<Spiel>/` im melonDS-Ordner
(in melonDS: **3DS Link → Backup- und ROM-Ordner öffnen**).

**Spieleliste:** zeigt alle `.nds` aus `/roms/nds` (änderbar: `romdir=` in
`sd:/3ds/melonDS-Link.cfg`). Steuerkreuz/Circle Pad wählen, L/R blättern,
A startet, X markiert als Favorit (steht dann mit * oben), Y lädt die Liste neu,
B schließt sie. Beim Start steht die Auswahl auf dem zuletzt gespielten Spiel.
Favoriten: `sd:/3ds/melonDS-Link-favoriten.txt`. **C-Stick nach unten** öffnet die
Liste jederzeit; solange sie offen ist, bekommt das Spiel keine Eingaben.
Spiele mit grünem **PC** am Ende liegen schon vollständig auf dem PC und starten sofort.

**ROMs vorab kopieren:** in melonDS **3DS Link → ROMs vom 3DS (vorab kopieren)…**.
Das Fenster zeigt alle Spiele des 3DS mit Status (*auf dem PC*, *angefangen*, *fehlt*).
„Alle fehlenden kopieren“ oder einzelne anhaken. Anhalten ist jederzeit möglich –
der kopierte Teil bleibt erhalten und wird beim nächsten Mal fortgesetzt (auch beim
Spielstart). Wählt man währenddessen auf dem 3DS ein Spiel, hält das Vorladen an und
das Spiel geht vor. Während des Kopierens steht der untere 3DS-Bildschirm still.
„Angehakte vom PC löschen…“ entfernt ROMs wieder vom PC (auf dem 3DS bleibt alles);
auf Wunsch auch die Spielstand-Backups dieser Spiele.

**Füllbereich überspringen** (Häkchen im selben Fenster, aus = Standard): DS-ROMs sind
hinter den eigentlichen Spieldaten mit Füllbytes aufgefüllt. Mit Häkchen liest der 3DS
diesen Bereich nicht mehr von der SD-Karte, sondern prüft nur Anfang und Ende jedes
256-KB-Blocks. Findet er dort etwas anderes als Füllbytes, liest er ab da wieder alles.
Nur Daten, die mitten im Füllbereich versteckt sind, würden so übersehen – bei normalen
ROMs gibt es das nicht. Spart bei einem 512-MB-Spiel etwa 20–30 Sekunden.

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

ROMs werden komprimiert übertragen (LZ4, leere Füllbereiche als Marker); der
Fortschritt zeigt, wie viel das spart. LZ4 © Yann Collet, BSD-2-Lizenz
(`src/frontend/qt_sdl/lz4/LICENSE`).

Protokoll: `src/frontend/qt_sdl/LinkProto.h` (TCP 6543, UDP 6544 Eingaben, UDP 6545 Bild).
PC-App und 3DS-App müssen dieselbe Protokollversion haben.
