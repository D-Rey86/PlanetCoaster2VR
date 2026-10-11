param(
    [string]$GameDir = '',  # default: gamedir.ps1's lookup
    [switch]$Remove
)
# Copies the built mod into the game folder (or removes it with -Remove).
# Refuses while the game is running, because the DLL would be locked.
$ErrorActionPreference = 'Stop'
if (-not $GameDir) { . "$PSScriptRoot\gamedir.ps1" }
$root = Split-Path $PSScriptRoot -Parent
if (Get-Process -Name PlanetCoaster2 -ErrorAction SilentlyContinue) { throw 'Close Planet Coaster 2 first.' }
if (-not (Test-Path "$GameDir\PlanetCoaster2.exe")) { throw "Game not found at $GameDir" }

$dst = "$GameDir\version.dll"
if (Test-Path $dst) {
    $info = (Get-Item $dst).VersionInfo
    $ours = (Select-String -Path $dst -Pattern 'PC2VR loader' -SimpleMatch -Quiet)
    if (-not $ours) { throw "A version.dll that is not PC2VR already exists in the game folder: $dst ($($info.FileDescription)). Not overwriting." }
}

if ($Remove) {
    Remove-Item $dst -ErrorAction SilentlyContinue
    Remove-Item "$GameDir\PC2VR\openxr_loader.dll" -ErrorAction SilentlyContinue
    Remove-Item "$GameDir\PC2VR\VRMenu.js" -ErrorAction SilentlyContinue
    Write-Output 'PC2VR removed (logs in PC2VR\ kept).'
    return
}

foreach ($file in 'version.dll', 'PC2VR\openxr_loader.dll', 'PC2VR\VRMenu.js') {
    if (-not (Test-Path -LiteralPath "$root\Builds\$file")) { throw "Build output missing: $file. Build the current branch first." }
}
New-Item -ItemType Directory -Force "$GameDir\PC2VR" | Out-Null
Copy-Item "$root\Builds\version.dll" $dst -Force
Copy-Item "$root\Builds\PC2VR\openxr_loader.dll" "$GameDir\PC2VR\" -Force
Copy-Item "$root\Builds\PC2VR\VRMenu.js" "$GameDir\PC2VR\" -Force
Get-Item $dst, "$GameDir\PC2VR\openxr_loader.dll", "$GameDir\PC2VR\VRMenu.js" | Select-Object FullName, Length, LastWriteTime
