[CmdletBinding()]
param(
    [switch] $Zip,

    [ValidateRange(1, 256)]
    [int] $Jobs = 4,

    [string] $RunId
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildOutput = Join-Path $repo '.local\build-candidate\output'
$exe = Join-Path $buildOutput 'klogg.exe'
$buildRoot = Join-Path $repo '.local\build-candidate'
$qtRoot = Join-Path $repo '.local\Qt\6.7.3\msvc2019_64'
$qtBin = Join-Path $qtRoot 'bin'
$deployTool = Join-Path $qtBin 'windeployqt.exe'
$python = $null
$verifyScript = Join-Path $repo 'tools\verify_repository.py'
$baselineScript = Join-Path $repo 'tools\prepare_baseline.py'
$exportScript = Join-Path $repo 'tools\export_patch.py'
$buildScript = Join-Path $repo 'tools\build.ps1'
$upstreamRoot = Join-Path $repo 'upstream\klogg'
$upstreamInfoPath = Join-Path $repo 'UPSTREAM.json'
if (-not $RunId) {
    $headForRunId = (& git -C $repo rev-parse --short=8 HEAD 2>$null | Select-Object -First 1)
    if ($LASTEXITCODE -ne 0 -or -not $headForRunId) { throw 'Unable to determine Git HEAD for the default RunId.' }
    $RunId = "$(Get-Date -Format 'yyyyMMdd-HHmmss-fff')-$($headForRunId.Trim())"
}
if ($RunId -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$') {
    throw 'RunId must be 1-64 ASCII letters, digits, dots, underscores, or hyphens, and must start with a letter or digit.'
}
$destination = Join-Path (Join-Path $repo '.local\dist') "klogg-longline-windows-x64-$RunId"
$zipPath = "$destination.zip"
if (Test-Path -LiteralPath $destination) { throw "Refusing to overwrite package directory: $destination" }
if (Test-Path -LiteralPath $zipPath) { throw "Refusing to overwrite package archive: $zipPath" }

$python = (Get-Command 'python' -ErrorAction Stop).Source
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
$tbbNames = @('tbb12.dll', 'tbbmalloc.dll', 'tbbmalloc_proxy.dll')
$metadataFiles = @(
    @{ Source = (Join-Path $repo 'LICENSE'); Relative = 'LICENSE' },
    @{ Source = (Join-Path $repo 'NOTICE'); Relative = 'NOTICE' },
    @{ Source = (Join-Path $repo 'UPSTREAM.json'); Relative = 'UPSTREAM.json' },
    @{ Source = (Join-Path $repo 'upstream\klogg\COPYING'); Relative = 'COPYING' },
    @{ Source = (Join-Path $repo 'upstream\klogg\NOTICE'); Relative = 'upstream-NOTICE' },
    @{ Source = (Join-Path $repo 'patches\long-line-reliability.patch'); Relative = 'provenance\historical-long-line-reliability.patch' },
    @{ Source = (Join-Path $repo 'patches\long-line-reliability.json'); Relative = 'provenance\historical-long-line-reliability.json' }
)

foreach ($required in @($verifyScript, $baselineScript, $exportScript, $buildScript, $upstreamInfoPath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required packaging input is missing: $required" }
}

# Validate provenance before building, and retain a complete source snapshot for
# comparison after the build. The source fingerprint is independent of Git HEAD.
& $python $verifyScript
if ($LASTEXITCODE -ne 0) { throw "Repository integrity verification failed with exit code $LASTEXITCODE." }
$baselineOutput = & $python $baselineScript
if ($LASTEXITCODE -ne 0) { throw "Pristine baseline verification failed with exit code $LASTEXITCODE." }
$baselineVerification = ($baselineOutput -join "`n") | ConvertFrom-Json
function Get-ProductionSourceSnapshot {
    $entries = @(Get-ChildItem -LiteralPath $upstreamRoot -Force -File -Recurse | ForEach-Object {
        [pscustomobject]@{
            path = $_.FullName.Substring($repo.Length + 1).Replace('\', '/')
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    } | Sort-Object path)
    $canonical = ($entries | ForEach-Object { "$($_.path) $($_.sha256)" }) -join "`n"
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $sourceFingerprint = [BitConverter]::ToString($sha256.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($canonical))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha256.Dispose()
    }
    return [pscustomobject]@{ entries = $entries; fingerprint = $sourceFingerprint }
}
function Get-GitState {
    $head = (& git -C $repo rev-parse HEAD 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $head -notmatch '^[0-9a-fA-F]{40,64}$') { throw 'Unable to read Git HEAD.' }
    $status = @(& git -C $repo status --porcelain=v1 --untracked-files=normal 2>&1)
    if ($LASTEXITCODE -ne 0) { throw 'Unable to read Git working tree status.' }
    return [pscustomobject]@{ head = $head; dirty = ($status.Count -gt 0); status = $status }
}
$sourceBefore = Get-ProductionSourceSnapshot
$gitState = Get-GitState
if (Test-Path -LiteralPath $destination) { throw "Refusing to overwrite package directory: $destination" }
if (Test-Path -LiteralPath $zipPath) { throw "Refusing to overwrite package archive: $zipPath" }

# Always rebuild the candidate immediately before packaging.
& $buildScript -Variant candidate -Backend qt -CleanFirst -Jobs $Jobs
$sourceAfter = Get-ProductionSourceSnapshot
if ($sourceBefore.fingerprint -ne $sourceAfter.fingerprint -or
    @(Compare-Object -ReferenceObject $sourceBefore.entries -DifferenceObject $sourceAfter.entries -Property path, sha256).Count -gt 0) {
    throw 'Production source file set or content changed during the build; refusing to package.'
}

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
$upstream = Get-Content -LiteralPath (Join-Path $repo 'UPSTREAM.json') -Raw | ConvertFrom-Json
$patchMetadata = Get-Content -LiteralPath (Join-Path $repo 'patches\long-line-reliability.json') -Raw | ConvertFrom-Json
$patchPath = Join-Path $repo 'patches\long-line-reliability.patch'
$patchHash = (Get-FileHash -LiteralPath $patchPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($patchMetadata.patch_sha256 -ne $patchHash) {
    throw 'Patch provenance JSON does not match patches/long-line-reliability.patch; regenerate the patch artifacts before packaging.'
}

if (Test-Path -LiteralPath $destination) { throw "Refusing to overwrite package directory: $destination" }
if (Test-Path -LiteralPath $zipPath) { throw "Refusing to overwrite package archive: $zipPath" }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
New-Item -ItemType Directory -Path (Join-Path $destination 'provenance') | Out-Null
$currentPatchPath = Join-Path $destination 'provenance\current-source.patch'
& $python $exportScript --output $currentPatchPath
if ($LASTEXITCODE -ne 0) { throw "Current source patch export failed with exit code $LASTEXITCODE; partial output retained at $destination" }
$currentPatchJson = [IO.Path]::ChangeExtension($currentPatchPath, '.json')
$currentPatchHash = (Get-FileHash -LiteralPath $currentPatchPath -Algorithm SHA256).Hash.ToLowerInvariant()
$currentPatchReport = Get-Content -LiteralPath $currentPatchJson -Raw | ConvertFrom-Json
if ($currentPatchReport.patch_sha256 -ne $currentPatchHash) { throw 'Exported current source patch hash does not match its report.' }
if ($currentPatchReport.reconstruction_check -ne 'passed' -or
    $currentPatchReport.reconstruction_verified_files -ne $sourceAfter.entries.Count) {
    throw 'Current source patch was not verified by reconstructing the complete source tree.'
}
$sourceExported = Get-ProductionSourceSnapshot
if ($sourceExported.fingerprint -ne $sourceAfter.fingerprint) {
    throw 'Production source changed after the build; refusing to package a different patch.'
}
$builtFileHashes = @{}
foreach ($entry in $sourceAfter.entries) { $builtFileHashes[$entry.path] = $entry.sha256 }
foreach ($change in $currentPatchReport.changed_files) {
    $sourcePath = "upstream/klogg/$($change.path)"
    if ($change.after_sha256 -ne $builtFileHashes[$sourcePath]) {
        throw "Exported patch does not match the built source: $sourcePath"
    }
}
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

Current production source changes are represented by provenance/current-source.patch
and provenance/current-source.json. The full patch digest is $currentPatchHash.
The patch was applied in an isolated baseline copy and every reconstructed file
was checked against the production source, including its original line endings.
The independent upstream snapshot commit is $($upstream.commit); this is not a
commit identifier for the current local production source.

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
$cmakeCachePath = Join-Path $buildRoot 'CMakeCache.txt'
$cmakeCache = @{}
if (Test-Path -LiteralPath $cmakeCachePath -PathType Leaf) {
    foreach ($line in Get-Content -LiteralPath $cmakeCachePath) {
        if ($line -match '^([^:#=]+):[^=]+=([\s\S]*)$') { $cmakeCache[$Matches[1]] = $Matches[2] }
    }
}
$configureLog = Get-ChildItem -LiteralPath (Join-Path $repo '.local\logs') -Filter 'candidate-qt-configure-*.log' -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$buildLog = Get-ChildItem -LiteralPath (Join-Path $repo '.local\logs') -Filter 'candidate-qt-build-*.log' -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$compilerVersion = $cmakeCache['CMAKE_CXX_COMPILER_VERSION']
if (-not $compilerVersion) { $compilerVersion = 'Not exposed in CMakeCache; see configure/build log.' }
$manifest = [ordered]@{
    schema_version = 1
    package_kind = 'unsigned_internal_validation_package'
    variant = 'candidate'
    run_id = $RunId
    git_head = $gitState.head
    git_worktree_dirty = $gitState.dirty
    git_status = $gitState.status
    upstream_snapshot_commit = $upstream.commit
    production_source_fingerprint_sha256 = $sourceAfter.fingerprint
    production_source_files = $sourceAfter.entries
    source_unchanged_during_build_and_export = $true
    baseline_verification = $baselineVerification
    source_patch_sha256 = $currentPatchHash
    executable_sha256 = (Get-FileHash -LiteralPath (Join-Path $destination 'klogg.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
    qt_version = '6.7.3'
    qt_architecture = 'win64_msvc2019_64'
    build_configuration = [ordered]@{
        variant = 'candidate'
        backend = 'qt'
        clean_first = $true
        jobs = $Jobs
        build_directory = '.local/build-candidate'
        cmake_generator = $cmakeCache['CMAKE_GENERATOR']
        compiler_path = $cmakeCache['CMAKE_CXX_COMPILER']
        compiler_version = $compilerVersion
        qt_root = $qtRoot
        configure_log = if ($configureLog) { $configureLog.FullName.Substring($repo.Length + 1).Replace('\', '/') } else { 'Not found' }
        build_log = if ($buildLog) { $buildLog.FullName.Substring($repo.Length + 1).Replace('\', '/') } else { 'Not found' }
    }
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
