[CmdletBinding()]
param([switch]$LauncherOnly,[switch]$SetupOnly)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$metadata=Get-Content -LiteralPath (Join-Path $root 'mod.json') -Raw | ConvertFrom-Json
$framework=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
$compiler=Join-Path $framework 'csc.exe'
if(!(Test-Path -LiteralPath $compiler)){throw 'The Windows .NET Framework C# compiler is required.'}
$build=Join-Path $root 'build-v2\desktop'
New-Item -ItemType Directory -Path $build,(Join-Path $root 'dist') -Force | Out-Null
$icon=Join-Path $build 'mod.ico'
if(!(Test-Path -LiteralPath $icon)){
    Add-Type -AssemblyName System.Drawing
    Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class AzsBuildIcon { [DllImport("user32.dll")] public static extern bool DestroyIcon(IntPtr h); }'
    $bitmap=New-Object Drawing.Bitmap(64,64)
    $graphics=[Drawing.Graphics]::FromImage($bitmap)
    $gold=New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(244,201,113))
    $background=New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(56,23,18))
    $font=New-Object Drawing.Font('Segoe UI',34,[Drawing.FontStyle]::Bold,[Drawing.GraphicsUnit]::Pixel)
    $format=New-Object Drawing.StringFormat
    $format.Alignment='Center';$format.LineAlignment='Center'
    try {
        $graphics.Clear([Drawing.Color]::FromArgb(24,20,18))
        $graphics.FillEllipse($background,2,2,60,60)
        $graphics.DrawString('K',$font,$gold,(New-Object Drawing.RectangleF(0,0,64,64)),$format)
        $handle=$bitmap.GetHicon()
        try {
            $native=[Drawing.Icon]::FromHandle($handle)
            $stream=[IO.File]::Create($icon)
            try{$native.Save($stream)}finally{$stream.Dispose()}
        } finally {[void][AzsBuildIcon]::DestroyIcon($handle)}
    } finally {$format.Dispose();$font.Dispose();$background.Dispose();$gold.Dispose();$graphics.Dispose();$bitmap.Dispose()}
}
$arguments=@('/nologo','/target:winexe','/platform:x64','/optimize+','/codepage:65001',('/win32icon:'+$icon))
foreach($reference in @('System.dll','System.Core.dll','System.Drawing.dll','System.Windows.Forms.dll','System.Web.Extensions.dll','System.IO.Compression.dll','System.IO.Compression.FileSystem.dll','Microsoft.CSharp.dll')){
    $arguments+=('/reference:'+(Join-Path $framework $reference))
}
$sources=@(Get-ChildItem -LiteralPath (Join-Path $root 'src\desktop') -Filter '*.cs' -File | ForEach-Object {$_.FullName})
$versionFile=Join-Path $build 'AssemblyVersion.cs'
$parts=$metadata.version -split '[-.]'
$revision=if($parts.Count -gt 3 -and $parts[-1] -match '^\d+$'){[int]$parts[-1]}else{0}
$assemblyVersion=($parts[0..2] -join '.')+'.'+$revision
[IO.File]::WriteAllText($versionFile,('[assembly: System.Reflection.AssemblyVersion("'+$assemblyVersion+'")]'+[Environment]::NewLine+'[assembly: System.Reflection.AssemblyFileVersion("'+$assemblyVersion+'")]'),(New-Object Text.UTF8Encoding($false)))
$sources+=$versionFile
foreach($fixture in @('json','sig')){
    $arguments+=('/resource:'+(Join-Path $root ('tests\fixtures\update-test.'+$fixture))+',AzerothCoop.UpdateTest.'+$fixture)
}
if(!$SetupOnly){
    $launcher=Join-Path $root 'bin\Azeroth Survivors Koop.exe'
    $staged=Join-Path $build ('launcher-'+[Guid]::NewGuid().ToString('N')+'.exe')
    & $compiler @arguments ('/out:'+$staged) @sources
    if($LASTEXITCODE -ne 0){throw 'Desktop launcher compilation failed'}
    if(Test-Path -LiteralPath $launcher){
        $retired=Join-Path $root 'bin\loaded'
        New-Item -ItemType Directory -Path $retired -Force | Out-Null
        Move-Item -LiteralPath $launcher -Destination (Join-Path $retired ('retired-'+[Guid]::NewGuid().ToString('N')+'-desktop.exe'))
    }
    Move-Item -LiteralPath $staged -Destination $launcher
}
if($LauncherOnly){return}
$metadata=Get-Content -LiteralPath (Join-Path $root 'mod.json') -Raw | ConvertFrom-Json
$manifest=Get-Content -LiteralPath (Join-Path $root 'package-manifest.json') -Raw | ConvertFrom-Json
if($metadata.version -ne $manifest.modVersion){throw 'Build the native package and launcher before creating the setup.'}
foreach($entry in $manifest.files){
    if((Get-FileHash -LiteralPath (Join-Path $root ('bin\'+$entry.name)) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256){throw ('Package hash mismatch: '+$entry.name)}
}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$payload=Join-Path $build ('runtime-'+[Guid]::NewGuid().ToString('N')+'.zip')
$runtime=@('Launch-CoopMod.ps1','mod.json','package-manifest.json')+
    @($metadata.compatibleBuilds)+@($metadata.nativeFiles | ForEach-Object {'bin/'+$_})+
    @($metadata.desktopFiles | ForEach-Object {'bin/'+$_})
$archive=[IO.Compression.ZipFile]::Open($payload,[IO.Compression.ZipArchiveMode]::Create)
try{
    foreach($relative in $runtime){
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,(Join-Path $root $relative),$relative.Replace('\','/'),[IO.Compression.CompressionLevel]::Optimal)
    }
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,(Join-Path $root 'third_party\minhook\LICENSE.txt'),'licenses/MinHook.txt',[IO.Compression.CompressionLevel]::Optimal)
}finally{$archive.Dispose()}
$setup=Join-Path $root 'dist\AzerothSurvivors-Koop-Setup.exe'
$stagedSetup=Join-Path $build ('setup-'+[Guid]::NewGuid().ToString('N')+'.exe')
& $compiler @arguments ('/out:'+$stagedSetup) ('/resource:'+$payload+',AzerothCoop.Runtime.zip') @sources
if($LASTEXITCODE -ne 0){throw 'Setup compilation failed'}
Copy-Item -LiteralPath $stagedSetup -Destination $setup -Force
$digest=(Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant()
($digest+'  '+(Split-Path -Leaf $setup)) | Set-Content -LiteralPath ($setup+'.sha256') -Encoding ASCII
Write-Output ('Built consumer setup: '+$setup)
$portableStage=Join-Path $build ('portable-'+[Guid]::NewGuid().ToString('N'))
$portableRoot=Join-Path $portableStage 'AzerothCoopMod'
$portableRuntime=Join-Path $portableRoot ('versions\'+$metadata.version)
New-Item -ItemType Directory -Path $portableRoot -Force | Out-Null
[IO.Compression.ZipFile]::ExtractToDirectory($payload,$portableRuntime)
Copy-Item -LiteralPath (Join-Path $root 'bin\Azeroth Survivors Koop.exe') -Destination (Join-Path $portableStage 'Azeroth Survivors Koop.exe')
[IO.File]::WriteAllText((Join-Path $portableRoot '.azs-portable-owned'),'AzerothSurvivorsCoopMod.Portable.v1',(New-Object Text.UTF8Encoding($false)))
$portableState=[ordered]@{schema=1;version=$metadata.version;runtime=('versions/'+$metadata.version)}
[IO.File]::WriteAllText((Join-Path $portableRoot 'portable.json'),($portableState | ConvertTo-Json -Compress),(New-Object Text.UTF8Encoding($false)))
@'
Azeroth Survivors Koop-Mod

1. Den gesamten Inhalt dieses ZIPs direkt in den Spielordner entpacken.
   Azeroth Survivors Koop.exe und der Ordner AzerothCoopMod muessen neben
   AzerothSurvivors.exe und dem vorhandenen Data-Ordner liegen.
2. Azeroth Survivors Koop.exe starten, dann Mit Mod spielen.
3. Im Spiel Wald von Elwynn betreten -> Solo / Koop.

Kein separates Setup und kein Kopieren einzelner DLLs noetig.
Mod-Updates werden automatisch geprueft und im selben Spielordner installiert.
Der Ordner muss fuer dein Windows-Konto beschreibbar sein.
Mod-Spielstaende und Einstellungen bleiben unter
%LOCALAPPDATA%\AzerothSurvivorsCoopMod erhalten.

Dies ist eine unfertige Preview. Unterstuetzte Spielbuilds:
Windows EXE SHA-256 dc10453398ceb398076f6d3b6ac7859470de2ff7ccd68ac1d00a02f9f1e5209a
und 0.104.0 / 9845e5c7. Weitere EXEs benoetigen eine gesonderte Mod-Anpassung.
https://github.com/bySenom/AzerothSurvivors-CoopMod
'@ | Set-Content -LiteralPath (Join-Path $portableStage 'Koop-Mod-Liesmich.txt') -Encoding UTF8
$portableArchive=Join-Path $root 'dist\AzerothSurvivors-Koop-Mod.zip'
$stagedArchive=Join-Path $build ('portable-release-'+[Guid]::NewGuid().ToString('N')+'.zip')
[IO.Compression.ZipFile]::CreateFromDirectory($portableStage,$stagedArchive,[IO.Compression.CompressionLevel]::Optimal,$false)
Copy-Item -LiteralPath $stagedArchive -Destination $portableArchive -Force
((Get-FileHash -LiteralPath $portableArchive -Algorithm SHA256).Hash.ToLowerInvariant()+'  '+(Split-Path -Leaf $portableArchive)) |
    Set-Content -LiteralPath ($portableArchive+'.sha256') -Encoding ASCII
Write-Output ('Built portable mod ZIP: '+$portableArchive)
