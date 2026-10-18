[CmdletBinding()]
param(
    [switch] $Zip
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildOutput = Join-Path $repo '.local\build-candidate\output'
$exe = Join-Path $buildOutput 'klogg.exe'
$qtRoot = Join-Path $repo '.local\Qt\6.7.3\msvc2019_64'
$qtBin = Join-Path $qtRoot 'bin'
$deployTool = Join-Path $qtBin 'windeployqt.exe'
$vcRoot = $env:VCINSTALLDIR
if (-not $vcRoot) {
    $vcRoot = 'C:\Program Files\Microsoft Visual Studio\2022\Preview\VC'
}
$crtRoot = Get-ChildItem -LiteralPath (Join-Path $vcRoot 'Redist\MSVC') -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64\Microsoft.VC143.CRT' } |
    Where-Object { Test-Path -LiteralPath (Join-Path $_ 'vcruntime140.dll') } |
    Select-Object -First 1
if (-not $crtRoot) { throw 'MSVC x64 redistributable DLLs are missing; set VCINSTALLDIR to the installed VC directory.' }
$env:VCINSTALLDIR = $vcRoot
$destination = Join-Path $repo '.local\dist\klogg-longline-windows-x64'
$zipPath = "$destination.zip"
$tbbNames = @('tbb12.dll', 'tbbmalloc.dll', 'tbbmalloc_proxy.dll')
$metadataFiles = @(
    @{ Source = (Join-Path $repo 'LICENSE'); Relative = 'LICENSE' },
    @{ Source = (Join-Path $repo 'NOTICE'); Relative = 'NOTICE' },
    @{ Source = (Join-Path $repo 'UPSTREAM.json'); Relative = 'UPSTREAM.json' },
    @{ Source = (Join-Path $repo 'upstream\klogg\COPYING'); Relative = 'COPYING' },
    @{ Source = (Join-Path $repo 'upstream\klogg\NOTICE'); Relative = 'upstream-NOTICE' },
    @{ Source = (Join-Path $repo 'patches\long-line-reliability.patch'); Relative = 'provenance\long-line-reliability.patch' },
    @{ Source = (Join-Path $repo 'patches\long-line-reliability.json'); Relative = 'provenance\long-line-reliability.json' }
)

foreach ($path in @($exe, $deployTool, (Join-Path $qtBin 'Qt6Core.dll'), (Join-Path $qtBin 'Qt6Widgets.dll'))) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required candidate or Qt runtime file is missing: $path"
    }
}
foreach ($metadata in $metadataFiles) {
    if (-not (Test-Path -LiteralPath $metadata.Source -PathType Leaf)) {
        throw "Required license/provenance file is missing: $($metadata.Source)"
    }
}
foreach ($name in $tbbNames) {
    if (-not (Test-Path -LiteralPath (Join-Path $buildOutput $name) -PathType Leaf)) {
        throw "Required oneTBB runtime DLL is missing: $(Join-Path $buildOutput $name)"
    }
}
if (Test-Path -LiteralPath $destination) {
    throw "Refusing to overwrite package directory: $destination"
}
if ($Zip -and (Test-Path -LiteralPath $zipPath)) {
    throw "Refusing to overwrite package archive: $zipPath"
}

$upstream = Get-Content -LiteralPath (Join-Path $repo 'UPSTREAM.json') -Raw | ConvertFrom-Json
$patchMetadata = Get-Content -LiteralPath (Join-Path $repo 'patches\long-line-reliability.json') -Raw | ConvertFrom-Json
$patchPath = Join-Path $repo 'patches\long-line-reliability.patch'
$patchHash = (Get-FileHash -LiteralPath $patchPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($patchMetadata.patch_sha256 -ne $patchHash) {
    throw 'Patch provenance JSON does not match patches/long-line-reliability.patch; regenerate the patch artifacts before packaging.'
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
New-Item -ItemType Directory -Path $destination | Out-Null
Copy-Item -LiteralPath $exe -Destination (Join-Path $destination 'klogg.exe')
Get-ChildItem -LiteralPath $crtRoot -Filter '*.dll' -File | Copy-Item -Destination $destination
$env:PATH = "$qtBin;$buildOutput;$env:PATH"

# Keep translations enabled: klogg embeds its own and Qt zh_CN translations as
# resources, while windeployqt may still need to deploy runtime translation data.
$deployArgs = @('--release', '--compiler-runtime', '--translations', 'zh_CN', '--dir', $destination, $exe)
Write-Host "Deploying Qt runtime with: $deployTool $($deployArgs -join ' ')"
$deployOutput = & $deployTool @deployArgs 2>&1
$deployExitCode = $LASTEXITCODE
$deployLogPath = Join-Path $destination 'windeployqt.log'
[System.IO.File]::WriteAllText($deployLogPath, (($deployOutput | ForEach-Object { [string]$_ }) -join [Environment]::NewLine) + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
if ($deployExitCode -ne 0) {
    throw "windeployqt failed with exit code $deployExitCode; partial package and log are retained at $destination"
}

foreach ($name in $tbbNames) {
    Copy-Item -LiteralPath (Join-Path $buildOutput $name) -Destination (Join-Path $destination $name) -Force
}
foreach ($metadata in $metadataFiles) {
    $target = Join-Path $destination $metadata.Relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath $metadata.Source -Destination $target
}

$sourceNote = @"
Klogg long-line reliability - internal Windows x64 validation package

This folder is an unsigned internal validation package. It is not an installer,
signed release, or claim of completed release qualification.

Upstream source: $($upstream.repository)/tree/$($upstream.commit)
Pinned source snapshot: $($upstream.commit)
Snapshot archive SHA-256: $($upstream.archive_sha256)
Import method: source snapshot only; no upstream Git history was imported.

Candidate source changes are represented by provenance/long-line-reliability.patch
and provenance/long-line-reliability.json. The patch digest is $patchHash.
The repository-relative canonical files are patches/long-line-reliability.patch
and patches/long-line-reliability.json. Their git apply check and changed-file
hashes are recorded in the JSON metadata.

The runtime was collected by tools/package.ps1 using Qt 6.7.3 MSVC x64
windeployqt --release --compiler-runtime --translations zh_CN, plus the candidate
build's oneTBB runtime DLLs.
"@
[System.IO.File]::WriteAllText((Join-Path $destination 'SOURCE-AND-PACKAGE-NOTES.txt'), $sourceNote, [System.Text.UTF8Encoding]::new($false))

$forbiddenExtensions = @('.lib', '.pdb', '.ilk', '.exp', '.obj', '.tmp')
$files = @(Get-ChildItem -LiteralPath $destination -File -Recurse | Sort-Object FullName)
$forbidden = @($files | Where-Object {
    $_.Extension.ToLowerInvariant() -in $forbiddenExtensions -or
    $_.Name -match '(^|[._-])(tests?|itests?|probe)([._-]|$)' -or
    $_.Extension.ToLowerInvariant() -eq '.conf'
})
if ($forbidden.Count -gt 0) {
    throw "Package contains disallowed build/test/config files; inspect and retain the partial output: $($forbidden.FullName -join ', ')"
}
$requiredFiles = @('klogg.exe', 'Qt6Core.dll', 'Qt6Widgets.dll', 'platforms\qwindows.dll', 'vcruntime140.dll', 'msvcp140.dll') + $tbbNames
foreach ($required in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $destination $required) -PathType Leaf)) {
        throw "Deployed runtime is incomplete; missing $required. Partial package retained at $destination"
    }
}

$manifestFiles = foreach ($file in $files) {
    $relative = $file.FullName.Substring($destination.Length + 1).Replace('\', '/')
    [pscustomobject]@{
        path = $relative
        size_bytes = $file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$manifest = [ordered]@{
    schema_version = 1
    package_kind = 'unsigned_internal_validation_package'
    variant = 'candidate'
    source_commit = $upstream.commit
    source_patch_sha256 = $patchHash
    executable_sha256 = (Get-FileHash -LiteralPath (Join-Path $destination 'klogg.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
    qt_version = '6.7.3'
    qt_architecture = 'win64_msvc2019_64'
    build_compiler = 'MSVC 19.44.35211 (Visual Studio 2022 Preview)'
    windeployqt_arguments = $deployArgs
    windeployqt_exit_code = $deployExitCode
    files = $manifestFiles
}
$manifestPath = Join-Path $destination 'SHA256SUMS.json'
[System.IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 6) + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))

if ($Zip) {
    Compress-Archive -Path (Join-Path $destination '*') -DestinationPath $zipPath -CompressionLevel Optimal
    $archiveHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Host "Package archive: $zipPath"
    Write-Host "Archive SHA-256: $archiveHash"
}
Write-Host "Internal validation package: $destination"
Write-Host "File manifest: $manifestPath"
