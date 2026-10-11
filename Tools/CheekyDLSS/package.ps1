param([Parameter(Mandatory=$true)][string]$SourceDirectory,
      [Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$source=(Resolve-Path -LiteralPath $SourceDirectory).Path
$output=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $output){throw 'Existing output left untouched; choose a fresh directory.'}
$revision='e016dc756383f1959fe0ce9257a73785cc18605f'
if((& git -C $source rev-parse HEAD).Trim() -ne $revision){throw 'Unexpected upstream revision.'}
$diff=(& git -C $source diff --binary) -join "`n"
$patch=Join-Path $PSScriptRoot 'pc2-cheeky-compatibility.patch'
if($diff.Trim() -ne ([IO.File]::ReadAllText($patch).Replace("`r`n","`n")).Trim()){
    throw 'Source does not match the supplied patch.'
}
$bin=Join-Path $source 'bin\Release'
$files=[ordered]@{
    'dxgi.dll'=(Join-Path $bin 'standalone-loader\dxgi.dll')
    'CheekyFoveatedDLSS\CheekyFoveatedDLSSHost.dll'=(Join-Path $bin 'CheekyFoveatedDLSS\CheekyFoveatedDLSSHost.dll')
    'CheekyFoveatedDLSS\CheekyFoveatedDLSSRuntime.dll'=(Join-Path $bin 'CheekyFoveatedDLSS\CheekyFoveatedDLSSRuntime.dll')
    'CheekyFoveatedDLSS\CheekyFoveatedDLSS.ini'=(Join-Path $PSScriptRoot 'CheekyFoveatedDLSS.ini.example')
    'PC2VR\CheekyOpenXR\CheekyOpenXRLayer.dll'=(Join-Path $bin 'CheekyOpenXRLayer.dll')
    'PC2VR\CheekyOpenXR\XR_APILAYER_CHEEKY_foveated_dlss.json'=(Join-Path $bin 'XR_APILAYER_CHEEKY_foveated_dlss.json')
    'launch.ps1'=(Join-Path $PSScriptRoot 'launch.ps1')
    'README.md'=(Join-Path $PSScriptRoot 'README.md')
    'licenses\Cheeky-GPLv3.txt'=(Join-Path $source 'LICENSE')
    'licenses\MinHook.txt'=(Join-Path $source 'third_party\reshade\deps\minhook\LICENSE.txt')
    'licenses\DearImGui.txt'=(Join-Path $source 'third_party\reshade\deps\imgui\LICENSE.txt')
    'licenses\OpenVR.txt'=(Join-Path $source 'third_party\openvr\LICENSE')
    'licenses\OpenXR.txt'=(Join-Path $source 'third_party\openxr\LICENSE.txt')
    'licenses\UEVR-API.txt'=(Join-Path $source 'third_party\uevr\LICENSE.txt')
    'licenses\ReShade.txt'=(Join-Path $source 'third_party\reshade\LICENSE.md')
    'source\pc2-cheeky-compatibility.patch'=$patch
    'source\build.ps1'=(Join-Path $PSScriptRoot 'build.ps1')
}
foreach($file in $files.Values){if(!(Test-Path -LiteralPath $file -PathType Leaf)){throw "Missing build input: $file"}}
New-Item -ItemType Directory -Path $output | Out-Null
foreach($entry in $files.GetEnumerator()){
    $destination=Join-Path $output $entry.Key
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $entry.Value -Destination $destination
    if((Get-FileHash -LiteralPath $entry.Value).Hash -ne (Get-FileHash -LiteralPath $destination).Hash){throw 'Copy mismatch.'}
}
& git -C $source archive --format=zip -o (Join-Path $output 'source\upstream.zip') $revision
if($LASTEXITCODE -ne 0){throw 'Corresponding source archive failed.'}
$manifest=@(Get-ChildItem -LiteralPath $output -File -Recurse | ForEach-Object {
    [pscustomobject]@{path=$_.FullName.Substring($output.Length+1);sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
})
[pscustomobject]@{upstream=$revision;patchSha256=(Get-FileHash -LiteralPath $patch).Hash;files=$manifest} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'manifest.json')
Write-Output "Staged optional dependency: $output. No game files, loader, graphics settings or system layer registration changed."
