[CmdletBinding()]
param(
    [ValidateSet('Play','Menu','Host','Join','Check')][string]$Mode='Play',
    [string]$GamePath,
    [ValidatePattern('^[A-Za-z0-9_-]{1,40}$')][string]$Profile='Main',
    [string]$Address='127.0.0.1',
    [ValidateRange(1024,65535)][int]$Port=27120,
    [string]$RoomToken,
    [string]$DataRoot=(Join-Path $env:LOCALAPPDATA 'AzerothSurvivorsCoopMod'),
    [switch]$Minimized,
    [switch]$NoErrorDialog
)
$ErrorActionPreference='Stop'
$OutputEncoding=New-Object Text.UTF8Encoding($false)
[Console]::OutputEncoding=$OutputEncoding
$root=$PSScriptRoot
$metadata=Get-Content -LiteralPath (Join-Path $root 'mod.json') -Raw | ConvertFrom-Json
$DataRoot=[IO.Path]::GetFullPath($DataRoot)
$settingsFile=Join-Path $DataRoot 'settings.json'
$saved=$null
if(Test-Path -LiteralPath $settingsFile){
    try{$saved=Get-Content -LiteralPath $settingsFile -Raw | ConvertFrom-Json}catch{$saved=$null}
}
if(!$GamePath){$GamePath=if($saved -and $saved.gamePath){$saved.gamePath}else{$metadata.defaultGamePath}}

function Get-Compatibility([string]$Path) {
    $state=[ordered]@{mod=$metadata.name;modVersion=$metadata.version;compatible=$false;reason='game_not_found';gamePath=$Path;dataRoot=$DataRoot;profilePath=(Join-Path $DataRoot "profiles\$Profile")}
    if([string]::IsNullOrWhiteSpace($Path) -or !(Test-Path -LiteralPath $Path -PathType Leaf)){return [pscustomobject]$state}
    $state.gamePath=(Resolve-Path -LiteralPath $Path).Path
    $state.gameSha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    $state.reason='unsupported_game_build'
    foreach($file in $metadata.compatibleBuilds) {
        $adapter=Get-Content -LiteralPath (Join-Path $root $file) -Raw | ConvertFrom-Json
        if($adapter.executableSha256 -eq $state.gameSha256){$state.compatible=$true;$state.reason='supported';$state.gameVersion=$adapter.gameVersion;$state.nativeDirectory=[string]$adapter.nativeDirectory;break}
    }
    if(!$state.compatible){return [pscustomobject]$state}
    if(!(Test-Path -LiteralPath (Join-Path (Split-Path -Parent $state.gamePath) 'Data') -PathType Container)){$state.compatible=$false;$state.reason='game_data_missing';return [pscustomobject]$state}
    $manifestFile=Join-Path $root 'package-manifest.json'
    if(!(Test-Path -LiteralPath $manifestFile)){$state.compatible=$false;$state.reason='build_mod_first';return [pscustomobject]$state}
    $manifest=Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
    if($manifest.modVersion -ne $metadata.version){$state.compatible=$false;$state.reason='native_package_version_mismatch';return [pscustomobject]$state}
    foreach($name in $metadata.nativeFiles) {
        $native=Join-Path $root "bin\$name"
        $entry=@($manifest.files | Where-Object {$_.name -eq $name})
        if(!(Test-Path -LiteralPath $native) -or $entry.Count -ne 1){$state.compatible=$false;$state.reason='native_package_incomplete';break}
        if((Get-FileHash -LiteralPath $native -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry[0].sha256){$state.compatible=$false;$state.reason='native_package_version_mismatch';break}
    }
    return [pscustomobject]$state
}
function New-RoomToken {
    $bytes=New-Object byte[] 16
    $random=[Security.Cryptography.RandomNumberGenerator]::Create()
    try{$random.GetBytes($bytes)}finally{$random.Dispose()}
    return -join ($bytes | ForEach-Object {$_.ToString('x2')})
}
function Invoke-Native([string[]]$Arguments,[string]$NativeDirectory='') {
    $start=New-Object Diagnostics.ProcessStartInfo
    $nativeBin=Join-Path $root 'bin'
    if($NativeDirectory){$nativeBin=Join-Path $nativeBin $NativeDirectory}
    $start.FileName=Join-Path $nativeBin 'AzerothCoop.exe'
    # Canonical file paths cannot contain quotes; values ending in a slash are
    # normalized before this call. Validate all non-path values below.
    $start.Arguments=($Arguments | ForEach-Object {'"'+$_+'"'}) -join ' '
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $managedApp=Join-Path $DataRoot 'app'
    $marker=Join-Path $managedApp '.azs-mod-owned'
    $managed=(Test-Path -LiteralPath $marker) -and ((Get-Content -LiteralPath $marker -Raw) -eq 'AzerothSurvivorsCoopMod.App.v1')
    $start.EnvironmentVariables['AZS_COOP_MODULE_CACHE']=if($managed){Join-Path $managedApp 'modules'}else{Join-Path $DataRoot 'module-cache'}
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $process=[Diagnostics.Process]::Start($start)
    $output=$process.StandardOutput.ReadToEnd();$errorText=$process.StandardError.ReadToEnd();$process.WaitForExit()
    if($process.ExitCode -ne 0){throw "Mod launcher failed: $errorText $output"}
    $rows=@($output -split '[\r\n]+' | Where-Object {$_.Trim()} | ForEach-Object {$_ | ConvertFrom-Json})
    foreach($row in $rows){if($row.error){throw $row.error};if($row.failure){throw $row.failure}}
    return $rows
}
function Start-Mod([string]$LaunchMode,[string]$Path,[string]$ProfileName,[string]$HostAddress,[int]$HostPort,[string]$Token) {
    if($ProfileName -notmatch '^[A-Za-z0-9_-]{1,40}$'){throw 'Use a profile name containing letters, numbers, underscores or hyphens.'}
    if($Token -notmatch '^[A-Fa-f0-9]{32}$'){throw 'The room key must contain 32 hexadecimal characters.'}
    if($HostAddress -notmatch '^[A-Za-z0-9.:-]{1,253}$'){throw 'Enter a host name or IP address.'}
    if($HostPort -lt 1024 -or $HostPort -gt 65535){throw 'Use a port between 1024 and 65535.'}
    $compatibility=Get-Compatibility $Path
    if(!$compatibility.compatible){throw "Compatibility check: $($compatibility.reason). Your mod files and characters are preserved. A changed game build needs an updated compatibility adapter."}
    $nativeDirectory=[string]$compatibility.nativeDirectory
    $nativeBin=Join-Path $root 'bin'
    if($nativeDirectory){$nativeBin=Join-Path $nativeBin $nativeDirectory}
    $profilePath=Join-Path $DataRoot "profiles\$ProfileName"
    $gameDirectory=(Split-Path -Parent $compatibility.gamePath).TrimEnd('\')+'\'
    if(($DataRoot.TrimEnd('\')+'\').StartsWith($gameDirectory,[StringComparison]::OrdinalIgnoreCase)){throw 'Keep mod profiles outside the game directory.'}
    $instanceFile=Join-Path $profilePath 'instance.json'
    if(Test-Path -LiteralPath $instanceFile){
        $previous=Get-Content -LiteralPath $instanceFile -Raw | ConvertFrom-Json
        $running=Get-Process -Id $previous.pid -ErrorAction SilentlyContinue
        if($running -and $running.Path -eq $compatibility.gamePath){throw 'This mod profile is already running. Close that game window or choose a different profile.'}
    }
    New-Item -ItemType Directory -Path $profilePath,(Join-Path $DataRoot 'logs') -Force | Out-Null
    $save=Join-Path $profilePath 'characters.txt'
    if(Test-Path -LiteralPath $save){
        $backup=Join-Path $DataRoot ('backups\'+$ProfileName+'\'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ'))
        New-Item -ItemType Directory -Path $backup -Force | Out-Null
        Copy-Item -LiteralPath $save -Destination (Join-Path $backup 'characters.txt')
    }
    @{gamePath=$compatibility.gamePath;profile=$ProfileName;address=$HostAddress;port=$HostPort;roomToken=$Token} | ConvertTo-Json | Set-Content -LiteralPath $settingsFile -Encoding UTF8
    if($LaunchMode -eq 'Play') {
        $command=if($Minimized){'play-probe'}else{'play'}
        $nativeArguments=@($command,$compatibility.gamePath,$profilePath,(Join-Path $nativeBin 'azs_coop.dll'),
            (Join-Path $nativeBin 'azs_transport.dll'),(Join-Path $nativeBin 'azs_gameplay.dll'),(Join-Path $nativeBin 'azs_lobby.dll'))
    } else {
        $command=if($LaunchMode -eq 'Host'){if($Minimized){'host-probe'}else{'host'}}else{if($Minimized){'join-probe'}else{'join'}}
        $nativeArguments=@($command,$compatibility.gamePath,$profilePath,(Join-Path $nativeBin 'azs_coop.dll'),
            (Join-Path $nativeBin 'azs_transport.dll'),$HostAddress,[string]$HostPort,$Token)
    }
    $rows=Invoke-Native -Arguments $nativeArguments -NativeDirectory $nativeDirectory
    $instance=@($rows | Where-Object {$_.phase -eq 'native-integration-probe'})[-1]
    if(!$instance.pid){throw 'The native game instance did not report its process ID.'}
    try {
        if($LaunchMode -eq 'Host') {
            $attach=@(Invoke-Native -Arguments @('attach-gameplay',[string]$instance.pid,(Join-Path $nativeBin 'azs_gameplay.dll')) -NativeDirectory $nativeDirectory)
            if(!$attach[-1].hooks_installed){throw 'The gameplay module did not initialize.'}
            foreach($setting in @('contexts','shared','replication')){$null=Invoke-Native -Arguments @('gameplay',[string]$instance.pid,$setting,'on') -NativeDirectory $nativeDirectory}
        }
    } catch {
        # Cleanup is restricted to the child reported by this launch operation.
        $child=Get-Process -Id $instance.pid -ErrorAction SilentlyContinue
        if($child -and $child.Path -eq $compatibility.gamePath){Stop-Process -Id $child.Id -Force}
        throw
    }
    $result=[ordered]@{modVersion=$metadata.version;mode=$LaunchMode;pid=$instance.pid;gamePath=$compatibility.gamePath;profilePath=$profilePath;inGameLobby=($LaunchMode -eq 'Play');address=$HostAddress;port=$HostPort;completeCoopVerified=$false}
    $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $DataRoot ('logs\last-'+$LaunchMode.ToLowerInvariant()+'.json')) -Encoding UTF8
    return [pscustomobject]$result
}

if($Mode -eq 'Check'){
    $check=Get-Compatibility $GamePath;$check | ConvertTo-Json -Depth 4
    if(!$check.compatible){exit 3};exit 0
}
if($Mode -ne 'Menu'){
    if(!$RoomToken){$RoomToken=if($saved -and $saved.roomToken){$saved.roomToken}else{New-RoomToken}}
    if($Mode -eq 'Host' -and !$PSBoundParameters.ContainsKey('Address')){$Address='0.0.0.0'}
    try { Start-Mod $Mode $GamePath $Profile $Address $Port $RoomToken | ConvertTo-Json }
    catch {
        if(!$Minimized -and !$NoErrorDialog) {
            Add-Type -AssemblyName System.Windows.Forms
            [void][Windows.Forms.MessageBox]::Show($_.Exception.Message,'Azeroth Survivors Co-op Mod','OK','Information')
        }
        throw
    }
    exit 0
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$form=New-Object Windows.Forms.Form
$form.Text='Azeroth Survivors Co-op Mod';$form.ClientSize=New-Object Drawing.Size(680,440)
$form.StartPosition='CenterScreen';$form.FormBorderStyle='FixedDialog';$form.MaximizeBox=$false
function Add-Label([string]$Text,[int]$X,[int]$Y,[int]$Width=640){$c=New-Object Windows.Forms.Label;$c.Text=$Text;$c.Location=New-Object Drawing.Point($X,$Y);$c.Size=New-Object Drawing.Size($Width,24);$form.Controls.Add($c);return $c}
function Add-Text([string]$Text,[int]$X,[int]$Y,[int]$Width){$c=New-Object Windows.Forms.TextBox;$c.Text=$Text;$c.Location=New-Object Drawing.Point($X,$Y);$c.Size=New-Object Drawing.Size($Width,25);$form.Controls.Add($c);return $c}
$null=Add-Label 'CO-OP MOD - DEVELOPMENT BUILD' 18 16
$null=Add-Label 'Separate mod files and persistent characters. The original game is loaded without file patches.' 18 44
$null=Add-Label 'Game executable' 18 80
$gameBox=Add-Text $GamePath 18 105 550
$browse=New-Object Windows.Forms.Button;$browse.Text='Browse...';$browse.Location=New-Object Drawing.Point(577,103);$browse.Size=New-Object Drawing.Size(85,27);$form.Controls.Add($browse)
$browse.Add_Click({$dialog=New-Object Windows.Forms.OpenFileDialog;$dialog.Filter='Azeroth Survivors|AzerothSurvivors.exe|Executable|*.exe';if($dialog.ShowDialog() -eq 'OK'){$gameBox.Text=$dialog.FileName};$dialog.Dispose()})
$null=Add-Label 'Profile' 18 145 130;$profileBox=Add-Text $(if($saved -and $saved.profile){$saved.profile}else{'Main'}) 18 170 140
$null=Add-Label 'Host IP / name (Join)' 180 145 270;$hostBox=Add-Text $(if($saved -and $saved.address -ne '0.0.0.0'){$saved.address}else{'127.0.0.1'}) 180 170 300
$null=Add-Label 'Port' 504 145 100;$portBox=Add-Text $(if($saved -and $saved.port){[string]$saved.port}else{'27120'}) 504 170 100
$null=Add-Label 'Room key - share this with your co-op partner' 18 212
$tokenBox=Add-Text $(if($saved -and $saved.roomToken){$saved.roomToken}else{New-RoomToken}) 18 238 450
$copy=New-Object Windows.Forms.Button;$copy.Text='Copy key';$copy.Location=New-Object Drawing.Point(486,236);$copy.Size=New-Object Drawing.Size(118,27);$form.Controls.Add($copy);$copy.Add_Click({[Windows.Forms.Clipboard]::SetText($tokenBox.Text)})
$status=Add-Label 'Native combat and progression prototype. Full gameplay and LAN testing remain.' 18 286
$hostButton=New-Object Windows.Forms.Button;$hostButton.Text='Host co-op';$hostButton.Location=New-Object Drawing.Point(18,326);$hostButton.Size=New-Object Drawing.Size(180,40);$form.Controls.Add($hostButton)
$joinButton=New-Object Windows.Forms.Button;$joinButton.Text='Join co-op';$joinButton.Location=New-Object Drawing.Point(214,326);$joinButton.Size=New-Object Drawing.Size(180,40);$form.Controls.Add($joinButton)
$folderButton=New-Object Windows.Forms.Button;$folderButton.Text='Open profiles';$folderButton.Location=New-Object Drawing.Point(410,326);$folderButton.Size=New-Object Drawing.Size(194,40);$form.Controls.Add($folderButton)
$folderButton.Add_Click({New-Item -ItemType Directory -Path (Join-Path $DataRoot 'profiles') -Force | Out-Null;Start-Process explorer.exe -ArgumentList ('"'+(Join-Path $DataRoot 'profiles')+'"') -WindowStyle Hidden})
$null=Add-Label ('Mod '+$metadata.version+' | Character data: '+$DataRoot) 18 393
function Launch-FromForm([string]$SelectedMode) {
    try {
        $status.Text='Checking compatibility and starting the mod...';$hostButton.Enabled=$false;$joinButton.Enabled=$false;[Windows.Forms.Application]::DoEvents()
        $target=if($SelectedMode -eq 'Host'){'0.0.0.0'}else{$hostBox.Text.Trim()}
        $instance=Start-Mod $SelectedMode $gameBox.Text.Trim() $profileBox.Text.Trim() $target ([int]$portBox.Text) $tokenBox.Text.Trim()
        $status.Text="$SelectedMode started (PID $($instance.pid)). Development build; use the game window."
    }catch{$status.Text='Launch stopped. Your profiles are preserved.';[Windows.Forms.MessageBox]::Show($_.Exception.Message,'Co-op Mod','OK','Information') | Out-Null}
    finally{$hostButton.Enabled=$true;$joinButton.Enabled=$true}
}
$hostButton.Add_Click({Launch-FromForm 'Host'});$joinButton.Add_Click({Launch-FromForm 'Join'})
$null=$form.ShowDialog();$form.Dispose()
