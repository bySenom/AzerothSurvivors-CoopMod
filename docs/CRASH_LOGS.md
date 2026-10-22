# Crash reports (dev.10)

Im Mod-Starter **Crashlogs** anklicken. Nach einem unerwarteten Abbruch entsteht
dort automatisch ein ZIP, pro Spielstart mit eigener Zeitkennung. Das neueste
ZIP des abgestürzten Gasts zusammen mit dem verwendeten Mod-/Spielbuild teilen.
Der lokale Ordner ist:

    %LOCALAPPDATA%\AzerothSurvivorsCoopMod\logs\crashes

Die letzten Zustände und Exit-Berichte aller Starts liegen unter logs/sessions.
Alte Berichte werden beim nächsten Spielstart nicht überschrieben. Ein schon
laufendes Spiel muss nach dem Update neu über den Mod-Starter gestartet werden.
Für einen normalen Exit mit Code 0 wird kein Crash-ZIP erzeugt.

## Erfasste Daten

- session.json: UTC-Start, Mod-Version, exakter Spiel-Hash und Build-Adapter.
- watcher.json / exit.json: Prozessidentität, UTC-Ende, Exit-Code, Host-/Gastrolle
  soweit vor dem Abbruch beobachtet, Klassifikation.
- status.json: maximal acht letzte Stichproben der vier Mod-Status-Pipes. Ein
  nicht erreichbares Modul wird markiert; Abfragen haben kurze Zeitlimits.
- modules.json / mod-files.json: letzte Modul-Adressen und Mod-Dateihashes, damit
  auch die unveränderlichen Cache-DLL-Namen einem Build zugeordnet werden können.
- native-crash.txt: Exception-Code, Thread und Register der erfassten Exception.
- crash.dmp / dump-result.json: Minidump mit Exception-Kontext, Threads und
  Modulen, sofern Windows und der noch laufende Prozess das Schreiben zulassen.

Der ZIP-Inhalt ist auf feste Dateinamen begrenzt. Spielstände, settings.json,
Lobbycodes und Spiel-Assets werden nicht als Dateien aufgenommen. Dumps enthalten
Speicher-Ausschnitte und können persönliche Pfade oder Daten im Stack enthalten;
Berichte werden ausschließlich lokal gespeichert und nie automatisch versendet.

## Technische Grenzen

Der native Starter startet das Spiel angehalten, verbindet einen unabhängigen
Beobachter mit dessen Prozesshandle und setzt es erst danach fort. Der Helfer
bleibt auch nach dem Schließen des Starters aktiv. Die native Exception-Routine
schreibt ohne C++-Heap/Streams in eine vorab geöffnete Datei und signalisiert den
externen Dump-Schreiber. Sie wartet höchstens acht Sekunden und setzt danach die
normale Windows-Exception-Verarbeitung fort.

Bei Fail-fast, Task-Manager-Abbruch oder einem vor der Mod-Injektion auftretenden
Fehler bleibt der Exit-Bericht erhalten; ein Minidump ist dabei nicht garantiert.
Ein Exit-Code allein unterscheidet Absturz und externes Beenden nicht zuverlässig.
Exit 0 beweist keinen fehlerfreien Lauf. Ein kompletter Rechnerabsturz/Stromausfall
kann den Beobachter ebenfalls beenden und erzeugt keinen fertigen Bericht.

DbgHelp-Vertrag: [MiniDumpWriteDump](https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump).

## Verifikation

    python tests/test_crash_watch.py --output runtime/crash-tests

Erzeugt eigene kurzlebige Fixture-Prozesse: native unbehandelte Exception mit
validiertem MDMP-Dump, normaler Exit, erzwungenes Beenden und Fail-fast. Es wird
kein Benutzer-Spiel absichtlich zum Absturz gebracht. Die Tests prüfen außerdem,
dass eingestreute Save-/Einstellungsdateien nicht im ZIP landen.

    python tests/test_native_guest_factory.py --game <current.exe> --legacy <old.exe> --output runtime/guest-tests

Startet echte isolierte Host-/Gast-Prozesse mit den vorhandenen Spiel-Assets,
ruft die detourte native Gast-Factory für Welt- und Auth-Dienste auf und prüft
die Originalantworten. Prüft auch Statusaufzeichnung nach Ende des Startprozesses.
Dies ist kein gerenderter Zwei-PC-Koop-Abnahmetest.
