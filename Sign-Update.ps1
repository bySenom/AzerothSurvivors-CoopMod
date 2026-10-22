[CmdletBinding()]
param([switch]$Fixture)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Security
$root=$PSScriptRoot
$keyFile=Join-Path $env:LOCALAPPDATA 'AzerothSurvivorsCoopMod-ReleaseKeys\update-signing-key.dpapi'
if(!(Test-Path -LiteralPath $keyFile)){throw 'The maintainer signing key is not available on this Windows account.'}
$encrypted=[IO.File]::ReadAllBytes($keyFile)
$plain=[Security.Cryptography.ProtectedData]::Unprotect($encrypted,$null,[Security.Cryptography.DataProtectionScope]::CurrentUser)
$rsa=New-Object Security.Cryptography.RSACryptoServiceProvider
$rsa.PersistKeyInCsp=$false
try {
    $rsa.FromXmlString([Text.Encoding]::UTF8.GetString($plain))
    $public=[Convert]::ToBase64String($rsa.ExportCspBlob($false))
    $trust=Get-Content -LiteralPath (Join-Path $root 'src\desktop\UpdateTrust.cs') -Raw
    if(!$trust.Contains('"'+$public+'"')){throw 'The signing key does not match the pinned update key.'}
    if($Fixture) {
        $path=Join-Path $root 'tests\fixtures\update-test.json'
        $data=[ordered]@{schema=1;version='0.0.0-test.1';channel='preview';asset='AzerothSurvivors-Koop-Setup.exe';size=4;sha256='9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08'}
    } else {
        $metadata=Get-Content -LiteralPath (Join-Path $root 'mod.json') -Raw | ConvertFrom-Json
        $setup=Get-Item -LiteralPath (Join-Path $root 'dist\AzerothSurvivors-Koop-Setup.exe')
        $path=Join-Path $root 'dist\update.json'
        $supported=@($metadata.compatibleBuilds | ForEach-Object {(Get-Content -LiteralPath (Join-Path $root $_) -Raw | ConvertFrom-Json).executableSha256})
        $data=[ordered]@{schema=1;version=$metadata.version;channel='preview';asset=$setup.Name;size=$setup.Length;sha256=(Get-FileHash -LiteralPath $setup.FullName -Algorithm SHA256).Hash.ToLowerInvariant();supportedGameSha256=$supported}
    }
    [IO.File]::WriteAllText($path,($data | ConvertTo-Json -Depth 4 -Compress),(New-Object Text.UTF8Encoding($false)))
    $bytes=[IO.File]::ReadAllBytes($path)
    $hash=[Security.Cryptography.SHA256]::Create()
    try{$signature=$rsa.SignData($bytes,$hash)}finally{$hash.Dispose()}
    $signatureFile=if($Fixture){[IO.Path]::ChangeExtension($path,'.sig')}else{$path+'.sig'}
    [IO.File]::WriteAllBytes($signatureFile,$signature)
    Write-Output ('Signed '+(Split-Path -Leaf $path)+' for '+$data.version)
} finally {[Array]::Clear($plain,0,$plain.Length);$rsa.Dispose()}
