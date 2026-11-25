/*
    melonDS Link - languages, see LinkLang.h
*/

#include "LinkLang.h"

#include <map>
#include <mutex>

#include <QDir>
#include <QFile>
#include <QTextStream>

extern QString linkDirectory;

namespace Link
{

namespace
{

struct Entry { const char* key; const char* en; const char* de; };

// %1, %2... are filled in with QString::arg
const Entry kTexts[] = {
    {"language.name", "English", "Deutsch"},

    // menu
    {"menu.connect", "Connect to 3DS...", "Mit 3DS verbinden..."},
    {"menu.connected", "Connected to %1 - reconnect...", "Verbunden mit %1 - neu verbinden..."},
    {"menu.connecting", "Connecting to %1... - other IP...", "Verbinde mit %1... - andere IP..."},
    {"menu.disconnect", "Disconnect", "Trennen"},
    {"menu.roms", "ROMs and saves of the 3DS...", "ROMs und Spielstände vom 3DS..."},
    {"menu.folder", "Open backup and ROM folder", "Backup- und ROM-Ordner öffnen"},
    {"menu.language", "Language", "Sprache"},
    {"menu.debug", "Debug", "Debug"},
    {"menu.help", "Features and controls...", "Funktionen und Bedienung..."},
    {"help.title", "melonDS Link - features and controls", "melonDS Link - Funktionen und Bedienung"},
    {"help.text", "# On the 3DS\n- SELECT + START: melonDS Link menu - game list, screenshot (what it shows is set there too), save backups, display, bottom picture on/off, top screen off, battery/WiFi at the top, language, end the game on the PC\n- L + R + SELECT + START (hold): quit the app\n- L + R + A: confirms everything that cannot be undone (other game, end game, load backup, quit) - B goes back\n- Game list: A start, X favourite, Y reload, L/R page. \"PC\" = the ROM is already on the PC\n- Picking the game that runs only goes back to it - it is never restarted\n- C-stick down: game list, C-stick up: enlarged/original (New 3DS)\n- ZR (hold): fast forward, ZL: swap screens\n- Closing the 3DS pauses the game; after opening it continues by itself\n# Saves\n- Every save in the game goes to the 3DS right away; the last versions are kept on the PC as backups\n- If the save on the PC changed (played without the 3DS), the 3DS asks which one counts: A = PC, B = 3DS\n- Put a backup back: 3DS menu \"Save backups\", or here under \"ROMs and saves of the 3DS\" > Backups\n# In melonDS (menu 3DS Link)\n- ROMs and saves of the 3DS: copy games ahead of time, delete them from the PC, backups\n- Connect automatically, pause when the 3DS disconnects or is closed, screen black while closed\n- 3DS battery and WiFi on the screen (also kind to OLED screens), hints at 30/20/10 % battery\n- Messages in melonDS: each kind can be switched off\n- Language, Debug (WiFi test, SD card test, log)\n- Folders: melonDS-Link/ROMs, Backups, Screenshots, Language, Debug next to melonDS\n# Extras in this melonDS\n- Texture filter: Config > Video settings > Texture filter (OpenGL / compute shader) - MMPX keeps the pixel look, xBRZ is smoother, 2x or 4x, optionally only characters and objects\n- Real fullscreen: F11 or View > Fullscreen (no menu bar, no mouse pointer), Esc leaves it; the window size is kept\n- Screenshots from the 3DS in full rendering resolution, with filter and upscaling", "# Auf dem 3DS\n- SELECT + START: melonDS-Link-Menü – Spieleliste, Screenshot (was er zeigt, stellst du dort auch ein), Spielstand-Backups, Anzeige, Bild unten an/aus, oberen Bildschirm aus, Akku/WLAN oben, Sprache, Spiel am PC beenden\n- L + R + SELECT + START (halten): App beenden\n- L + R + A: bestätigt alles, was sich nicht rückgängig machen lässt (anderes Spiel, Spiel beenden, Backup laden, beenden) – B geht zurück\n- Spieleliste: A starten, X Favorit, Y neu laden, L/R blättern. „PC“ = die ROM liegt schon am PC\n- Das laufende Spiel auswählen führt nur zurück ins Spiel – es wird nie neu gestartet\n- C-Stick runter: Spieleliste, C-Stick hoch: Vergrößert/Original (New 3DS)\n- ZR (halten): Vorspulen, ZL: Bildschirme tauschen\n- Zuklappen pausiert das Spiel; nach dem Aufklappen geht es von selbst weiter\n# Spielstände\n- Jedes Speichern im Spiel landet sofort auf dem 3DS; die letzten Versionen bleiben am PC als Backup\n- Wurde der Spielstand am PC geändert (ohne 3DS gespielt), fragt der 3DS, welcher gilt: A = PC, B = 3DS\n- Backup zurückspielen: 3DS-Menü „Spielstand-Backups“ oder hier unter „ROMs und Spielstände vom 3DS“ > Backups\n# In melonDS (Menü 3DS Link)\n- ROMs und Spielstände vom 3DS: Spiele vorab kopieren, vom PC löschen, Backups\n- Automatisch verbinden, pausieren beim Trennen oder Zuklappen, Bild schwarz solange zugeklappt\n- 3DS-Akku und WLAN im Bild (auch OLED-schonend), Hinweise bei 30/20/10 % Akku\n- Hinweise in melonDS: jede Art einzeln abschaltbar\n- Sprache, Debug (WLAN-Test, SD-Karten-Test, Log)\n- Ordner neben melonDS: melonDS-Link/ROMs, Backups, Screenshots, Language, Debug\n# Extras in diesem melonDS\n- Texturfilter: Config > Video settings > Texture filter (OpenGL / Compute Shader) – MMPX behält den Pixel-Look, xBRZ ist weicher, 2x oder 4x, auf Wunsch nur Figuren und Objekte\n- Echter Vollbildmodus: F11 oder View > Fullscreen (ohne Menüleiste und Mauszeiger), Esc beendet ihn; die Fenstergröße bleibt erhalten\n- Screenshots vom 3DS in voller Berechnungsauflösung, mit Filter und Hochskalierung"},
    {"menu.pauseonlid", "Pause the game when the 3DS is closed", "Spiel pausieren, wenn der 3DS zugeklappt wird"},
    {"menu.blackout", "Screen black while the 3DS is closed (game paused)", "Bild schwarz, solange der 3DS zugeklappt ist (Spiel pausiert)"},
    {"menu.autoconnect", "Connect automatically when melonDS starts", "Beim Start automatisch verbinden"},
    {"menu.skipdup", "Skip presenting duplicate frames", "Doppelte Bilder nicht anzeigen"},
    {"menu.video", "Picture on the 3DS", "Bild auf dem 3DS"},
    {"menu.video.tile20", "20 pictures a second for 30 fps games too (uneven, but more often)", "20 Bilder pro Sekunde auch bei 30-fps-Spielen (ungleichmäßig, aber öfter)"},
    {"menu.video.prio", "WiFi priority of the picture (from the next connect):", "WLAN-Priorität des Bildes (ab der nächsten Verbindung):"},
    {"menu.video.prio.video", "Video (default)", "Video (Standard)"},
    {"menu.video.prio.background", "Background (lowest)", "Hintergrund (niedrigste)"},
    {"menu.video.prio.normal", "Normal (unmarked)", "Normal (ohne Markierung)"},
    {"menu.backupkeep", "Backups: keep the newest %1...", "Backups: die neuesten %1 behalten..."},
    {"menu.backupdays", "Daily backups: keep %1 days played...", "Tägliche Backups: %1 Spieltage behalten..."},
    {"menu.backups.title", "Save backups", "Spielstand-Backups"},
    {"menu.backupkeep.ask", "How many of the newest backups of a game are kept:", "Wie viele der neuesten Backups eines Spiels aufgehoben werden:"},
    {"menu.backupdays.ask", "Besides the newest backups, the first backup of each day the game was played is kept, for this many such days (0 = none):", "Zusätzlich zu den neuesten Backups wird das erste Backup jedes Tages, an dem gespielt wurde, aufgehoben - für so viele Spieltage (0 = keine):"},
    {"menu.status", "Show 3DS battery and WiFi", "3DS-Akku und WLAN anzeigen"},
    {"status.mode.off", "Off", "Aus"},
    {"status.mode.always", "Always", "Immer"},
    {"status.mode.moving", "Always, moving slightly (kind to OLED screens)", "Immer, leicht wandernd (schont OLED-Bildschirme)"},
    {"status.show.battery", "Battery", "Akku"},
    {"status.show.wifi", "WiFi", "WLAN"},
    {"status.battery", "3DS battery: %1", "3DS-Akku: %1"},
    {"status.wifi", "3DS WiFi: %1", "3DS-WLAN: %1"},
    {"menu.notes", "Messages in melonDS", "Hinweise in melonDS"},
    {"notes.connection", "Connected / disconnected", "Verbunden / getrennt"},
    {"notes.save", "Save written to the 3DS", "Spielstand auf den 3DS geschrieben"},
    {"notes.rom", "ROM copied", "ROM kopiert"},
    {"notes.screenshot", "Screenshot saved", "Screenshot gespeichert"},
    {"notes.pause", "Game paused / continues", "Spiel pausiert / geht weiter"},
    {"notes.battery", "Battery low", "Akku schwach"},
    {"status.mode.wander", "Always, moving slowly over a bigger area (OLED, safest)", "Immer, wandert langsam über einen größeren Bereich (OLED, am sichersten)"},
    {"status.mode.every", "Briefly every %1 minutes", "Alle %1 Minuten kurz"},
    {"pause.lid", "3DS Link: game paused - the 3DS was closed", "3DS Link: Spiel pausiert – der 3DS wurde zugeklappt"},
    {"battery.30", "3DS battery at %1 % - you may want to plug in the charger.", "3DS-Akku bei %1 % – am besten schon mal das Ladekabel anschließen."},
    {"battery.20", "3DS battery at %1 % - please plug in the charger.", "3DS-Akku bei %1 % – bitte das Ladekabel anschließen."},
    {"battery.10", "3DS battery at %1 % - it will turn off soon. Better save now.", "3DS-Akku bei %1 % – der 3DS geht bald aus. Speichere am besten jetzt."},
    {"battery.low.3ds", "Battery %1 % - please plug in the charger", "Akku %1 % – bitte Ladekabel anschließen"},
    {"battery.10.3ds", "Battery %1 % - turning off soon, save now!", "Akku %1 % – geht bald aus, jetzt speichern!"},
    {"status.line", "3DS: battery %1, WiFi %2", "3DS: Akku %1, WLAN %2"},
    {"status.charging", "(charging)", "(lädt)"},
    {"status.wifi0", "very weak", "sehr schwach"},
    {"status.wifi1", "weak", "schwach"},
    {"status.wifi2", "good", "gut"},
    {"status.wifi3", "very good", "sehr gut"},
    {"menu.pauseonloss", "Pause the game when the 3DS disconnects", "Spiel pausieren, wenn der 3DS getrennt wird"},
    {"pause.paused", "3DS Link: game paused - the 3DS is gone (continues when you are back in the game on the 3DS)",
                     "3DS Link: Spiel pausiert – der 3DS ist weg (geht weiter, sobald du am 3DS wieder im Spiel bist)"},
    {"pause.resumed", "3DS Link: game continues", "3DS Link: Spiel geht weiter"},
    {"stop.done", "3DS Link: game ended from the 3DS", "3DS Link: Spiel vom 3DS aus beendet"},
    {"menu.speedtest", "Test and tune WiFi speed...", "WLAN-Geschwindigkeit testen und optimieren..."},
    {"menu.sdtest", "Test the 3DS SD card...", "SD-Karte des 3DS testen..."},
    {"menu.debugfolder", "Open debug folder (log)", "Debug-Ordner öffnen (Log)"},
    {"menu.screenshots", "Open screenshot folder", "Screenshot-Ordner öffnen"},

    // connecting
    {"connect.prompt", "IP address of the 3DS (shown at the top of the 3DS screen in the melonDS Link app):",
                       "IP-Adresse des 3DS (steht oben auf dem 3DS-Bildschirm in der melonDS-Link-App):"},
    {"connect.connecting", "3DS Link: connecting to %1...", "3DS Link: verbinde mit %1..."},
    {"connect.disconnected", "3DS Link: disconnected", "3DS Link: getrennt"},
    {"connect.first", "Connect to the 3DS first.", "Zuerst mit dem 3DS verbinden."},
    {"client.badaddr", "3DS Link: invalid address: %1", "3DS Link: Adresse ungültig: %1"},
    {"client.version", "3DS Link: versions don't match - please update both programs",
                       "3DS Link: Versionen passen nicht zusammen – bitte beide Programme aktualisieren"},
    {"client.connected", "3DS Link: connected to %1 (%2)", "3DS Link: verbunden mit %1 (%2)"},
    {"client.lost", "3DS Link: connection lost - reconnecting...", "3DS Link: Verbindung verloren – verbinde neu..."},
    {"status.connected", "melonDS connected", "melonDS verbunden"},

    // starting a game
    {"prep.preparing", "Preparing %1...", "Bereite %1 vor..."},
    {"prep.copying", "Copying the ROM from the 3DS (only the first time):\n%1\n%2 of %3  -  %4/s  -  about %5 left\nWiFi: %6/s, compression saves %7%",
                     "ROM wird vom 3DS kopiert (nur beim ersten Mal):\n%1\n%2 von %3  -  %4/s  -  noch ca. %5\nWLAN: %6/s, Kompression spart %7%"},
    {"prep.romnotfound", "ROM not found on the 3DS: %1", "ROM auf dem 3DS nicht gefunden: %1"},
    {"prep.cancelled", "Cancelled (the part copied so far is kept)", "Abgebrochen (der bisher kopierte Teil bleibt erhalten)"},
    {"prep.fetchsave", "Getting the save from the 3DS...", "Hole Spielstand vom 3DS..."},
    {"prep.savereadfail", "The save could not be read from the 3DS", "Spielstand konnte nicht vom 3DS gelesen werden"},
    {"prep.savewritefail", "Cannot store the save: %1", "Kann Spielstand nicht speichern: %1"},
    {"prep.running", "Game running in melonDS", "Spiel läuft in melonDS"},
    {"prep.asking", "Which save should be used? Please answer on the 3DS...", "Welcher Spielstand soll genommen werden? Bitte am 3DS antworten..."},
    {"prep.uploadpc", "Copying the PC save to the 3DS...", "Kopiere den PC-Spielstand auf den 3DS..."},
    {"prep.uploadfail", "The PC save could not be written to the 3DS", "Der PC-Spielstand konnte nicht auf den 3DS geschrieben werden"},
    {"prep.cancel", "Cancel", "Abbrechen"},

    // ROM copy
    {"copy.cantstore", "Cannot store the ROM: %1", "Kann ROM nicht speichern: %1"},
    {"copy.lost", "Connection lost while copying the ROM (can be continued later)", "Verbindung verloren beim Kopieren der ROM (kann später fortgesetzt werden)"},
    {"copy.cancelled", "Cancelled", "Abgebrochen"},
    {"copy.status", "Copying ROM: %1%", "ROM wird kopiert: %1%"},
    {"copy.summary", "3DS Link: ROM copied in %1 - data %2/s, WiFi %3/s, compression saved %4%",
                     "3DS Link: ROM kopiert in %1 – Daten %2/s, WLAN %3/s, Kompression sparte %4%"},

    // saves
    {"save.otherplace", "3DS Link: careful, melonDS saves somewhere else than expected - save sync may be missing",
                        "3DS Link: Achtung, melonDS speichert an einem anderen Ort als erwartet – der Spielstand-Abgleich kann fehlen"},
    {"save.uploading", "3DS Link: writing the save to the 3DS...", "3DS Link: Spielstand wird auf den 3DS geschrieben..."},
    {"save.uploading3ds", "Writing save - don't switch off the 3DS", "Spielstand wird geschrieben – 3DS nicht ausschalten"},
    {"save.done", "3DS Link: save stored on the 3DS", "3DS Link: Spielstand auf dem 3DS gesichert"},
    {"save.done3ds", "Save stored %1", "Spielstand gesichert %1"},
    {"save.retry", "3DS Link: the save could not reach the 3DS - will retry (the copy on the PC is safe)",
                   "3DS Link: Spielstand konnte nicht auf den 3DS – wird nachgeholt (die Kopie am PC ist sicher)"},
    {"ask.newer", "The save on the PC is newer", "Der Spielstand am PC ist neuer"},
    {"ask.both", "Both saves were changed (on the PC and on the 3DS)", "Beide Spielstände wurden geändert (am PC und auf dem 3DS)"},
    {"ask.differ", "The saves on the PC and on the 3DS differ", "Die Spielstände am PC und auf dem 3DS sind verschieden"},
    {"ask.no3ds", "There is a save on the PC, but none on the 3DS", "Am PC gibt es einen Spielstand, auf dem 3DS keinen"},
    {"ask.nosave", "Start without a save", "Ohne Spielstand starten"},
    {"ask.pcdate", "PC save: %1", "PC-Spielstand: %1"},
    {"ask.note", "The other one is kept as a backup on the PC.", "Der andere wird am PC als Backup aufgehoben."},
    {"ask.usepc", "Use the PC save (copy it to the 3DS)", "PC-Spielstand nehmen (auf den 3DS kopieren)"},
    {"ask.use3ds", "Keep the 3DS save", "3DS-Spielstand behalten"},

    // screenshots
    {"shot.saved", "3DS Link: screenshot saved: %1", "3DS Link: Screenshot gespeichert: %1"},
    {"shot.saved3ds", "Screenshot saved", "Screenshot gespeichert"},
    {"shot.nogame", "No game running - no screenshot", "Kein Spiel läuft – kein Screenshot"},
    {"shot.failed", "3DS Link: screenshot failed", "3DS Link: Screenshot fehlgeschlagen"},

    // ROM / save window
    {"roms.title", "melonDS Link - ROMs and saves of the 3DS", "melonDS Link - ROMs und Spielstände vom 3DS"},
    {"roms.intro", "Copy games to the PC ahead of time: they then start right away when you pick them on the 3DS "
                   "(marked \"PC\" there). While copying, the bottom 3DS screen stands still - better not play meanwhile.\n"
                   "Backups: put an older save (or one played on the PC) back on the 3DS.",
                   "Hier kannst du Spiele vorab auf den PC kopieren. Danach starten sie sofort, wenn du sie auf dem 3DS "
                   "auswählst (dort steht dann \"PC\"). Während des Kopierens bleibt der untere 3DS-Bildschirm stehen – am besten nicht nebenbei spielen.\n"
                   "Backups: einen älteren (oder am PC gespielten) Spielstand wieder auf den 3DS bringen."},
    {"roms.col.game", "Game", "Spiel"},
    {"roms.col.size", "Size", "Größe"},
    {"roms.col.status", "Status", "Status"},
    {"roms.skip", "Skip padding (faster): behind the game data the 3DS only reads samples instead of everything. "
                  "If it finds anything there, it reads everything from then on.",
                  "Füllbereich überspringen (schneller): hinter den Spieldaten liest der 3DS nur Stichproben statt allem. "
                  "Findet er dort doch etwas, liest er ab da wieder alles."},
    {"roms.refresh", "Refresh", "Aktualisieren"},
    {"roms.copysel", "Copy ticked", "Angehakte kopieren"},
    {"roms.copyall", "Copy all missing", "Alle fehlenden kopieren"},
    {"roms.delete", "Delete ticked from the PC...", "Angehakte vom PC löschen..."},
    {"roms.backups", "Backups...", "Backups..."},
    {"roms.stop", "Stop", "Anhalten"},
    {"roms.close", "Close", "Schließen"},
    {"roms.stopping", "Stopping (after the current piece, a few seconds)...\nThe part copied so far is kept and continued next time.",
                      "Wird angehalten (nach dem aktuellen Abschnitt, ein paar Sekunden)...\nDer bisher kopierte Teil bleibt erhalten und wird beim nächsten Mal fortgesetzt."},
    {"roms.stopclose", "Stopping, then closing...", "Wird angehalten und dann geschlossen..."},
    {"roms.notconnected", "Not connected to the 3DS (3DS Link > Connect to 3DS).", "Nicht mit dem 3DS verbunden (3DS Link > Mit 3DS verbinden)."},
    {"roms.reading", "Reading the game list from the 3DS...", "Lese die Spieleliste vom 3DS..."},
    {"roms.readfail", "The game list could not be read. Is the newest melonDS Link app on the 3DS?",
                      "Die Spieleliste konnte nicht gelesen werden. Ist auf dem 3DS die neueste melonDS-Link-App?"},
    {"roms.onpc", "✓ on the PC", "✓ auf dem PC"},
    {"roms.started", "started (%1%)", "angefangen (%1%)"},
    {"roms.missing", "missing", "fehlt"},
    {"roms.summary", "%1 of %2 games on the PC%3   (folder on the 3DS: %4)", "%1 von %2 Spielen auf dem PC%3   (Ordner auf dem 3DS: %4)"},
    {"roms.left", ", %1 left to copy", ", noch %1 zu kopieren"},
    {"roms.nonemissing", "None of the ticked games is missing on the PC.", "Keins der angehakten Spiele fehlt auf dem PC."},
    {"roms.allthere", "All games are on the PC already.", "Alle Spiele sind schon auf dem PC."},
    {"roms.starting", "Starting...", "Starte..."},
    {"roms.progress", "Copying %1 of %2: %3\n%4 of %5  -  %6/s  -  WiFi %7/s, compression saves %8%\nAll together %9 of %10  -  about %11 left",
                      "Kopiere %1 von %2: %3\n%4 von %5  -  %6/s  -  WLAN %7/s, Kompression spart %8%\nInsgesamt %9 von %10  -  noch ca. %11"},
    {"roms.stoppedlaunch", "Stopped because a game was started on the 3DS. The part copied so far is kept.",
                           "Angehalten, weil auf dem 3DS ein Spiel gestartet wurde. Der bisher kopierte Teil bleibt erhalten."},
    {"roms.stopped", "Stopped. The part copied so far is kept and continued next time.",
                     "Angehalten. Der bisher kopierte Teil bleibt erhalten und wird beim nächsten Mal fortgesetzt."},
    {"roms.done", "Done: %1 game(s) copied in %2.", "Fertig: %1 Spiel(e) kopiert in %2."},
    {"roms.nonetodelete", "None of the ticked games is on the PC.", "Keins der angehakten Spiele liegt auf dem PC."},
    {"roms.askdelete", "Delete %1 game(s) from the PC (%2)?\n\nEverything stays on the 3DS, the saves too. Next time the ROM is simply copied again.",
                       "%1 Spiel(e) vom PC löschen (%2)?\n\nAuf dem 3DS bleibt alles, auch die Spielstände. Beim nächsten Start wird die ROM einfach wieder kopiert."},
    {"roms.delbackups", "Also delete the save backups of these games on the PC", "Auch die Spielstand-Backups dieser Spiele am PC löschen"},
    {"roms.deleted", "%1 game(s) deleted from the PC.", "%1 Spiel(e) vom PC gelöscht."},
    {"roms.notdeleted", " Not deleted (running or being copied): %1", " Nicht gelöscht (läuft gerade oder wird kopiert): %1"},
    {"roms.pickone", "Tick exactly one game for its backups.", "Für die Backups genau ein Spiel anhaken."},

    // backups
    {"bak.title", "Backups - %1", "Backups – %1"},
    {"bak.intro", "Saves of this game kept on the PC (newest first). \"Put on the 3DS\" replaces the save on the 3DS; "
                  "the current one there is backed up first.",
                  "Am PC aufgehobene Spielstände dieses Spiels (neueste zuerst). \"Auf den 3DS übertragen\" ersetzt den "
                  "Spielstand auf dem 3DS; der aktuelle dort wird vorher gesichert."},
    {"bak.none", "No backups of this game yet.", "Noch keine Backups von diesem Spiel."},
    {"bak.restore", "Put on the 3DS", "Auf den 3DS übertragen"},
    {"bak.confirm", "Replace the save on the 3DS with the backup from %1?\n\nThe current save on the 3DS is kept as a backup.",
                    "Den Spielstand auf dem 3DS durch das Backup vom %1 ersetzen?\n\nDer aktuelle Spielstand auf dem 3DS wird als Backup aufgehoben."},
    {"bak.running", "This game is running in melonDS right now - quit it first.", "Dieses Spiel läuft gerade in melonDS – bitte zuerst beenden."},
    {"bak.done", "The backup is now the save on the 3DS.", "Das Backup ist jetzt der Spielstand auf dem 3DS."},
    {"bak.failed", "That did not work: %1", "Das hat nicht geklappt: %1"},
    {"bak.done3ds", "Backup loaded", "Backup geladen"},
    {"bak.failed3ds", "Backup not loaded", "Backup nicht geladen"},
    {"bak.gone", "the list has changed meanwhile - please open it again", "die Liste hat sich inzwischen geändert – bitte neu öffnen"},
    {"bak.working", "Writing to the 3DS...", "Schreibe auf den 3DS..."},

    // tests
    {"test.askspeed", "The 3DS now sends 8 MB of test data several times with different settings (takes about 1-2 minutes). "
                      "The fastest setting is used from then on.\n\nStart?",
                      "Der 3DS sendet jetzt mehrmals je 8 MB Testdaten mit verschiedenen Einstellungen (dauert ca. 1–2 Minuten). "
                      "Die schnellste Einstellung wird danach automatisch verwendet.\n\nStarten?"},
    {"test.speedrunning", "3DS Link: WiFi test running...", "3DS Link: WLAN-Test läuft..."},
    {"test.speedline", "%1 buffer %2K  piece %3K:  %4", "%1 Puffer %2K  Stück %3K:  %4"},
    {"test.blocking", "blocking", "blockierend"},
    {"test.polling", "polling", "abfragend"},
    {"test.error", "error", "Fehler"},
    {"test.best", "Best setting: %1, buffer %2K, piece %3K -> %4 KB/s (used from now on)",
                  "Beste Einstellung: %1, Puffer %2K, Stück %3K -> %4 KB/s (wird ab jetzt verwendet)"},
    {"test.results", "Results (3DS -> PC, pure WiFi speed):", "Ergebnisse (3DS -> PC, reine WLAN-Geschwindigkeit):"},
    {"test.vary", "The values vary a lot with the WiFi load; one test is only a snapshot.\nEverything is also in melonDS-Link\\Debug\\link-log.txt.",
                  "Die Werte schwanken je nach WLAN-Auslastung stark; ein Test ist nur eine Momentaufnahme.\nAlles steht auch in melonDS-Link\\Debug\\link-log.txt."},
    {"test.failed", "Test failed. Is the newest melonDS Link app on the 3DS?", "Test fehlgeschlagen. Ist auf dem 3DS die neueste melonDS-Link-App?"},
    {"test.sdrunning", "3DS Link: SD card test running (up to a minute)...", "3DS Link: SD-Karten-Test läuft (bis zu einer Minute)..."},
    {"test.sdtitle", "melonDS Link - SD card", "melonDS Link - SD-Karte"},
    {"test.inlog", "(also in melonDS-Link\\Debug\\link-log.txt)", "(steht auch in melonDS-Link\\Debug\\link-log.txt)"},
};

// The files carry a fingerprint of the built-in texts: when a new version of
// melonDS changes a text, en.txt/de.txt are written anew, so the change shows
// up (own edits there are lost then - own texts belong in a file with a new
// name, e.g. de-mine.txt, which becomes a language of its own).
QString fileVersion()
{
    static QString v;
    if (v.isEmpty())
    {
        uint32_t h = 2166136261u;
        for (const Entry& e : kTexts)
            for (const char* t : {e.key, e.en, e.de})
                for (const char* c = t; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
        v = QString::number(h, 16);
    }
    return v;
}

std::mutex g_mutex;
std::string g_code = "en";
std::map<std::string, std::map<std::string, QString>> g_langs;   // code -> key -> text
std::map<std::string, QString> g_english;

QString langDir()
{
    return linkDirectory + "/Language";
}

// "key=value", '#' comments, "\n" for line breaks
std::map<std::string, QString> readFile(const QString& path)
{
    std::map<std::string, QString> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    while (!in.atEnd())
    {
        QString line = in.readLine();
        if (line.startsWith('#') || line.trimmed().isEmpty()) continue;
        int eq = line.indexOf('=');
        if (eq <= 0) continue;
        QString value = line.mid(eq + 1);
        value.replace("\\n", "\n");
        out[line.left(eq).trimmed().toStdString()] = value;
    }
    return out;
}

void writeFile(const QString& path, bool german)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) return;
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    out << "# melonDS Link - " << (german ? "Deutsch" : "English") << "\n";
    out << "# key=text (UTF-8). %1, %2... are filled in. \\n = new line.\n";
    out << "# Written anew when melonDS changes its texts. For own texts copy it to a new name\n";
    out << "# (e.g. fr.txt or de-mine.txt): that becomes a language of its own; missing keys fall back to English.\n";
    out << "version=" << fileVersion() << "\n\n";
    for (const Entry& e : kTexts)
    {
        QString v = QString::fromUtf8(german ? e.de : e.en);
        v.replace("\n", "\\n");
        out << e.key << "=" << v << "\n";
    }
}

}

void InitLanguages(const std::string& code)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_english.clear();
    std::map<std::string, QString> german;
    for (const Entry& e : kTexts)
    {
        g_english[e.key] = QString::fromUtf8(e.en);
        german[e.key] = QString::fromUtf8(e.de);
    }
    g_langs.clear();
    g_langs["en"] = g_english;
    g_langs["de"] = german;

    // the folder with the editable files; the built-in ones are refreshed
    // when the program has new texts (a file for another language stays as it is)
    QDir().mkpath(langDir());
    for (const char* c : {"en", "de"})
    {
        QString path = langDir() + "/" + c + ".txt";
        auto existing = readFile(path);
        auto ver = existing.find("version");
        if (ver == existing.end() || ver->second != fileVersion()) existing.clear();
        existing.erase("version");
        bool complete = !existing.empty();
        for (const Entry& e : kTexts) if (!existing.count(e.key)) { complete = false; break; }
        if (!complete)
        {
            // keep the user's own edits, add what is new
            writeFile(path, std::string(c) == "de");
            if (!existing.empty())
            {
                auto& target = g_langs[c];
                for (auto& [k, v] : existing) target[k] = v;
                // write the merged result back
                QFile f(path);
                if (f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
                {
                    QTextStream out(&f);
                    out.setEncoding(QStringConverter::Utf8);
                    out << "# melonDS Link - " << target["language.name"] << "\n";
                    out << "# key=text (UTF-8). %1, %2... are filled in. \\n = new line.\n";
                    out << "version=" << fileVersion() << "\n\n";
                    for (const Entry& e : kTexts)
                    {
                        QString v = target[e.key];
                        v.replace("\n", "\\n");
                        out << e.key << "=" << v << "\n";
                    }
                }
            }
        }
        else
        {
            auto& target = g_langs[c];
            for (auto& [k, v] : existing) target[k] = v;
        }
    }
    // further languages
    for (const QString& file : QDir(langDir()).entryList({"*.txt"}, QDir::Files))
    {
        std::string c = file.left(file.size() - 4).toStdString();
        if (c == "en" || c == "de") continue;
        auto texts = readFile(langDir() + "/" + file);
        if (texts.empty()) continue;
        auto& target = g_langs[c];
        target = g_english;
        for (auto& [k, v] : texts) target[k] = v;
    }
    g_code = g_langs.count(code) ? code : "en";
}

void SetLanguage(const std::string& code)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_langs.count(code)) g_code = code;
}

std::string Language()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_code;
}

std::vector<std::pair<std::string, QString>> Languages()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<std::pair<std::string, QString>> out;
    for (auto& [code, texts] : g_langs)
    {
        auto it = texts.find("language.name");
        out.push_back({code, it != texts.end() ? it->second : QString::fromStdString(code)});
    }
    return out;
}

QString Tr(const char* key)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_english.empty())
    {
        // used before InitLanguages: built-in English
        for (const Entry& e : kTexts) if (std::string(e.key) == key) return QString::fromUtf8(e.en);
        return QString::fromUtf8(key);
    }
    auto lang = g_langs.find(g_code);
    if (lang != g_langs.end())
    {
        auto it = lang->second.find(key);
        if (it != lang->second.end()) return it->second;
    }
    auto it = g_english.find(key);
    return it != g_english.end() ? it->second : QString::fromUtf8(key);
}

QString TrIn(const std::string& code, const char* key)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto lang = g_langs.find(code);
        if (lang != g_langs.end())
        {
            auto it = lang->second.find(key);
            if (it != lang->second.end()) return it->second;
        }
        if (!g_english.empty())
        {
            auto it = g_english.find(key);
            if (it != g_english.end()) return it->second;
        }
    }
    for (const Entry& e : kTexts) if (std::string(e.key) == key) return QString::fromUtf8(code == "de" ? e.de : e.en);
    return QString::fromUtf8(key);
}

std::string TrS(const char* key)
{
    return Tr(key).toStdString();
}

}
