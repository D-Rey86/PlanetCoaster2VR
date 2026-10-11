param([Parameter(Mandatory=$true)][string]$SourceDirectory,
      [switch]$BuildOnly)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$revision='e016dc756383f1959fe0ce9257a73785cc18605f'
$source=(Resolve-Path -LiteralPath $SourceDirectory).Path
$patch=Join-Path $PSScriptRoot 'pc2-cheeky-compatibility.patch'
if(!$BuildOnly -and (Get-Process PlanetCoaster2 -ErrorAction SilentlyContinue)){throw 'The game is running; use -BuildOnly to compile and run CPU-only checks without affecting it.'}
$head=(& git -C $source rev-parse HEAD).Trim()
if($LASTEXITCODE -ne 0 -or $head -ne $revision){throw 'Unexpected upstream revision.'}
$diff=(& git -C $source diff --binary) -join "`n"
if($LASTEXITCODE -ne 0){throw 'Unable to inspect source changes.'}
if(!$diff){
    & git -C $source apply --check $patch
    if($LASTEXITCODE -ne 0){throw 'Patch does not apply cleanly.'}
    & git -C $source apply $patch
    if($LASTEXITCODE -ne 0){throw 'Patch failed.'}
} elseif($diff.Trim() -ne ([IO.File]::ReadAllText($patch).Replace("`r`n","`n")).Trim()) {
    throw 'Source contains changes other than the supplied patch; left untouched.'
}
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild=& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if(!$msbuild){throw 'Visual Studio C++ MSBuild is required.'}
& $msbuild (Join-Path $source 'CheekyFoveatedDLSS.sln') /m /nologo /v:minimal '/t:CheekyFoveatedDLSSRuntime,CheekyFoveatedDLSSHost,CheekyStandaloneProxy,CheekyOpenXRLayer,CheekyTests,CheekyOverlayTests,CheekyUEVRTests' /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
if($LASTEXITCODE -ne 0){throw 'Build failed.'}
$tests=Join-Path $source 'bin\Release\CheekyTests.exe'
$policyTests=Join-Path $source 'bin\Release\CheekyUEVRTests.exe'
$overlayTests=Join-Path $source 'bin\Release\CheekyOverlayTests.exe'
& $policyTests --pc2-settings-policy generic
if($LASTEXITCODE -ne 0){throw 'Failed generic process settings policy.'}
& $tests --hook-path
if($LASTEXITCODE -ne 0){throw 'Failed generic process hook routing.'}
& $overlayTests --ui
if($LASTEXITCODE -ne 0){throw 'Failed generic process menu checks.'}
# These are console-only test binaries, not the game. Their basename exercises
# the exact production process policy; neither selected mode initializes a GPU.
foreach($case in @(@{name='settings';file=$policyTests;args=@('--pc2-settings-policy','pc2')},
                  @{name='menu';file=$overlayTests;args=@('--ui')})){
    $testDirectory=Join-Path $source ('bin\Release\pc2-policy-checks\'+$case.name)
    New-Item -ItemType Directory -Path $testDirectory -Force | Out-Null
    $helper=Join-Path $testDirectory 'PlanetCoaster2.exe'
    Copy-Item -LiteralPath $case.file -Destination $helper -Force
    & $helper @($case.args)
    if($LASTEXITCODE -ne 0){throw ('Failed PC2 '+$case.name+' policy checks.')}
}
if($BuildOnly){
    Write-Output 'Dependency built; CPU settings/reset/menu compatibility checks passed. No GPU tests, game launch or deployment performed.'
    return
}
foreach($test in '--native-shared-calibration','--retained-calibration','--mixed-calibration','--crop-calibration-dx12','--d3d12-safety'){
    if(Get-Process PlanetCoaster2 -ErrorAction SilentlyContinue){throw 'The game started; GPU tests stopped without touching it.'}
    & $tests $test
    if($LASTEXITCODE -ne 0){throw "Failed $test"}
}
Write-Output 'Dependency and focused GPU tests passed. Headset correctness and performance still require runtime validation.'
