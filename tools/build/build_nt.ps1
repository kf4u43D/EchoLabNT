param([string]$SdkPath = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$version = ([IO.File]::ReadAllText((Join-Path $root 'VERSION'))).Trim()
$python = Join-Path $root '.venv/Scripts/python.exe'
if (!(Test-Path -LiteralPath $python)) { $python = (Get-Command python -ErrorAction Stop).Source }
& $python (Join-Path $root 'tools/build/generate_presets.py') --check
if ($LASTEXITCODE -ne 0) { throw 'Factory preset outputs are stale. Run tools/build/generate_presets.py.' }
if ($version -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?$') { throw 'VERSION invalide.' }
if (!$SdkPath) { $SdkPath = Join-Path $root 'third_party/distingNT_API_1.18.0' }
$header = Join-Path $SdkPath 'include/distingnt/api.h'
if (!(Test-Path -LiteralPath $header)) { throw 'SDK missing. Run tools/import/fetch_sdk.ps1; see third_party/dependencies.lock.json.' }
$expectedHeader = '2fb0a626ee495cd69d7c2c68aa3ec1206ca7f853e7967aad8f709fc4613e4206'
$headerBytes = [Text.Encoding]::UTF8.GetBytes([IO.File]::ReadAllText($header).Replace("`r`n", "`n"))
$canonicalHeaderHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($headerBytes)).ToLowerInvariant()
if ($canonicalHeaderHash -ne $expectedHeader) { throw 'Header SDK différent du snapshot épinglé (après normalisation CRLF). Revalider le contrat avant compilation.' }
$compiler = (Get-Command 'arm-none-eabi-g++' -ErrorAction Stop).Source
$out = Join-Path $root 'build/nt'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$sourceFiles = @(Get-ChildItem -LiteralPath (Join-Path $root 'src/dsp'), (Join-Path $root 'src/nt') -File | Sort-Object FullName)
$sourceFiles += Get-Item -LiteralPath (Join-Path $root 'VERSION')
$sourceFiles = @($sourceFiles | Sort-Object FullName)
$sources = @($sourceFiles | ForEach-Object { [ordered]@{ path = [IO.Path]::GetRelativePath($root, $_.FullName).Replace('\', '/'); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() } })
$bytes = [Text.Encoding]::UTF8.GetBytes(($sources | ConvertTo-Json -Compress))
$sourceHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
$buildId = $version + '+' + $sourceHash.Substring(0, 12)
$idHeader = Join-Path $out 'build_id.h'
('#define ECHOLAB_BUILD_ID "' + $buildId + '"') | Set-Content -LiteralPath $idHeader -Encoding ascii
$object = Join-Path $out 'echolabnt.o'
$arguments = @('-std=c++17', '-mcpu=cortex-m7', '-mfpu=fpv5-d16', '-mfloat-abi=hard', '-mthumb', '-fno-rtti', '-fno-exceptions', '-Os', '-fPIC', '-Wall', '-Wextra', ('-I' + (Join-Path $SdkPath 'include')), '-include', $idHeader, '-c', '-o', $object, (Join-Path $root 'src/nt/echo_lab.cpp'))
& $compiler @arguments
if ($LASTEXITCODE -ne 0) { throw 'Compilation ARM échouée.' }
$elf = & arm-none-eabi-readelf -h -A -S -r $object
if ($LASTEXITCODE -ne 0) { throw 'Audit ELF échoué.' }
$elf | Set-Content -LiteralPath (Join-Path $out 'elf_audit.txt') -Encoding utf8
if (($elf -join "`n") -notmatch 'REL \(Relocatable file\)' -or ($elf -join "`n") -notmatch 'Machine:\s+ARM') { throw 'La sortie doit être un objet relogeable ARM.' }
if (($elf -join "`n") -match '\.init_array|\.ctors') { throw 'Initialisation globale dynamique inattendue.' }
$undefined = @(& arm-none-eabi-nm -u $object)
if ($LASTEXITCODE -ne 0) { throw 'Audit symboles échoué.' }
$undefined | Set-Content -LiteralPath (Join-Path $out 'undefined_symbols.txt') -Encoding utf8
$symbols = @($undefined | ForEach-Object { ($_ -split '\s+')[-1] })
foreach ($symbol in $symbols) {
    # SDK api.h exports the three UI functions; official examples use memcpy.
    if ($symbol -notmatch '^(_GLOBAL_OFFSET_TABLE_|NT_globals|memset|memcpy|expf|NT_algorithmIndex|NT_updateParameterPages|NT_drawText|NT_setParameterFromAudio|NT_parameterOffset)$') { throw "Import runtime non audité : $symbol" }
}
$defined = & arm-none-eabi-nm --defined-only $object
if (($defined -join "`n") -notmatch 'pluginEntry') { throw 'pluginEntry absent.' }
$gitHead = & git -c ('safe.directory=' + $root.Replace('\', '/')) -C $root rev-parse --verify HEAD 2>$null
if ($LASTEXITCODE -ne 0) { $gitHead = $null }
$manifest = [ordered]@{
    schema_version = 1; build_id = $buildId; version = $version; release_stage = $(if ($version.Contains('-')) { 'prerelease' } else { 'release' }); source_tree_sha256 = $sourceHash
    git_commit = $gitHead; source_files = $sources; sdk_commit = 'f440d20c2a96a9ca43892c7418a1d8185ad47eb5'
    sdk_tag = 'v1.18.0'; sdk_api_header_sha256 = $expectedHeader; api_version = 13; factory_guid = 'ELnt'
    target_firmware = '1.18.0'; entry_section = '.text'
    compiler = (& $compiler --version | Select-Object -First 1); flags = $arguments
    object = 'echolabnt.o'; bytes = (Get-Item -LiteralPath $object).Length
    sha256 = (Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash.ToLowerInvariant()
    undefined_symbols = $symbols; compiled = 'passed'; hardware_test = 'not_run'; sonic_fidelity = 'not_qualified'
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'build_manifest.json') -Encoding utf8
Write-Output "ARM OK : $object ($buildId)"
Write-Output ('SHA-256 ' + $manifest.sha256)
