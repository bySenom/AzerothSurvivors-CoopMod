# Desktop installation

Default user flow: extract the full mod ZIP into the game installation folder →
open Azeroth Survivors Koop.exe beside AzerothSurvivors.exe → Mit Mod spielen →
in-game Solo/Koop. A separate setup wizard is not required.

## Portable ZIP (dev.7+)

dist/AzerothSurvivors-Koop-Mod.zip contains a root-level Koop launcher,
Koop-Mod-Liesmich.txt and AzerothCoopMod. The latter holds an ownership marker,
portable.json and versioned complete runtimes. No game files are included or
overwritten. The launcher uses the adjacent game rather than silently choosing
another saved installation when the adjacent build is unsupported.

Automatic updates use the existing signed setup download as a hidden helper,
with a portable target. It extracts into a new version directory inside
AzerothCoopMod, replaces the root Koop launcher, and switches portable.json.
Previous runtime versions are retained. The helper does not migrate the ZIP
installation into the per-user app folder or create shortcuts. The game folder
must be writable. Profiles/settings stay in the existing per-user data directory.

dev.8 includes both the inspected current Windows adapter and the previous
0.104.0 adapter. The latter's native files live under bin/legacy. The exact game
hash selects the CLI and DLL set together; every listed native file is included
in the runtime manifest and setup/ZIP payload. See NATIVE_ADAPTERS.md.

## Distribution

dist/AzerothSurvivors-Koop-Setup.exe is a single Windows x64 GUI executable,
roughly 4.6 MB with both adapters. It embeds only the runtime: desktop launcher, native mod files,
backend launch script, metadata, compatibility adapter and MinHook license.
No game EXE, MPQs, assets, profiles or machine-specific starter is included.
The package metadata has no hard-coded user path.

The setup and launcher target the Windows .NET Framework 4.8. No SDK, Python,
CMake or Visual Studio is needed by a player. The current test installer is
**not Authenticode-signed**; this is not a signed production release.

## Per-user installation

- App versions: %LOCALAPPDATA%\AzerothSurvivorsCoopMod\app\versions.
- Immutable loaded module cache: app\modules. Development launches without a
  managed installation use a separate module-cache folder.
- Game selection: launcher.json in the data directory.
- Characters: profiles\Main; existing development profiles are not migrated.
- Desktop and Start menu links target the installed launcher EXE.
- Windows Apps uninstall registration is in HKCU; installation requires no
  machine-wide registry writes or administrator elevation.

An ownership marker gates app replacement/removal. Archives cannot extract
outside their staging directory. Native/desktop binaries are checked against
the package manifest. Recursive deletion validates the absolute app boundary
and rejects reparse points before deleting. Profiles/settings are siblings of
the app directory and remain on reinstall/uninstall.

Updates install a new app directory, redirect shortcuts, and retain the previous
app copy. Active game/launcher processes must be closed before uninstall.
The uninstall helper waits for its parent to exit before removing the app.
No game install, network rule or game save is deleted.

## Friendly launcher

The app discovers saved choices, a running game and a bounded set of usual
download locations. It checks exact game/package compatibility before enabling
Play. Missing game, unsupported update, missing assets and damaged package have
German explanations. Selection is persisted; no IP/token configuration appears
in this launcher. That choice stays inside the game.

Starting with dev.7, startup validates the runtime layout before reading the
updater metadata. A bare launcher copied next to the game redirects to the
complete owned per-user installation, after checking its package hashes and
installation target. If no complete installation exists, a recovery window links
to the official mod ZIP releases. Players should extract the entire ZIP
instead of copying the contents of a development bin folder into the game.

The backend runs hidden. Its Windows PowerShell module path is initialized
independently of an inherited PowerShell 7 environment. UTF-8 and Windows argument
quoting cover paths containing spaces and umlauts. Long installed paths use a
shorter shared module-cache location and explicit extended-length native paths.

## Evidence and limits

desktop-validation-result.json records the isolated setup self-test:

- Install and reinstall real embedded runtime; create actual test .lnk files.
- Preserve an existing character sentinel and settings.
- Reject deletion outside the app boundary; uninstall only owned app files.
- Run the installed backend against the supported and updated original builds.
- Start the actual supported native game, initialize the lobby module, and stop
  only that newly launched minimized test child. No game/desktop input is sent.
- Render actual owned offscreen GUI windows for setup, ready, missing-game and
  unsupported-build states; preserve the foreground window.

The captures were visually inspected. desktop-install-result.json records the
real per-user install, desktop/start-menu targets, uninstall registration and
saved compatible game selection on this PC.

The Windows Settings uninstall dialog and a human click on the installed
desktop shortcut were not exercised interactively. These checks do not establish
a rendered two-player run, real LAN/Internet access, or new-game-build support.
The outstanding co-op work listed in STATUS.md remains.

The dev.7 startup regression tests also execute actual relocated launcher
processes without opening interactive windows. They cover complete installs,
bare EXEs with and without an installed fallback, malformed metadata/state,
missing backend files and corrupt package binaries. The recovery window is
rendered offscreen. These probes do not represent a human double-click test.

Portable tests extract the real built ZIP, run the portable startup probe,
reject an unknown adjacent game, and execute the actual hidden update helper
against an isolated fixture. That helper test reinstalls the same version; it
does not claim an older-to-newer public portable upgrade. It checks that game
files, character saves and the previous runtime remain intact.
