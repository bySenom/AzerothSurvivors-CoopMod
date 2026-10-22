# Azeroth Survivors Koop-Mod

Experimental two-player co-op mod for the original Windows game. **This is an
unfinished preview.** A complete rendered two-PC co-op run has not been accepted.
The repository and releases contain mod files only, with no game executable,
MPQs, game assets or personal character profiles.

## Installieren

1. Unter [Releases](https://github.com/bySenom/AzerothSurvivors-CoopMod/releases)
   die aktuelle Preview öffnen und **AzerothSurvivors-Koop-Mod.zip** herunterladen.
2. Den gesamten Inhalt direkt in den Spielordner entpacken. Der Koop-Starter und
   der Ordner AzerothCoopMod müssen neben AzerothSurvivors.exe und Data liegen.
3. **Azeroth Survivors Koop.exe** öffnen, dann **Mit Mod spielen**.
4. Im Spiel Wald von Elwynn betreten anklicken → Solo / Koop.
5. Bei Koop eine Lobby erstellen oder den Code des Gastgebers eingeben.

Windows 10/11 x64 mit .NET Framework 4.8. Das Spiel ist separat erforderlich.
Ein separates Setup ist für die ZIP-Version nicht nötig. Der Starter verwendet
die Spielinstallation in seinem Ordner. Die Mod-Dateien liegen in AzerothCoopMod;
Spielstände und Einstellungen bleiben unter %LOCALAPPDATA%\AzerothSurvivorsCoopMod.
Der Spielordner muss zum Entpacken und Aktualisieren beschreibbar sein.

Der Starter prüft beim Öffnen automatisch auf Mod-Updates und installiert
verifizierte neue Versionen im selben Spielordner. Während eines Mod-Spiels wartet das Update.
Automatische Updates können im Starter deaktiviert werden; die manuelle
Update-Prüfung bleibt verfügbar. Einstellungen und Mod-Spielstände bleiben erhalten.

Die Setup-EXE bleibt für ältere Installationen und als automatisch gestarteter
Update-Helfer verfügbar. Bei einem einzeln kopierten Starter ohne vollständiges
Paket erscheint ab dev.7 eine Download-Hilfe; eine vorhandene vollständige ältere
Setup-Installation kann als Rückfall geöffnet werden.

## Spielkompatibilität

Aktuell unterstützt: 0.104.0 / 9845e5c7, EXE SHA-256:

    96e24dcd0e53fc13a6fdef5826aa46f77a29aa1aad4abd585adf04e94b2756d3

Neuere Spielbuilds benötigen einen untersuchten und getesteten Adapter. Eine
Mod-Aktualisierung allein macht unbekannte Spielversionen nicht kompatibel.
Unbekannte EXEs werden vor dem Laden der Mod abgewiesen.

Die Lobby nutzt TCP 27121, der Spieltransport TCP 27120. Der Lobbycode enthält
eine erreichbare IPv4-Adresse und einen Raum-Schlüssel. Kein Relay oder
automatisches NAT-Traversal ist enthalten. Reale LAN-/Internet-Sitzungen sind
noch nicht abgenommen.

## Entwicklungsstand

Implementiert und getestet: in-game Solo/Koop-Gate, native Protokoll-/Simulations-
Prototypen, Lobby/Ready/Start, getrennte Profile, Setup und signaturgeprüfte Updates.
Offen: vollständige Zwei-Client-Abnahme, alle Klassen, saubere Gegner-Zielwahl,
Tod/Wiederbelebung, Rejoin und finale Belohnungen. Das Windows-Setup besitzt noch
keine Authenticode-Signatur; die Update-Authentizität wird separat über einen
fest eingebauten öffentlichen RSA-Schlüssel geprüft.

## Bauen

Visual Studio 2022 C++ x64, CMake und .NET Framework 4.8:

    .\Build-Mod.ps1
    ctest --test-dir build-v2 -C RelWithDebInfo --output-on-failure
    .\dist\AzerothSurvivors-Koop-Setup.exe /self-test C:\Temp\AzsModTests /quiet

Die Tests benötigen keine Spiel-Assets. Die optionalen nativen Startup-Tests
benötigen eine eigene unterstützte Spielinstallation. Die generierten Bindings
sind enthalten; für erneutes Auslesen werden Python und pefile benötigt.

Der private Release-Schlüssel ist nicht enthalten. Ein eigener Build besitzt
keine Berechtigung, Updates für die bestehenden Mod-Installationen
zu signieren. Details: [Updater](docs/UPDATER.md).

Die Lizenz des verwendeten MinHook ist in third_party/minhook/LICENSE.txt erhalten.
