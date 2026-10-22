[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$Destination=[IO.Path]::GetFullPath($Destination)
if($Destination -eq $root -or ($Destination+'\').StartsWith($root.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){
    throw 'Use a separate public checkout, outside the private working project.'
}
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
$files=@('.gitignore','.gitattributes','Build-Mod.cmd','Build-Mod.ps1','Build-Desktop.ps1','Launch-CoopMod.ps1','Start-CoopMod.cmd',
    'Package-Mod.ps1','CMakeLists.txt','mod.json','Sign-Update.ps1','Export-Public.ps1','Publish-Release.ps1')
foreach($directory in @('src','scripts','tests','third_party\minhook','compat')){
    $files+=@(Get-ChildItem -LiteralPath (Join-Path $root $directory) -Recurse -File |
        Where-Object {$_.FullName -notmatch '[\\/](__pycache__|\.git)[\\/]'} |
        ForEach-Object {$_.FullName.Substring($root.Length+1)})
}
$files+=@('docs\UPDATER.md','docs\CONSUMER_SETUP.md','docs\PUBLIC_README.md')
foreach($relative in $files){
    $source=Join-Path $root $relative
    $target=Join-Path $Destination $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $target -Force
}
Copy-Item -LiteralPath (Join-Path $root 'docs\PUBLIC_README.md') -Destination (Join-Path $Destination 'README.md') -Force
@'
/bin/
/dist/
/game/
/build*/
/runtime/
/package-manifest.json
__pycache__/
*.dpapi
*.pfx
*.p12
*-private-key*
'@ | Set-Content -LiteralPath (Join-Path $Destination '.gitignore') -Encoding ASCII
foreach($relative in @($files+@('README.md') | Select-Object -Unique)){
    $file=Get-Item -LiteralPath (Join-Path $Destination $relative)
    if($file.Extension -in '.mpq','.exe','.dll','.pdb','.dpapi','.pfx','.p12'){throw ('Forbidden public source file: '+$file.Name)}
    if($file.Extension -in '.cs','.cpp','.hpp','.ps1','.py','.md','.json','.cmd','.txt'){
        $content=[IO.File]::ReadAllText($file.FullName)
        if($content -match '(?i)[a-z]:[\\/]+Users[\\/]+|gh[pousr]_[A-Za-z0-9]{20,}|<D>[A-Za-z0-9+/=]{40,}</D>'){
            throw ('Personal path or secret pattern in public export: '+$file.Name)
        }
    }
}
Write-Output ('Public source export prepared: '+$Destination)
