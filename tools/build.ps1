[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('baseline', 'candidate')]
    [string] $Variant,

    [ValidateSet('qt', 'hyperscan')]
    [string] $Backend = 'qt',

    [switch] $ConfigureOnly,

    [switch] $CleanFirst,

    [string[]] $Targets = @(),

    [ValidateRange(1, 256)]
    [int] $Jobs = [Math]::Min([Environment]::ProcessorCount, 4),

    [string] $QtRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$localRoot = Join-Path $repoRoot '.local'
$sourceRoot = if ($Variant -eq 'baseline') {
    Join-Path $localRoot 'baseline\klogg-25c7de6d8f6da2ce6a00882e5af70b4f331af4c5'
} else {
    Join-Path $repoRoot 'upstream\klogg'
}
$buildName = if ($Backend -eq 'qt') { "build-$Variant" } else { "build-$Variant-$Backend" }
$buildRoot = Join-Path $localRoot $buildName
$logsRoot = Join-Path $localRoot 'logs'
$cpmCache = Join-Path $localRoot 'cpm-cache'
if (-not $QtRoot) {
    $QtRoot = Join-Path $localRoot 'Qt\6.7.3\msvc2019_64'
}

foreach ($requiredPath in @($sourceRoot, $QtRoot)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Container)) {
        throw "Required directory does not exist: $requiredPath"
    }
}
$qtConfig = Join-Path $QtRoot 'lib\cmake\Qt6\Qt6Config.cmake'
if (-not (Test-Path -LiteralPath $qtConfig -PathType Leaf)) {
    throw "Qt6 CMake package was not found at '$qtConfig'. Pass -QtRoot with a complete Qt 6.7.3 MSVC x64 installation."
}

$cmake = (Get-Command 'cmake.exe' -ErrorAction Stop).Source
$ninja = (Get-Command 'ninja.exe' -ErrorAction Stop).Source
$vcVars = 'C:\Program Files\Microsoft Visual Studio\2022\Preview\VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path -LiteralPath $vcVars -PathType Leaf)) {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        $vswhereCommand = Get-Command 'vswhere.exe' -ErrorAction SilentlyContinue
        if ($vswhereCommand) { $vswhere = $vswhereCommand.Source }
    }
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $found = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC\Auxiliary\Build\vcvarsall.bat'
        if ($LASTEXITCODE -eq 0 -and $found) { $vcVars = ($found | Select-Object -First 1).Trim() }
    }
}
if (-not (Test-Path -LiteralPath $vcVars -PathType Leaf)) {
    throw "MSVC vcvarsall.bat was not found. Checked the known Visual Studio path and vswhere discovery."
}

# Import the environment from vcvarsall into this PowerShell process. No child
# build window is launched, and no machine/user environment setting is changed.
$environmentLines = & $env:ComSpec /d /c "call `"$vcVars`" x64 >nul && set"
if ($LASTEXITCODE -ne 0) { throw "MSVC environment initialization failed: $vcVars x64" }
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        [Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }
}
# Prefer English when installed. The CMake injection also detects the actual
# output bytes because installations with only a local language ignore VSLANG.
$env:VSLANG = '1033'

# Apply only to this PowerShell process and child Git processes. Preserve any
# caller-provided GIT_CONFIG_COUNT entries, then append the required settings.
$gitConfigCount = 0
if ($env:GIT_CONFIG_COUNT) {
    $parsedCount = 0
    if (-not [int]::TryParse($env:GIT_CONFIG_COUNT, [ref]$parsedCount) -or $parsedCount -lt 0) {
        throw "GIT_CONFIG_COUNT must be a nonnegative integer; found '$($env:GIT_CONFIG_COUNT)'"
    }
    $gitConfigCount = $parsedCount
}
[Environment]::SetEnvironmentVariable("GIT_CONFIG_KEY_$gitConfigCount", 'url.https://github.com/.insteadOf', 'Process')
[Environment]::SetEnvironmentVariable("GIT_CONFIG_VALUE_$gitConfigCount", 'git@github.com:', 'Process')
$gitConfigCount++
[Environment]::SetEnvironmentVariable("GIT_CONFIG_KEY_$gitConfigCount", 'http.sslBackend', 'Process')
[Environment]::SetEnvironmentVariable("GIT_CONFIG_VALUE_$gitConfigCount", 'openssl', 'Process')
$gitConfigCount++
$env:GIT_CONFIG_COUNT = "$gitConfigCount"

New-Item -ItemType Directory -Force -Path $buildRoot, $logsRoot, $cpmCache | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$configureLog = Join-Path $logsRoot "$Variant-$Backend-configure-$stamp.log"
$buildLog = Join-Path $logsRoot "$Variant-$Backend-build-$stamp.log"
$injectFile = Join-Path $repoRoot 'tests\native\inject.cmake'

$configureArgs = @(
    '-S', $sourceRoot,
    '-B', $buildRoot,
    '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DCMAKE_PREFIX_PATH=$QtRoot",
    "-DCPM_SOURCE_CACHE=$cpmCache",
    '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
    '-DKLOGG_BUILD_TESTS=ON',
    '-DKLOGG_USE_LTO=OFF',
    '-DENABLE_ROARING_TESTS=OFF',
    '-DROARING_USE_CPM=OFF',
    '-DSIMDUTF_TESTS=OFF',
    '-DSIMDUTF_TOOLS=OFF',
    "-DKLOGG_USE_HYPERSCAN=$(if ($Backend -eq 'hyperscan') { 'ON' } else { 'OFF' })",
    '-DKLOGG_USE_VECTORSCAN=OFF'
)
if (Test-Path -LiteralPath $injectFile -PathType Leaf) {
    $configureArgs += "-DCMAKE_PROJECT_klogg_INCLUDE=$injectFile"
}
$dependencyOverrides = Join-Path $localRoot 'dependency-overrides.cmake'
if (Test-Path -LiteralPath $dependencyOverrides -PathType Leaf) {
    $configureArgs += @('-C', $dependencyOverrides)
}

Write-Host "Configuring $Variant ($Backend backend) source: $sourceRoot"
Write-Host "Build directory: $buildRoot"
Write-Host "Qt root: $QtRoot"
Write-Host "Configure log: $configureLog"
& $cmake @configureArgs 2>&1 | Tee-Object -FilePath $configureLog
$configureExitCode = $LASTEXITCODE
if ($configureExitCode -ne 0) {
    throw "CMake configure failed with exit code $configureExitCode. See $configureLog"
}
if ($ConfigureOnly) { return }

$buildArgs = @('--build', $buildRoot, '--parallel', "$Jobs")
if ($CleanFirst) { $buildArgs += '--clean-first' }
if ($Targets.Count -gt 0) {
    $buildArgs += '--target'
    $buildArgs += $Targets
}
Write-Host "Building $Variant ($Backend backend, jobs: $Jobs)"
Write-Host "Build log: $buildLog"
& $cmake @buildArgs 2>&1 | Tee-Object -FilePath $buildLog
$buildExitCode = $LASTEXITCODE
if ($buildExitCode -ne 0) {
    throw "CMake build failed with exit code $buildExitCode. See $buildLog"
}
