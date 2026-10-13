param(
    [string]$Ref='HEAD',
    [Parameter(Mandatory=$true)][string]$CheekySourceDirectory,
    [Parameter(Mandatory=$true)][string]$OpenXRSourceArchive,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [switch]$DraftWithoutLicense
)
# Creates a local draft from committed, reviewed inputs. Never deploys or launches.
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repository=Split-Path $PSScriptRoot -Parent
function GitChecked([string[]]$GitArguments) {
    $result=& git -C $repository @GitArguments
    if($LASTEXITCODE -ne 0){throw ('Git failed: '+($GitArguments -join ' '))}
    return $result
}
$commit=(GitChecked @('rev-parse',($Ref+'^{commit}'))).Trim()
$files=@(GitChecked @('ls-tree','-r','--name-only',$commit))
$publicFiles=@($files | Where-Object {
    $_ -in @('.gitignore','.gitattributes','README.md','LICENSE','THIRD_PARTY.md','RELEASE_PREP.md') -or
    $_ -match '^Loader/(src/[^/]+\.(cpp|h|hpp|def)|ui/VRMenu\.js|PC2VR\.Loader\.vcxproj|pc2vr\.ini\.example)$' -or
    $_ -match '^ThirdParty/OpenXR/(LICENSE\.txt|README\.md|SOURCE\.json|openxr_loader\.dll|include/openxr/[^/]+\.(h|txt))$' -or
    $_ -in @('Tools/package-alpha.ps1','Tools/sanitize-pdb-paths.ps1','Tools/deploy.ps1','Tools/gamedir.ps1') -or
    $_ -match '^Tools/CheekyDLSS/(README\.md|LICENSE\.upstream\.txt|pc2-cheeky-compatibility\.patch|CheekyFoveatedDLSS\.ini\.example|build\.ps1|package\.ps1|launch\.ps1)$' -or
    $_ -match '^Tools/test/(control_settings_test|upload_read_test|upload_registry_test|frustum_bounds_test|coverage_policy_test|vr_menu_test|vrs_capability_test|game_state_test)\.(cpp|vcxproj)$' -or
    $_ -in @('Tools/test/vr_menu_frontend_test.cjs','Tools/test/launcher_test.ps1')
})
foreach($required in @('LICENSE','README.md','THIRD_PARTY.md','RELEASE_PREP.md',
    'Loader/PC2VR.Loader.vcxproj','Loader/ui/VRMenu.js','Loader/pc2vr.ini.example',
    'ThirdParty/OpenXR/SOURCE.json','ThirdParty/OpenXR/openxr_loader.dll')) {
    if($required -eq 'LICENSE' -and $DraftWithoutLicense){continue}
    if($publicFiles -notcontains $required){throw "Required committed public input missing: $required. Select/commit the mod license before packaging."}
}
$output=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $output){throw 'Output exists; preserve it and choose a fresh directory.'}
$archive=(Resolve-Path -LiteralPath $OpenXRSourceArchive).Path
$cheeky=(Resolve-Path -LiteralPath $CheekySourceDirectory).Path
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild=& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if(!$msbuild){throw 'Visual Studio C++ MSBuild is required.'}
New-Item -ItemType Directory -Path $output | Out-Null
$sourceZip=Join-Path $output 'PC2VR-source.zip'
& git -C $repository archive --format=zip -o $sourceZip $commit -- @publicFiles
if($LASTEXITCODE -ne 0){throw 'Public source archive failed.'}
$publicRoot=Join-Path $output 'public-source'
Expand-Archive -LiteralPath $sourceZip -DestinationPath $publicRoot
# Only paths selected from this immutable Git tree may appear in the export.
$exported=@(Get-ChildItem -LiteralPath $publicRoot -File -Recurse | ForEach-Object {
    $_.FullName.Substring($publicRoot.Length+1).Replace('\','/')
})
if(@(Compare-Object ($publicFiles | Sort-Object) ($exported | Sort-Object)).Count){throw 'Public export file set differs.'}
$privatePattern='(?i)C:[/\\]Users[/\\]|765611\d{11}|PC2VR-(?:Research)|(?m)^\s*(?:[A-Z]:[/\\].*GameReferences)'
$ownerName=[Environment]::UserName
if($ownerName.Length -ge 4){$privatePattern+='|\b'+[Regex]::Escape($ownerName)+'\b'}
foreach($relative in $exported) {
    if($relative -match '\.dll$'){continue} # the one allowlisted Khronos loader
    $text=[IO.File]::ReadAllText((Join-Path $publicRoot $relative))
    if($text -match $privatePattern) {
        throw "Private machine detail found in public export: $relative"
    }
}
$pin=Get-Content -LiteralPath (Join-Path $publicRoot 'ThirdParty\OpenXR\SOURCE.json') -Raw | ConvertFrom-Json
if((Get-FileHash -LiteralPath $archive).Hash -ne $pin.sourceArchiveSha256){throw 'OpenXR corresponding-source archive hash differs from pin.'}
if((Get-FileHash -LiteralPath (Join-Path $publicRoot 'ThirdParty\OpenXR\openxr_loader.dll')).Hash -ne $pin.loaderSha256){throw 'OpenXR loader hash differs from pin.'}
$build=Join-Path $output 'core-build'
$objects=Join-Path $output 'core-obj'
& $msbuild (Join-Path $publicRoot 'Loader\PC2VR.Loader.vcxproj') /nologo /v:minimal /p:Configuration=Release /p:Platform=x64 ('/p:OutDir='+$build+'\') ('/p:IntDir='+$objects+'\')
if($LASTEXITCODE -ne 0){throw 'Fresh public-source core build failed.'}
& (Join-Path $publicRoot 'Tools\CheekyDLSS\build.ps1') -SourceDirectory $cheeky -BuildOnly
if($LASTEXITCODE -ne 0){throw 'Cheeky build checks failed.'}
$dependency=Join-Path $output 'cheeky-build-package'
& (Join-Path $publicRoot 'Tools\CheekyDLSS\package.ps1') -SourceDirectory $cheeky -OutputDirectory $dependency
$package=Join-Path $output 'game-root'
New-Item -ItemType Directory -Path (Join-Path $package 'source'),(Join-Path $package 'licenses'),(Join-Path $package 'PC2VR') -Force | Out-Null
foreach($file in Get-ChildItem -LiteralPath $dependency -File -Recurse) {
    $relative=$file.FullName.Substring($dependency.Length+1)
    # Keep our install guide at the root; preserve the dependency's own guide/receipt.
    if($relative -eq 'README.md'){$relative='CheekyFoveatedDLSS\README.md'}
    elseif($relative -eq 'manifest.json'){$relative='source\cheeky-manifest.json'}
    elseif($relative -eq 'source\upstream.zip'){$relative='source\Cheeky-upstream.zip'}
    $target=Join-Path $package $relative
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $target
}
$coreFiles=@('version.dll','PC2VR\openxr_loader.dll','PC2VR\VRMenu.js')
foreach($file in $coreFiles) {Copy-Item -LiteralPath (Join-Path $build $file) -Destination (Join-Path $package $file)}
foreach($file in @('version.dll','dxgi.dll',
    'CheekyFoveatedDLSS\CheekyFoveatedDLSSHost.dll',
    'CheekyFoveatedDLSS\CheekyFoveatedDLSSRuntime.dll',
    'PC2VR\CheekyOpenXR\CheekyOpenXRLayer.dll')) {
    $binary=Join-Path $package $file
    & (Join-Path $publicRoot 'Tools\sanitize-pdb-paths.ps1') -Path $binary
    $binaryText=[Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($binary))
    if($binaryText -match $privatePattern -or $binaryText -match '(?i)GameReferences[/\\]') {
        throw "Private build detail found in release binary: $file"
    }
}
if((Get-FileHash -LiteralPath (Join-Path $package 'PC2VR\VRMenu.js')).Hash -ne
    (Get-FileHash -LiteralPath (Join-Path $publicRoot 'Loader\ui\VRMenu.js')).Hash){throw 'Built menu differs from committed source.'}
Copy-Item -LiteralPath (Join-Path $publicRoot 'Loader\pc2vr.ini.example') -Destination (Join-Path $package 'PC2VR\pc2vr.ini')
foreach($file in @('README.md','THIRD_PARTY.md','RELEASE_PREP.md')){Copy-Item -LiteralPath (Join-Path $publicRoot $file) -Destination (Join-Path $package $file)}
$hasLicense=$publicFiles -contains 'LICENSE'
if($hasLicense){Copy-Item -LiteralPath (Join-Path $publicRoot 'LICENSE') -Destination (Join-Path $package 'LICENSE')}
else {
    'LOCAL DRAFT ONLY. DO NOT DISTRIBUTE: the original PC2VR mod license has not been selected. This package is for private review/testing; commit the selected license and regenerate before publication.' |
        Set-Content -LiteralPath (Join-Path $package 'DO-NOT-DISTRIBUTE.txt') -Encoding UTF8
}
Copy-Item -LiteralPath (Join-Path $publicRoot 'ThirdParty\OpenXR\LICENSE.txt') -Destination (Join-Path $package 'licenses\PC2VR-OpenXR-Apache2.txt')
Copy-Item -LiteralPath $sourceZip -Destination (Join-Path $package 'source\PC2VR-source.zip')
Copy-Item -LiteralPath $archive -Destination (Join-Path $package 'source\OpenXR-SDK-Source-1.1.63.zip')
$ini=Get-Content -LiteralPath (Join-Path $package 'PC2VR\pc2vr.ini') -Raw
if($ini -notmatch '(?m)^Stereo=1\r?$' -or $ini -match '(?m)^\[Experiments\]'){throw 'Fresh core configuration is not the reviewed stereo template.'}
$manifest=@(Get-ChildItem -LiteralPath $package -File -Recurse | Sort-Object FullName | ForEach-Object {
    $relative=$_.FullName.Substring($package.Length+1).Replace('\','/')
    if($relative -match '(?i)\.(pdb|ilk|iobj|ipdb|log|dmp|rdc|exe)$' -or
        $relative -match '(^|/)(HANDOFF\.md|Notes|GameReferences|Dumps|obj|\.git)(/|$)' -or
        $relative -match '(?i)(nvngx|nvapi|sl\.).*\.dll$'){throw "Forbidden release file: $relative"}
    [pscustomobject]@{path=$relative;bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
})
[pscustomobject]@{state=$(if($hasLicense){'local-alpha-draft'}else{'local-license-pending-draft'});sourceCommit=$commit;cheekyCommit='e016dc756383f1959fe0ce9257a73785cc18605f';
    openxrSourceCommit=$pin.sourceCommit;validation='Offline checks; exact-package physical smoke test pending';files=$manifest} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $package 'manifest.json') -Encoding UTF8
$zip=Join-Path $output 'PC2VR-alpha-draft.zip'
Compress-Archive -Path (Join-Path $package '*') -DestinationPath $zip
Get-FileHash -LiteralPath $zip | ForEach-Object {($_.Hash+'  PC2VR-alpha-draft.zip') | Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Encoding ASCII}
Write-Output "Created local alpha draft from $commit with $($manifest.Count) files. No installation, game launch, runtime change or GitHub upload."
