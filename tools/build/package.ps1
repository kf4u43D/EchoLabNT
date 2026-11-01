$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$version = ([IO.File]::ReadAllText((Join-Path $root 'VERSION'))).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?$') { throw 'Invalid VERSION.' }
& (Join-Path $PSScriptRoot 'build_nt.ps1')
$out = Join-Path $root ('dist/EchoLabNT-public-' + $version)
New-Item -ItemType Directory -Path $out -Force | Out-Null
$packageFiles = @('echolabnt.o', 'BUILD.txt', 'README.md', 'RELEASE.md', 'docs/INSTALL_NT.md', 'docs/PRESETS.md', 'SDK_LICENSE.txt')
$build = Get-Content -LiteralPath (Join-Path $root 'build/nt/build_manifest.json') -Raw | ConvertFrom-Json
if ($build.version -ne $version) { throw 'Object build version does not match VERSION.' }
@("EchoLabNT $version", "Build: $($build.build_id)", "Target: disting NT firmware $($build.target_firmware) / API $($build.api_version)", "SHA-256 echolabnt.o: $($build.sha256)") | Set-Content -LiteralPath (Join-Path $out 'BUILD.txt') -Encoding ascii
foreach ($name in @('README.md', 'RELEASE.md', 'docs/INSTALL_NT.md', 'docs/PRESETS.md')) {
    $destination = Join-Path $out $name
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $root $name) -Destination $destination -Force
}
Copy-Item -LiteralPath (Join-Path $root 'build/nt/echolabnt.o') -Destination (Join-Path $out 'echolabnt.o') -Force
Copy-Item -LiteralPath (Join-Path $root 'third_party/distingNT_API_1.18.0/LICENSE') -Destination (Join-Path $out 'SDK_LICENSE.txt') -Force
New-Item -ItemType Directory -Path (Join-Path $out 'profiles') -Force | Out-Null
foreach ($profile in @(Get-ChildItem -LiteralPath (Join-Path $root 'models/presets') -Filter '*.profile' -File | Sort-Object Name)) {
    Copy-Item -LiteralPath $profile.FullName -Destination (Join-Path $out ('profiles/' + $profile.Name)) -Force
    $packageFiles += 'profiles/' + $profile.Name
}
if ($packageFiles.Count -le 7) { throw 'No factory profiles found.' }
$checksums = $packageFiles | Sort-Object | ForEach-Object { (Get-FileHash -LiteralPath (Join-Path $out $_) -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $_ }
$checksums | Set-Content -LiteralPath (Join-Path $out 'SHA256SUMS.txt') -Encoding ascii
$archiveName = 'EchoLabNT-public-' + $version + '.zip'
$archive = Join-Path $root ('dist/' + $archiveName)
$archiveStream = [IO.File]::Open($archive, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite)
$zip = [IO.Compression.ZipArchive]::new($archiveStream, [IO.Compression.ZipArchiveMode]::Create, $false)
try {
    foreach ($name in (($packageFiles + 'SHA256SUMS.txt') | Sort-Object)) {
        $entry = $zip.CreateEntry($name, [IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime = [DateTimeOffset]::Parse('2026-10-23T00:00:00+00:00')
        $inputStream = [IO.File]::OpenRead((Join-Path $out $name))
        $entryStream = $entry.Open()
        try { $inputStream.CopyTo($entryStream) } finally { $entryStream.Dispose(); $inputStream.Dispose() }
    }
} finally { $zip.Dispose(); $archiveStream.Dispose() }
((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $archiveName) | Set-Content -LiteralPath ($archive + '.sha256') -Encoding ascii
Write-Output "Local package: $archive"
