param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$output=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $output){throw 'Use a fresh test-only directory.'}
$source=Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\CheekyDLSS\launch.ps1') -Raw
$launchCall='$process=[Diagnostics.Process]::Start($start)'
$index=$source.IndexOf($launchCall,[StringComparison]::Ordinal)
if($index -lt 0 -or $index -ne $source.LastIndexOf($launchCall,[StringComparison]::Ordinal)){
    throw 'Launcher structure changed; review the non-launching harness before use.'
}
# Execute actual path/environment construction, stopping BEFORE process creation.
$probe=$source.Substring(0,$index)+"`n"+'return $start'
$game=Join-Path $output 'game folder with spaces'
$outside=Join-Path $output 'outside'
New-Item -ItemType Directory -Path $game,$outside -Force | Out-Null
foreach($relative in @('PlanetCoaster2.exe','version.dll','dxgi.dll',
    'CheekyFoveatedDLSS\CheekyFoveatedDLSSHost.dll',
    'CheekyFoveatedDLSS\CheekyFoveatedDLSSRuntime.dll',
    'PC2VR\CheekyOpenXR\CheekyOpenXRLayer.dll',
    'PC2VR\CheekyOpenXR\XR_APILAYER_CHEEKY_foveated_dlss.json')) {
    $file=Join-Path $game $relative
    New-Item -ItemType Directory -Path (Split-Path $file -Parent) -Force | Out-Null
    [IO.File]::WriteAllText($file,'TEST ONLY: not an executable or DLL')
}
$inGame=Join-Path $game 'probe.ps1';$outGame=Join-Path $outside 'probe.ps1'
[IO.File]::WriteAllText($inGame,$probe);[IO.File]::WriteAllText($outGame,$probe)
$saved=@{}
foreach($key in @('PC2VR_GAME_DIR','XR_API_LAYER_PATH','XR_ENABLE_API_LAYERS','XR_RUNTIME_JSON')) {
    $saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')
}
try {
    $env:PC2VR_GAME_DIR=$null
    $env:XR_API_LAYER_PATH='test-existing-layer'
    $env:XR_ENABLE_API_LAYERS='TEST_existing_layer'
    $env:XR_RUNTIME_JSON='test-selected-runtime.json'
    foreach($start in @((& $inGame),(& $outGame -GameDirectory $game))) {
        if($start.FileName -ne (Join-Path $game 'PlanetCoaster2.exe') -or $start.WorkingDirectory -ne $game -or $start.UseShellExecute){throw 'Portable path construction failed'}
        if($start.Environment['XR_RUNTIME_JSON'] -ne $env:XR_RUNTIME_JSON){throw 'Launcher changes selected runtime'}
        if($start.Environment['XR_ENABLE_API_LAYERS'] -notmatch 'TEST_existing_layer' -or
            $start.Environment['XR_ENABLE_API_LAYERS'] -notmatch 'XR_APILAYER_CHEEKY_foveated_dlss'){throw 'Child layer activation lost an existing layer'}
        if($start.Environment['XR_API_LAYER_PATH'] -notmatch 'test-existing-layer' -or
            !$start.Environment['XR_API_LAYER_PATH'].Contains((Join-Path $game 'PC2VR\CheekyOpenXR'))){throw 'Child layer path failed'}
    }
    $env:PC2VR_GAME_DIR=$game
    $fromEnvironment=& $outGame
    if($fromEnvironment.FileName -ne (Join-Path $game 'PlanetCoaster2.exe')){throw 'Environment game path failed'}
    if($env:XR_API_LAYER_PATH -ne 'test-existing-layer' -or $env:XR_ENABLE_API_LAYERS -ne 'TEST_existing_layer' -or
        $env:XR_RUNTIME_JSON -ne 'test-selected-runtime.json'){throw 'Launcher modifies parent environment'}
    $env:PC2VR_GAME_DIR=$null
    $rejected=$false
    try{& $outGame | Out-Null}catch{$rejected=$true}
    if(!$rejected){throw 'Missing location must reject instead of guessing a drive'}
    Write-Output 'PASS: adjacent/explicit/environment paths with spaces, missing-path rejection, child-only layer activation and unchanged selected runtime. No game launched.'
} finally {
    foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
}
