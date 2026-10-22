param([string]$GamePath,[ValidateSet('RelWithDebInfo','Release')][string]$Configuration='RelWithDebInfo')
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
if($GamePath) {
    $game=(Resolve-Path -LiteralPath $GamePath).Path
    foreach($script in @('generate_transport_contract.py','generate_gameplay_contract.py','generate_lobby_contract.py')) {
        & python (Join-Path $root "scripts\$script") --game $game
        if($LASTEXITCODE -ne 0){throw "Native build contract generation failed: $script"}
    }
}
& cmake -S $root -B (Join-Path $root 'build-v2') -G 'Visual Studio 17 2022' -A x64
if($LASTEXITCODE -ne 0){throw 'CMake configuration failed'}
& cmake --build (Join-Path $root 'build-v2') --config $Configuration --parallel 4
if($LASTEXITCODE -ne 0){throw 'Native mod build failed'}
$metadata=Get-Content -LiteralPath (Join-Path $root 'mod.json') -Raw | ConvertFrom-Json
$destination=Join-Path $root 'bin'
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$files=@()
foreach($name in $metadata.nativeFiles) {
    $built=Join-Path $root "build-v2\bin\$Configuration\$name"
    $target=Join-Path $destination $name
    $digest=(Get-FileHash -LiteralPath $built -Algorithm SHA256).Hash.ToLowerInvariant()
    $current=if(Test-Path -LiteralPath $target){(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()}else{''}
    if($current -ne $digest) {
        # Windows cannot overwrite an image that a running process has mapped.
        # Preserve that image under a different name and publish a new file.
        # The running game keeps its original mapping; new launches use the build.
        $staged=Join-Path $destination ('.stage-'+[Guid]::NewGuid().ToString('N')+'-'+$name)
        Copy-Item -LiteralPath $built -Destination $staged
        try {
            if(Test-Path -LiteralPath $target) {
                $retired=Join-Path $destination 'loaded'
                New-Item -ItemType Directory -Path $retired -Force | Out-Null
                Move-Item -LiteralPath $target -Destination (Join-Path $retired ('retired-'+[Guid]::NewGuid().ToString('N')+'-'+$name))
            }
            Move-Item -LiteralPath $staged -Destination $target
        } finally {
            if(Test-Path -LiteralPath $staged){Remove-Item -LiteralPath $staged}
        }
    }
    $files+=@{name=$name;sha256=$digest}
}
& (Join-Path $root 'Build-Desktop.ps1') -LauncherOnly
foreach($name in $metadata.desktopFiles){
    $files+=@{name=$name;sha256=(Get-FileHash -LiteralPath (Join-Path $destination $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
}
@{schema=1;modVersion=$metadata.version;files=$files} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'package-manifest.json') -Encoding UTF8
& (Join-Path $root 'Build-Desktop.ps1') -SetupOnly
Write-Output "Built $($metadata.name) $($metadata.version) in $destination"
