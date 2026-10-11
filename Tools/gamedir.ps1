# Dot-source to get $GameDir, the Planet Coaster 2 install folder:
#   . "$PSScriptRoot\gamedir.ps1"     (from Tools\test: . "$PSScriptRoot\..\gamedir.ps1")
# Order: the PC2VR_GAME_DIR environment variable, then the Steam library that holds
# app 2688950 (from Steam's libraryfolders.vdf).
function Find-Pc2GameDir {
    if ($env:PC2VR_GAME_DIR) { return $env:PC2VR_GAME_DIR }
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if (-not $steam) { throw 'Steam not found; set PC2VR_GAME_DIR to the game folder.' }
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    $library = $null
    foreach ($line in Get-Content $vdf) {
        if ($line -match '^\s*"path"\s*"(.+)"') { $library = $Matches[1].Replace('\\', '\') }
        elseif ($line -match '^\s*"2688950"' -and $library) {
            $dir = Join-Path $library 'steamapps\common\Planet Coaster 2'
            if (Test-Path "$dir\PlanetCoaster2.exe") { return $dir }
        }
    }
    throw 'Planet Coaster 2 not found in any Steam library; set PC2VR_GAME_DIR to the game folder.'
}
$GameDir = Find-Pc2GameDir
