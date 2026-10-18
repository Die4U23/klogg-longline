[CmdletBinding()]
param(
    [string] $RunId = (Get-Date -Format 'yyyyMMdd-HHmmss-fff'),
    [ValidateRange(1, 8)] [int] $Jobs = 2
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$local = Join-Path $repo '.local'
$source = Join-Path $repo 'upstream\klogg'
$qt = Join-Path $local 'Qt\6.7.3\msvc2019_64'
$typeSafe = Join-Path $local 'cpm-cache\type_safe\71a1f33a7b982527e268b7782e28521750f87981'
$streamVbyte = Join-Path $local 'index-deps\streamvbyte-1.0.0'
$mimalloc = Join-Path $local 'index-deps\mimalloc-2.1.7'
$build = Join-Path $local 'build-index-asan'
$logs = Join-Path $local 'logs'
$evidence = Join-Path $local "evidence\index-only-asan\$RunId"
$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Preview\VC\Auxiliary\Build\vcvarsall.bat'
$asanRuntime = 'C:\Program Files\Microsoft Visual Studio\2022\Preview\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll'

if ($RunId -notmatch '^[A-Za-z0-9._-]+$' -or $RunId -in @('.', '..')) { throw "Unsafe run id: $RunId" }
foreach ($path in @($source, $qt, $typeSafe, $streamVbyte, $mimalloc)) {
    if (-not (Test-Path -LiteralPath $path -PathType Container)) { throw "Required source/dependency directory missing: $path" }
}
foreach ($path in @($vcvars, $asanRuntime)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "MSVC AddressSanitizer prerequisite missing: $path" }
}
if (Test-Path -LiteralPath $build) { throw "Refusing to reuse existing ASan build directory: $build" }
if (Test-Path -LiteralPath $evidence) { throw "Refusing to overwrite existing evidence: $evidence" }

$environmentLines = & $env:ComSpec /d /c "call `"$vcvars`" x64 >nul && set"
if ($LASTEXITCODE -ne 0) { throw "MSVC environment initialization failed: $vcvars x64" }
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        [Environment]::SetEnvironmentVariable($line.Substring(0, $separator), $line.Substring($separator + 1), 'Process')
    }
}
$compiler = (Get-Command cl.exe -ErrorAction Stop).Source
$cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
$ninja = (Get-Command ninja.exe -ErrorAction Stop).Source
$python = Join-Path $local 'python\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $python -PathType Leaf)) { throw "Repository Python runtime missing: $python" }
New-Item -ItemType Directory -Force -Path $logs, $evidence | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$configureLog = Join-Path $logs "index-asan-configure-$stamp.log"
$buildLog = Join-Path $logs "index-asan-build-$stamp.log"
$compilerReport = Join-Path $evidence 'compiler-version.txt'
& $compiler /Bv *> $compilerReport
$compilerExit = $LASTEXITCODE
if (-not (Select-String -LiteralPath $compilerReport -Pattern '19\.44\.35211' -Quiet)) {
    throw "MSVC compiler version output was not verified: $compilerReport"
}
Add-Content -LiteralPath $compilerReport -Value "`ncl /Bv exit code: $compilerExit (D8003 is expected without a source file)`nASan runtime: $asanRuntime"

$configArgs = @(
    '-S', (Join-Path $repo 'tests\index-only'), '-B', $build, '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$ninja", "-DKLOGG_SOURCE_ROOT=$source", "-DQT_ROOT=$qt",
    "-DTYPE_SAFE_ROOT=$typeSafe", "-DSTREAMVBYTE_ROOT=$streamVbyte", "-DMIMALLOC_ROOT=$mimalloc",
    '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF',
    '-DCMAKE_C_FLAGS=/fsanitize=address /Zi',
    '-DCMAKE_CXX_FLAGS=/fsanitize=address /Zi'
)
Write-Host "MSVC: $compiler"
Write-Host "AddressSanitizer runtime: $asanRuntime"
Write-Host "Configure log: $configureLog"
& $cmake @configArgs 2>&1 | Tee-Object -FilePath $configureLog
if ($LASTEXITCODE -ne 0) { throw "ASan index probe configure failed. See $configureLog" }

Write-Host "Build log: $buildLog"
& $cmake --build $build --parallel $Jobs *> $buildLog
$buildExitCode = $LASTEXITCODE
if ($buildExitCode -ne 0) {
    Get-Content $buildLog -Tail 50
    throw "ASan index probe build failed ($buildExitCode). See $buildLog"
}

$probe = Join-Path $build 'klogg_index_probe.exe'
if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) { throw "ASan probe executable missing: $probe" }
$env:ASAN_OPTIONS = 'halt_on_error=1'
$cases = @('high-absolute-small-span', 'uint32-max-span', 'two-to-32-span',
           'lifecycle-pop-recompress', 'lifecycle-move-append', 'range-zero',
           'lifecycle-mixed-stress')
$results = [System.Collections.Generic.List[object]]::new()
foreach ($caseName in $cases) {
    $runDirectory = Join-Path $evidence $caseName
    $runLog = Join-Path $logs "index-asan-run-$caseName-$stamp.log"
    Write-Host "Run evidence: $runDirectory"
    & $python (Join-Path $repo 'tools\run_probe.py') --exe $probe --output $runDirectory `
        --qt-root $qt --timeout 60 --qpa offscreen --compiler 'MSVC 19.44.35211; AddressSanitizer' `
        -- $caseName 2>&1 | Tee-Object -FilePath $runLog
    $runnerCode = $LASTEXITCODE
    $summaryPath = Join-Path $runDirectory 'summary.json'
    $summary = if (Test-Path -LiteralPath $summaryPath) { Get-Content $summaryPath -Raw | ConvertFrom-Json } else { $null }
    $results.Add([pscustomobject]@{ case = $caseName; runner_exit_code = $runnerCode; summary = $summary })
}
$results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $evidence 'results.json') -Encoding utf8
Write-Host "ASan results: $(Join-Path $evidence 'results.json')"
