[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PublicCheckout,
      [Parameter(Mandatory=$true)][string]$NotesFile,
      [switch]$Publish)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$metadata=Get-Content -LiteralPath (Join-Path $root 'mod.json') -Raw | ConvertFrom-Json
$repo='bySenom/AzerothSurvivors-CoopMod'
$tag='v'+$metadata.version
$state=@(git -C $PublicCheckout status --porcelain)
if($LASTEXITCODE -ne 0 -or $state.Count){throw 'Commit the audited public source before publishing.'}
$remote=git -C $PublicCheckout remote get-url origin
if($LASTEXITCODE -ne 0 -or $remote -notmatch 'github\.com[:/]bySenom/AzerothSurvivors-CoopMod(?:\.git)?$'){throw 'Unexpected release repository.'}
$revision=git -C $PublicCheckout rev-parse HEAD
$manifest=Get-Content -LiteralPath (Join-Path $root 'package-manifest.json') -Raw | ConvertFrom-Json
if($manifest.modVersion -ne $metadata.version){throw 'Build the release before publishing.'}
foreach($entry in $manifest.files){
    if((Get-FileHash -LiteralPath (Join-Path $root ('bin\'+$entry.name)) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256){throw ('Package differs: '+$entry.name)}
}
& (Join-Path $root 'Sign-Update.ps1')
$assets=@('AzerothSurvivors-Koop-Setup.exe','AzerothSurvivors-Koop-Setup.exe.sha256','update.json','update.json.sig') | ForEach-Object {Join-Path $root ('dist\'+$_)}
& gh release create $tag @assets --repo $repo --target $revision --title ('Koop-Mod '+$metadata.version+' · Preview') --notes-file $NotesFile --prerelease --draft
if($LASTEXITCODE -ne 0){throw 'Creating the draft release failed.'}
if($Publish){
    & gh release edit $tag --repo $repo --draft=false
    if($LASTEXITCODE -ne 0){throw 'Publishing the release failed.'}
}
