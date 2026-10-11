param([string]$GameDirectory='')
$ErrorActionPreference='Stop'
if (!$GameDirectory) {
    # Packaged next to the game executable; source checkout can use Steam discovery.
    if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'PlanetCoaster2.exe') -PathType Leaf) {
        $GameDirectory=$PSScriptRoot
    } elseif ($env:PC2VR_GAME_DIR) {
        $GameDirectory=$env:PC2VR_GAME_DIR
    } else {
        $finder=Join-Path (Split-Path $PSScriptRoot -Parent) 'gamedir.ps1'
        if (!(Test-Path -LiteralPath $finder -PathType Leaf)) {
            throw 'Place launch.ps1 next to PlanetCoaster2.exe, or supply -GameDirectory.'
        }
        . $finder
        $GameDirectory=$GameDir
    }
}
$game=(Resolve-Path -LiteralPath $GameDirectory).Path
if(Get-Process PlanetCoaster2 -ErrorAction SilentlyContinue){throw 'The game is already running.'}
foreach($file in 'version.dll','dxgi.dll','CheekyFoveatedDLSS\CheekyFoveatedDLSSHost.dll','CheekyFoveatedDLSS\CheekyFoveatedDLSSRuntime.dll','PC2VR\CheekyOpenXR\CheekyOpenXRLayer.dll','PC2VR\CheekyOpenXR\XR_APILAYER_CHEEKY_foveated_dlss.json'){
    if(!(Test-Path -LiteralPath (Join-Path $game $file) -PathType Leaf)){throw "Missing prototype file: $file"}
}
$start=[Diagnostics.ProcessStartInfo]::new((Join-Path $game 'PlanetCoaster2.exe'))
$start.WorkingDirectory=$game
$start.UseShellExecute=$false
$start.CreateNoWindow=$true
$start.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
$start.Environment['SteamAppId']='2688950'
$layer=Join-Path $game 'PC2VR\CheekyOpenXR'
$directories=@($start.Environment['XR_API_LAYER_PATH'] -split ';' | Where-Object {$_})
if($directories -notcontains $layer){$directories+=$layer}
$start.Environment['XR_API_LAYER_PATH']=$directories -join ';'
$layers=@($start.Environment['XR_ENABLE_API_LAYERS'] -split ';' | Where-Object {$_})
if($layers -notcontains 'XR_APILAYER_CHEEKY_foveated_dlss'){$layers+='XR_APILAYER_CHEEKY_foveated_dlss'}
$start.Environment['XR_ENABLE_API_LAYERS']=$layers -join ';'
$process=[Diagnostics.Process]::Start($start)
[pscustomobject]@{pid=$process.Id;path=$start.FileName;startUtc=$process.StartTime.ToUniversalTime();layerDirectory=$layer}
