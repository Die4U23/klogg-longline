[CmdletBinding()]
param(
    [ValidateSet('baseline', 'candidate')] [string] $Variant = 'baseline',
    [string[]] $Case = @('high-absolute-small-span', 'uint32-max-span', 'two-to-32-span',
                        'lifecycle-pop-recompress', 'lifecycle-move-append', 'range-zero'),
    [switch] $ConfigureOnly,
    [string] $RunId = (Get-Date -Format 'yyyyMMdd-HHmmss-fff'),
    [ValidateRange(1, 10)] [int] $Repeat = 1,
    [ValidateRange(1, 32)] [int] $Jobs = [Math]::Min([Environment]::ProcessorCount, 4)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$local = Join-Path $repo '.local'
$source = if ($Variant -eq 'baseline') {
    Join-Path $local 'baseline\klogg-25c7de6d8f6da2ce6a00882e5af70b4f331af4c5'
} else {
    Join-Path $repo 'upstream\klogg'
}
$qt = Join-Path $local 'Qt\6.7.3\msvc2019_64'
$typeSafe = Join-Path $local 'cpm-cache\type_safe\71a1f33a7b982527e268b7782e28521750f87981'
$streamVbyte = Join-Path $local 'index-deps\streamvbyte-1.0.0'
$mimalloc = Join-Path $local 'index-deps\mimalloc-2.1.7'
$buildName = if ($Variant -eq 'baseline') { 'build-index-only' } else { 'build-index-only-candidate' }
$build = Join-Path $local $buildName
$logs = Join-Path $local 'logs'
$evidence = Join-Path $local "evidence\index-only-$Variant\$RunId"
if ($RunId -notmatch '^[A-Za-z0-9._-]+$' -or $RunId -in @('.', '..')) { throw "Unsafe run id: $RunId" }
$validCases = @('high-absolute-small-span', 'uint32-max-span', 'two-to-32-span',
                'lifecycle-pop-recompress', 'lifecycle-move-append', 'range-zero',
                'lifecycle-mixed-stress', 'normal-performance')
foreach ($name in $Case) {
    if ($name -notin $validCases) { throw "Unsupported case '$name'. Valid: $($validCases -join ', ')" }
}
foreach ($path in @($source, $qt, $typeSafe, $streamVbyte, $mimalloc)) {
    if (-not (Test-Path -LiteralPath $path -PathType Container)) { throw "Required source/dependency directory missing: $path" }
}

$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Preview\VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf)) { throw "MSVC vcvarsall not found: $vcvars" }
$environmentLines = & $env:ComSpec /d /c "call `"$vcvars`" x64 >nul && set"
if ($LASTEXITCODE -ne 0) { throw "MSVC environment initialization failed: $vcvars x64" }
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        [Environment]::SetEnvironmentVariable($line.Substring(0, $separator), $line.Substring($separator + 1), 'Process')
    }
}

$cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
$ninja = (Get-Command ninja.exe -ErrorAction Stop).Source
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$configureLog = Join-Path $logs "index-only-configure-$stamp.log"
$buildLog = Join-Path $logs "index-only-build-$stamp.log"
$configArgs = @(
    '-S', (Join-Path $repo 'tests\index-only'), '-B', $build, '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$ninja", "-DKLOGG_SOURCE_ROOT=$source", "-DQT_ROOT=$qt",
    "-DTYPE_SAFE_ROOT=$typeSafe", "-DSTREAMVBYTE_ROOT=$streamVbyte", "-DMIMALLOC_ROOT=$mimalloc",
    '-DCMAKE_BUILD_TYPE=RelWithDebInfo'
)
Write-Host "Configure log: $configureLog"
& $cmake @configArgs 2>&1 | Tee-Object -FilePath $configureLog
if ($LASTEXITCODE -ne 0) { throw "Index probe configure failed. See $configureLog" }
if ($ConfigureOnly) { return }
Write-Host "Build log: $buildLog"
& $cmake --build $build --parallel $Jobs *> $buildLog
$buildExitCode = $LASTEXITCODE
if ($buildExitCode -ne 0) { Get-Content $buildLog -Tail 40; throw "Index probe build failed ($buildExitCode). See $buildLog" }

$probe = Join-Path $build 'klogg_index_probe.exe'
if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) { throw "Probe executable missing: $probe" }
$runner = Join-Path $repo 'tools\run_probe.py'
$python = Join-Path $local 'python\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $python -PathType Leaf)) { throw "Repository Python runtime missing: $python" }
if (-not (Test-Path -LiteralPath $evidence)) { New-Item -ItemType Directory -Force -Path $evidence | Out-Null }
$results = [System.Collections.Generic.List[object]]::new()
foreach ($caseName in $Case) {
    for ($iteration = 1; $iteration -le $Repeat; $iteration++) {
        $runName = if ($Repeat -eq 1) { $caseName } else { "$caseName-$iteration" }
        $runDirectory = Join-Path $evidence $runName
        $runLog = Join-Path $logs "index-only-run-$runName-$stamp.log"
        Write-Host "Run evidence: $runDirectory"
        & $python $runner --exe $probe --output $runDirectory --qt-root $qt --timeout 30 --qpa offscreen --compiler 'MSVC 19.44.35211' -- $caseName 2>&1 | Tee-Object -FilePath $runLog
        $runnerCode = $LASTEXITCODE
        $summaryPath = Join-Path $runDirectory 'summary.json'
        $results.Add([pscustomobject]@{ case = $runName; probe_case = $caseName; repetition = $iteration; runner_exit_code = $runnerCode; summary = if (Test-Path $summaryPath) { (Get-Content $summaryPath -Raw | ConvertFrom-Json) } else { $null } })
    }
}
$results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $evidence "results-$stamp.json") -Encoding utf8
Write-Host "Results index: $(Join-Path $evidence "results-$stamp.json")"
