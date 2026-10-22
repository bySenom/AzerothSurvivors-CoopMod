# Mod update channel

Public repository: https://github.com/bySenom/AzerothSurvivors-CoopMod

The desktop launcher reads the public release list anonymously over HTTPS.
Preview releases are intentional while the mod is in development. No GitHub
login or access token is needed by players.

Each release must include:

- AzerothSurvivors-Koop-Setup.exe
- AzerothSurvivors-Koop-Setup.exe.sha256
- update.json
- update.json.sig
- AzerothSurvivors-Koop-Mod.zip (default manual installation)
- AzerothSurvivors-Koop-Mod.zip.sha256

The launcher verifies the manifest's RSA-3072/SHA-256 signature against the public
key in src/desktop/UpdateTrust.cs. It requires the tag/version, channel, size and
setup filename to match, and checks the downloaded setup's SHA-256 before it can
run. GitHub's asset digest, when present, must agree. Asset URLs are restricted
to this repository and HTTPS GitHub/CDN hosts. Downloads have time/size bounds.
Older versions cannot be installed through the update path.

Automatic checking/installing is enabled by default and can be disabled in the
launcher. Update failures leave the current app available. A running mod game
defers installation. The helper waits for the old launcher to exit, installs a
new immutable app version, redirects shortcuts, and opens the new launcher.
Profiles, game selection and automatic-update/desktop preferences are preserved.
The old app version remains available on disk for recovery.

For ZIP installs, the same signed download is used as a hidden update helper
with an explicit portable target. It installs a new runtime under the game's
AzerothCoopMod folder and replaces only the root Koop launcher and the mod's
pointer file. Game files and per-user saves/settings remain intact. No setup
wizard or migration to a per-user app installation is involved. The signed
manifest also records the bootstrap ZIP's size and SHA-256. Older setup-based
installations retain their existing update route.

This does not update the game or enable unknown native builds. Windows
Authenticode signing and this mod-specific update signature are separate; the
preview setup is not Authenticode-signed.

## Maintainer publishing

The private key is kept outside the project under the Windows user's local
application data, in AzerothSurvivorsCoopMod-ReleaseKeys/update-signing-key.dpapi.
It is encrypted with Windows DPAPI for that user. It is never copied by the
public export or source backup scripts. The public verification key is committed.
Keep the signing account/key recoverable; loss requires a new manual bootstrap.

1. Bump mod.json, build, and run the native/UI/updater tests.
2. Export the source with Export-Public.ps1 to the separate public checkout.
   This whitelist excludes private Git history, local reports, game files,
   binaries and machine-specific starters.
3. Review/commit/push that checkout.
4. Publish-Release.ps1 -PublicCheckout <checkout> -NotesFile <notes> prepares a
   signed draft release. Add -Publish only for the completed, tested release.

No signing key or GitHub token is shipped in source/runtime packages. The public
repository is a fresh source history; the private working project's local
history and machine paths are not pushed.

## Verification

The fixture is signed for version 0.0.0-test.1, older than real installations,
and represents a harmless four-byte checksum test file. Unit tests reject
changed manifests/signatures, unsigned updates, tag/size/digest mismatches,
foreign or HTTP download sources, corrupt files and downgrades. Setup tests
check profile/settings preservation. A separate live probe records an actual
public GitHub download and installer hand-off between installed mod versions.
