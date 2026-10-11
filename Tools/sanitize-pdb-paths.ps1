param([Parameter(Mandatory=$true)][string]$Path)
# Replace only a PE CodeView debug path with its filename. No code is modified.
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$bytes=[IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Path).Path)
function U16([int]$offset) { [BitConverter]::ToUInt16($bytes,$offset) }
function U32([int]$offset) { [BitConverter]::ToUInt32($bytes,$offset) }
if($bytes.Length -lt 64 -or (U16 0) -ne 0x5A4D){throw 'Not a PE image.'}
$pe=[int](U32 0x3C)
if($pe -lt 0 -or $pe+264 -gt $bytes.Length -or (U32 $pe) -ne 0x4550){throw 'Invalid PE header.'}
$optional=$pe+24
if((U16 $optional) -ne 0x20B){throw 'Expected a PE32+ image.'}
$sectionCount=U16 ($pe+6)
$sectionTable=$optional+(U16 ($pe+20))
if($sectionTable+40*$sectionCount -gt $bytes.Length){throw 'Truncated section table.'}
$debugRva=U32 ($optional+112+6*8)
$debugSize=U32 ($optional+112+6*8+4)
if(!$debugRva -and !$debugSize){return}
if(!$debugRva -or !$debugSize -or $debugSize%28){throw 'Invalid debug directory.'}
$debugOffset=-1
for($s=0;$s -lt $sectionCount;$s++) {
    $section=$sectionTable+40*$s
    $rva=U32 ($section+12);$rawSize=U32 ($section+16);$rawOffset=U32 ($section+20)
    if($debugRva -ge $rva -and [uint64]$debugRva+$debugSize -le [uint64]$rva+$rawSize) {
        $debugOffset=[int]($rawOffset+$debugRva-$rva);break
    }
}
if($debugOffset -lt 0 -or [uint64]$debugOffset+$debugSize -gt $bytes.Length){throw 'Debug directory outside file data.'}
$changed=0
for($d=0;$d -lt $debugSize;$d+=28) {
    $entry=$debugOffset+$d
    if((U32 ($entry+12)) -ne 2){continue} # IMAGE_DEBUG_TYPE_CODEVIEW
    $size=U32 ($entry+16);$start=U32 ($entry+24)
    if($size -lt 25 -or [uint64]$start+$size -gt $bytes.Length){throw 'CodeView data outside file data.'}
    if((U32 $start) -ne 0x53445352){throw 'Expected RSDS CodeView data.'}
    $pathStart=[int]$start+24;$end=$pathStart
    while($end -lt [uint64]$start+$size -and $bytes[$end]){$end++}
    if($end -ge [uint64]$start+$size){throw 'CodeView path is not terminated.'}
    $original=[Text.Encoding]::UTF8.GetString($bytes,$pathStart,$end-$pathStart)
    $name=($original.Replace('\','/').Split('/'))[-1]
    if(!$name -or !$name.EndsWith('.pdb',[StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected CodeView filename.'}
    if($original -eq $name){continue}
    $replacement=[Text.Encoding]::UTF8.GetBytes($name)
    if($replacement.Length -gt $end-$pathStart){throw 'Replacement debug path exceeds original range.'}
    [Array]::Clear($bytes,$pathStart,$end-$pathStart)
    [Array]::Copy($replacement,0,$bytes,$pathStart,$replacement.Length)
    $changed++
}
if($changed){[IO.File]::WriteAllBytes((Resolve-Path -LiteralPath $Path).Path,$bytes)}
Write-Output ('Public debug metadata: '+[IO.Path]::GetFileName($Path)+' ('+$changed+' path entries)')
