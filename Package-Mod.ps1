[CmdletBinding()]
param([string]$OutputDirectory=(Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'AzerothSurvivors-CoopMod-Backups'))
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$metadata=Get-Content -LiteralPath (Join-Path $root 'mod.json') -Raw | ConvertFrom-Json
$manifest=Get-Content -LiteralPath (Join-Path $root 'package-manifest.json') -Raw | ConvertFrom-Json
if($manifest.modVersion -ne $metadata.version){throw 'Rebuild the mod before packaging this version.'}
foreach($entry in $manifest.files){
    if((Get-FileHash -LiteralPath (Join-Path $root ('bin\'+$entry.name)) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256){throw ('Rebuild the mod before packaging: '+$entry.name)}
}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$name='AzerothSurvivors-CoopMod-'+$metadata.version+'-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ')+'.zip'
$destination=Join-Path $OutputDirectory $name
$files=@(Get-ChildItem -LiteralPath $root -File -Force | Where-Object {$_.Extension -in '.ps1','.cmd','.json','.md' -or $_.Name -in '.gitignore','.gitattributes','CMakeLists.txt'})
foreach($directory in @('src','scripts','tests','third_party','compat','docs')){
    $files+=@(Get-ChildItem -LiteralPath (Join-Path $root $directory) -File -Recurse -Force | Where-Object {$_.FullName -notmatch '[\\/](__pycache__|\.git)[\\/]'})
}
$files+=@($metadata.nativeFiles | ForEach-Object {Get-Item -LiteralPath (Join-Path $root ('bin\'+$_))})
$files+=@($metadata.desktopFiles | ForEach-Object {Get-Item -LiteralPath (Join-Path $root ('bin\'+$_))})
$files+=@(Get-Item -LiteralPath (Join-Path $root 'dist\AzerothSurvivors-Koop-Setup.exe'))
$files+=@(Get-Item -LiteralPath (Join-Path $root 'dist\AzerothSurvivors-Koop-Setup.exe.sha256'))
$files+=@(Get-Item -LiteralPath (Join-Path $root 'dist\AzerothSurvivors-Koop-Mod.zip'))
$files+=@(Get-Item -LiteralPath (Join-Path $root 'dist\AzerothSurvivors-Koop-Mod.zip.sha256'))
$files+=@(Get-ChildItem -LiteralPath (Join-Path $root 'dist') -Filter 'update.json*' -File)
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive=[IO.Compression.ZipFile]::Open($destination,[IO.Compression.ZipArchiveMode]::Create)
try{
    foreach($file in $files){
        $relative=$file.FullName.Substring($root.Length+1).Replace('\','/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$file.FullName,$relative,[IO.Compression.CompressionLevel]::Optimal)
    }
}finally{$archive.Dispose()}
$digest=(Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
($digest+'  '+$name) | Set-Content -LiteralPath ($destination+'.sha256') -Encoding ASCII
[pscustomobject]@{archive=$destination;sha256=$digest;entries=$files.Count;includesSource=$true;includesGameAssets=$false} | ConvertTo-Json
